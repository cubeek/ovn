/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

#include "bitmap.h"
#include "command-line.h"
#include "daemon.h"
#include "dirs.h"
#include "openvswitch/dynamic-string.h"
#include "fatal-signal.h"
#include "hash.h"
#include "openvswitch/hmap.h"
#include "openvswitch/json.h"
#include "ovn/lex.h"
#include "lib/chassis-index.h"
#include "lib/ip-mcast-index.h"
#include "lib/mcast-group-index.h"
#include "lib/ovn-l7.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "ovn/actions.h"
#include "ovn/logical-fields.h"
#include "packets.h"
#include "openvswitch/poll-loop.h"
#include "smap.h"
#include "sset.h"
#include "svec.h"
#include "stream.h"
#include "stream-ssl.h"
#include "unixctl.h"
#include "util.h"
#include "uuid.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(ovn_northd);

static unixctl_cb_func ovn_northd_exit;
static unixctl_cb_func ovn_northd_pause;
static unixctl_cb_func ovn_northd_resume;
static unixctl_cb_func ovn_northd_is_paused;

struct northd_context {
    struct ovsdb_idl *ovnnb_idl;
    struct ovsdb_idl *ovnsb_idl;
    struct ovsdb_idl_txn *ovnnb_txn;
    struct ovsdb_idl_txn *ovnsb_txn;
    struct ovsdb_idl_index *sbrec_ha_chassis_grp_by_name;
    struct ovsdb_idl_index *sbrec_mcast_group_by_name_dp;
    struct ovsdb_idl_index *sbrec_ip_mcast_by_dp;
};

/* An IPv4 or IPv6 address */
struct v46_ip {
    int family;
    union {
        ovs_be32 ipv4;
        struct in6_addr ipv6;
    };
};

static const char *ovnnb_db;
static const char *ovnsb_db;
static const char *unixctl_path;

#define MAC_ADDR_SPACE 0xffffff

/* MAC address management (macam) table of "struct eth_addr"s, that holds the
 * MAC addresses allocated by the OVN ipam module. */
static struct hmap macam = HMAP_INITIALIZER(&macam);
static struct eth_addr mac_prefix;

static bool controller_event_en;

#define MAX_OVN_TAGS 4096

/* Pipeline stages. */

/* The two pipelines in an OVN logical flow table. */
enum ovn_pipeline {
    P_IN,                       /* Ingress pipeline. */
    P_OUT                       /* Egress pipeline. */
};

/* The two purposes for which ovn-northd uses OVN logical datapaths. */
enum ovn_datapath_type {
    DP_SWITCH,                  /* OVN logical switch. */
    DP_ROUTER                   /* OVN logical router. */
};

/* Returns an "enum ovn_stage" built from the arguments.
 *
 * (It's better to use ovn_stage_build() for type-safety reasons, but inline
 * functions can't be used in enums or switch cases.) */
#define OVN_STAGE_BUILD(DP_TYPE, PIPELINE, TABLE) \
    (((DP_TYPE) << 9) | ((PIPELINE) << 8) | (TABLE))

/* A stage within an OVN logical switch or router.
 *
 * An "enum ovn_stage" indicates whether the stage is part of a logical switch
 * or router, whether the stage is part of the ingress or egress pipeline, and
 * the table within that pipeline.  The first three components are combined to
 * form the stage's full name, e.g. S_SWITCH_IN_PORT_SEC_L2,
 * S_ROUTER_OUT_DELIVERY. */
enum ovn_stage {
#define PIPELINE_STAGES                                                   \
    /* Logical switch ingress stages. */                                  \
    PIPELINE_STAGE(SWITCH, IN,  PORT_SEC_L2,    0, "ls_in_port_sec_l2")   \
    PIPELINE_STAGE(SWITCH, IN,  PORT_SEC_IP,    1, "ls_in_port_sec_ip")   \
    PIPELINE_STAGE(SWITCH, IN,  PORT_SEC_ND,    2, "ls_in_port_sec_nd")   \
    PIPELINE_STAGE(SWITCH, IN,  PRE_ACL,        3, "ls_in_pre_acl")       \
    PIPELINE_STAGE(SWITCH, IN,  PRE_LB,         4, "ls_in_pre_lb")        \
    PIPELINE_STAGE(SWITCH, IN,  PRE_STATEFUL,   5, "ls_in_pre_stateful")  \
    PIPELINE_STAGE(SWITCH, IN,  ACL,            6, "ls_in_acl")           \
    PIPELINE_STAGE(SWITCH, IN,  QOS_MARK,       7, "ls_in_qos_mark")      \
    PIPELINE_STAGE(SWITCH, IN,  QOS_METER,      8, "ls_in_qos_meter")     \
    PIPELINE_STAGE(SWITCH, IN,  LB,             9, "ls_in_lb")            \
    PIPELINE_STAGE(SWITCH, IN,  STATEFUL,      10, "ls_in_stateful")      \
    PIPELINE_STAGE(SWITCH, IN,  ARP_ND_RSP,    11, "ls_in_arp_rsp")       \
    PIPELINE_STAGE(SWITCH, IN,  DHCP_OPTIONS,  12, "ls_in_dhcp_options")  \
    PIPELINE_STAGE(SWITCH, IN,  DHCP_RESPONSE, 13, "ls_in_dhcp_response") \
    PIPELINE_STAGE(SWITCH, IN,  DNS_LOOKUP,    14, "ls_in_dns_lookup")    \
    PIPELINE_STAGE(SWITCH, IN,  DNS_RESPONSE,  15, "ls_in_dns_response")  \
    PIPELINE_STAGE(SWITCH, IN,  EXTERNAL_PORT, 16, "ls_in_external_port") \
    PIPELINE_STAGE(SWITCH, IN,  L2_LKUP,       17, "ls_in_l2_lkup")       \
                                                                          \
    /* Logical switch egress stages. */                                   \
    PIPELINE_STAGE(SWITCH, OUT, PRE_LB,       0, "ls_out_pre_lb")         \
    PIPELINE_STAGE(SWITCH, OUT, PRE_ACL,      1, "ls_out_pre_acl")        \
    PIPELINE_STAGE(SWITCH, OUT, PRE_STATEFUL, 2, "ls_out_pre_stateful")   \
    PIPELINE_STAGE(SWITCH, OUT, LB,           3, "ls_out_lb")             \
    PIPELINE_STAGE(SWITCH, OUT, ACL,          4, "ls_out_acl")            \
    PIPELINE_STAGE(SWITCH, OUT, QOS_MARK,     5, "ls_out_qos_mark")       \
    PIPELINE_STAGE(SWITCH, OUT, QOS_METER,    6, "ls_out_qos_meter")      \
    PIPELINE_STAGE(SWITCH, OUT, STATEFUL,     7, "ls_out_stateful")       \
    PIPELINE_STAGE(SWITCH, OUT, PORT_SEC_IP,  8, "ls_out_port_sec_ip")    \
    PIPELINE_STAGE(SWITCH, OUT, PORT_SEC_L2,  9, "ls_out_port_sec_l2")    \
                                                                      \
    /* Logical router ingress stages. */                              \
    PIPELINE_STAGE(ROUTER, IN,  ADMISSION,       0, "lr_in_admission")    \
    PIPELINE_STAGE(ROUTER, IN,  LOOKUP_NEIGHBOR, 1, "lr_in_lookup_neighbor") \
    PIPELINE_STAGE(ROUTER, IN,  LEARN_NEIGHBOR,  2, "lr_in_learn_neighbor") \
    PIPELINE_STAGE(ROUTER, IN,  IP_INPUT,        3, "lr_in_ip_input")     \
    PIPELINE_STAGE(ROUTER, IN,  DEFRAG,          4, "lr_in_defrag")       \
    PIPELINE_STAGE(ROUTER, IN,  UNSNAT,          5, "lr_in_unsnat")       \
    PIPELINE_STAGE(ROUTER, IN,  DNAT,            6, "lr_in_dnat")         \
    PIPELINE_STAGE(ROUTER, IN,  ND_RA_OPTIONS,   7, "lr_in_nd_ra_options") \
    PIPELINE_STAGE(ROUTER, IN,  ND_RA_RESPONSE,  8, "lr_in_nd_ra_response") \
    PIPELINE_STAGE(ROUTER, IN,  IP_ROUTING,      9, "lr_in_ip_routing")   \
    PIPELINE_STAGE(ROUTER, IN,  POLICY,          10, "lr_in_policy")       \
    PIPELINE_STAGE(ROUTER, IN,  ARP_RESOLVE,     11, "lr_in_arp_resolve")  \
    PIPELINE_STAGE(ROUTER, IN,  CHK_PKT_LEN   ,  12, "lr_in_chk_pkt_len")   \
    PIPELINE_STAGE(ROUTER, IN,  LARGER_PKTS,     13,"lr_in_larger_pkts")   \
    PIPELINE_STAGE(ROUTER, IN,  GW_REDIRECT,     14, "lr_in_gw_redirect")  \
    PIPELINE_STAGE(ROUTER, IN,  ARP_REQUEST,     15, "lr_in_arp_request")  \
                                                                      \
    /* Logical router egress stages. */                               \
    PIPELINE_STAGE(ROUTER, OUT, UNDNAT,    0, "lr_out_undnat")        \
    PIPELINE_STAGE(ROUTER, OUT, SNAT,      1, "lr_out_snat")          \
    PIPELINE_STAGE(ROUTER, OUT, EGR_LOOP,  2, "lr_out_egr_loop")      \
    PIPELINE_STAGE(ROUTER, OUT, DELIVERY,  3, "lr_out_delivery")

#define PIPELINE_STAGE(DP_TYPE, PIPELINE, STAGE, TABLE, NAME)   \
    S_##DP_TYPE##_##PIPELINE##_##STAGE                          \
        = OVN_STAGE_BUILD(DP_##DP_TYPE, P_##PIPELINE, TABLE),
    PIPELINE_STAGES
#undef PIPELINE_STAGE
};

/* Due to various hard-coded priorities need to implement ACLs, the
 * northbound database supports a smaller range of ACL priorities than
 * are available to logical flows.  This value is added to an ACL
 * priority to determine the ACL's logical flow priority. */
#define OVN_ACL_PRI_OFFSET 1000

/* Register definitions specific to switches. */
#define REGBIT_CONNTRACK_DEFRAG  "reg0[0]"
#define REGBIT_CONNTRACK_COMMIT  "reg0[1]"
#define REGBIT_CONNTRACK_NAT     "reg0[2]"
#define REGBIT_DHCP_OPTS_RESULT  "reg0[3]"
#define REGBIT_DNS_LOOKUP_RESULT "reg0[4]"
#define REGBIT_ND_RA_OPTS_RESULT "reg0[5]"

/* Register definitions for switches and routers. */
#define REGBIT_NAT_REDIRECT     "reg9[0]"
/* Indicate that this packet has been recirculated using egress
 * loopback.  This allows certain checks to be bypassed, such as a
 * logical router dropping packets with source IP address equals
 * one of the logical router's own IP addresses. */
#define REGBIT_EGRESS_LOOPBACK  "reg9[1]"
#define REGBIT_DISTRIBUTED_NAT  "reg9[2]"
/* Register to store the result of check_pkt_larger action. */
#define REGBIT_PKT_LARGER        "reg9[3]"
#define REGBIT_LOOKUP_NEIGHBOR_RESULT "reg9[4]"
#define REGBIT_SKIP_LOOKUP_NEIGHBOR "reg9[5]"

/* Returns an "enum ovn_stage" built from the arguments. */
static enum ovn_stage
ovn_stage_build(enum ovn_datapath_type dp_type, enum ovn_pipeline pipeline,
                uint8_t table)
{
    return OVN_STAGE_BUILD(dp_type, pipeline, table);
}

/* Returns the pipeline to which 'stage' belongs. */
static enum ovn_pipeline
ovn_stage_get_pipeline(enum ovn_stage stage)
{
    return (stage >> 8) & 1;
}

/* Returns the pipeline name to which 'stage' belongs. */
static const char *
ovn_stage_get_pipeline_name(enum ovn_stage stage)
{
    return ovn_stage_get_pipeline(stage) == P_IN ? "ingress" : "egress";
}

/* Returns the table to which 'stage' belongs. */
static uint8_t
ovn_stage_get_table(enum ovn_stage stage)
{
    return stage & 0xff;
}

/* Returns a string name for 'stage'. */
static const char *
ovn_stage_to_str(enum ovn_stage stage)
{
    switch (stage) {
#define PIPELINE_STAGE(DP_TYPE, PIPELINE, STAGE, TABLE, NAME)       \
        case S_##DP_TYPE##_##PIPELINE##_##STAGE: return NAME;
    PIPELINE_STAGES
#undef PIPELINE_STAGE
        default: return "<unknown>";
    }
}

/* Returns the type of the datapath to which a flow with the given 'stage' may
 * be added. */
static enum ovn_datapath_type
ovn_stage_to_datapath_type(enum ovn_stage stage)
{
    switch (stage) {
#define PIPELINE_STAGE(DP_TYPE, PIPELINE, STAGE, TABLE, NAME)       \
        case S_##DP_TYPE##_##PIPELINE##_##STAGE: return DP_##DP_TYPE;
    PIPELINE_STAGES
#undef PIPELINE_STAGE
    default: OVS_NOT_REACHED();
    }
}

static void
usage(void)
{
    printf("\
%s: OVN northbound management daemon\n\
usage: %s [OPTIONS]\n\
\n\
Options:\n\
  --ovnnb-db=DATABASE       connect to ovn-nb database at DATABASE\n\
                            (default: %s)\n\
  --ovnsb-db=DATABASE       connect to ovn-sb database at DATABASE\n\
                            (default: %s)\n\
  --unixctl=SOCKET          override default control socket name\n\
  -h, --help                display this help message\n\
  -o, --options             list available options\n\
  -V, --version             display version information\n\
", program_name, program_name, default_nb_db(), default_sb_db());
    daemon_usage();
    vlog_usage();
    stream_usage("database", true, true, false);
}

struct tnlid_node {
    struct hmap_node hmap_node;
    uint32_t tnlid;
};

static void
destroy_tnlids(struct hmap *tnlids)
{
    struct tnlid_node *node;
    HMAP_FOR_EACH_POP (node, hmap_node, tnlids) {
        free(node);
    }
    hmap_destroy(tnlids);
}

static void
add_tnlid(struct hmap *set, uint32_t tnlid)
{
    struct tnlid_node *node = xmalloc(sizeof *node);
    hmap_insert(set, &node->hmap_node, hash_int(tnlid, 0));
    node->tnlid = tnlid;
}

static bool
tnlid_in_use(const struct hmap *set, uint32_t tnlid)
{
    const struct tnlid_node *node;
    HMAP_FOR_EACH_IN_BUCKET (node, hmap_node, hash_int(tnlid, 0), set) {
        if (node->tnlid == tnlid) {
            return true;
        }
    }
    return false;
}

static uint32_t
next_tnlid(uint32_t tnlid, uint32_t min, uint32_t max)
{
    return tnlid + 1 <= max ? tnlid + 1 : min;
}

static uint32_t
allocate_tnlid(struct hmap *set, const char *name, uint32_t min, uint32_t max,
               uint32_t *hint)
{
    for (uint32_t tnlid = next_tnlid(*hint, min, max); tnlid != *hint;
         tnlid = next_tnlid(tnlid, min, max)) {
        if (!tnlid_in_use(set, tnlid)) {
            add_tnlid(set, tnlid);
            *hint = tnlid;
            return tnlid;
        }
    }

    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
    VLOG_WARN_RL(&rl, "all %s tunnel ids exhausted", name);
    return 0;
}

struct ovn_chassis_qdisc_queues {
    struct hmap_node key_node;
    uint32_t queue_id;
    struct uuid chassis_uuid;
};

static uint32_t
hash_chassis_queue(const struct uuid *chassis_uuid, uint32_t queue_id)
{
    return hash_2words(uuid_hash(chassis_uuid), queue_id);
}

static void
destroy_chassis_queues(struct hmap *set)
{
    struct ovn_chassis_qdisc_queues *node;
    HMAP_FOR_EACH_POP (node, key_node, set) {
        free(node);
    }
    hmap_destroy(set);
}

static void
add_chassis_queue(struct hmap *set, const struct uuid *chassis_uuid,
                  uint32_t queue_id)
{
    struct ovn_chassis_qdisc_queues *node = xmalloc(sizeof *node);
    node->queue_id = queue_id;
    node->chassis_uuid = *chassis_uuid;
    hmap_insert(set, &node->key_node,
                hash_chassis_queue(chassis_uuid, queue_id));
}

static bool
chassis_queueid_in_use(const struct hmap *set, const struct uuid *chassis_uuid,
                       uint32_t queue_id)
{
    const struct ovn_chassis_qdisc_queues *node;
    HMAP_FOR_EACH_WITH_HASH (node, key_node,
                             hash_chassis_queue(chassis_uuid, queue_id), set) {
        if (uuid_equals(chassis_uuid, &node->chassis_uuid)
            && node->queue_id == queue_id) {
            return true;
        }
    }
    return false;
}

static uint32_t
allocate_chassis_queueid(struct hmap *set, const struct uuid *uuid, char *name)
{
    if (!uuid) {
        return 0;
    }

    for (uint32_t queue_id = QDISC_MIN_QUEUE_ID + 1;
         queue_id <= QDISC_MAX_QUEUE_ID;
         queue_id++) {
        if (!chassis_queueid_in_use(set, uuid, queue_id)) {
            add_chassis_queue(set, uuid, queue_id);
            return queue_id;
        }
    }

    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
    VLOG_WARN_RL(&rl, "all %s queue ids exhausted", name);
    return 0;
}

static void
free_chassis_queueid(struct hmap *set, const struct uuid *uuid,
                     uint32_t queue_id)
{
    if (!uuid) {
        return;
    }

    struct ovn_chassis_qdisc_queues *node;
    HMAP_FOR_EACH_WITH_HASH (node, key_node,
                             hash_chassis_queue(uuid, queue_id), set) {
        if (uuid_equals(uuid, &node->chassis_uuid)
            && node->queue_id == queue_id) {
            hmap_remove(set, &node->key_node);
            free(node);
            break;
        }
    }
}

static inline bool
port_has_qos_params(const struct smap *opts)
{
    return (smap_get(opts, "qos_max_rate") ||
            smap_get(opts, "qos_burst"));
}


struct ipam_info {
    uint32_t start_ipv4;
    size_t total_ipv4s;
    unsigned long *allocated_ipv4s; /* A bitmap of allocated IPv4s */
    bool ipv6_prefix_set;
    struct in6_addr ipv6_prefix;
    bool mac_only;
};

/*
 * Multicast snooping and querier per datapath configuration.
 */
struct mcast_switch_info {

    bool enabled;               /* True if snooping enabled. */
    bool querier;               /* True if querier enabled. */
    bool flood_unregistered;    /* True if unregistered multicast should be
                                 * flooded.
                                 */
    bool flood_relay;           /* True if the switch is connected to a
                                 * multicast router and unregistered multicast
                                 * should be flooded to the mrouter. Only
                                 * applicable if flood_unregistered == false.
                                 */
    bool flood_reports;         /* True if the switch has at least one port
                                 * configured to flood reports.
                                 */
    bool flood_static;          /* True if the switch has at least one port
                                 * configured to flood traffic.
                                 */
    int64_t table_size;         /* Max number of IP multicast groups. */
    int64_t idle_timeout;       /* Timeout after which an idle group is
                                 * flushed.
                                 */
    int64_t query_interval;     /* Interval between multicast queries. */
    char *eth_src;              /* ETH src address of the multicast queries. */
    char *ipv4_src;             /* IP src address of the multicast queries. */
    int64_t query_max_response; /* Expected time after which reports should
                                 * be received for queries that were sent out.
                                 */

    uint32_t active_flows;      /* Current number of active IP multicast
                                 * flows.
                                 */
};

struct mcast_router_info {
    bool relay;        /* True if the router should relay IP multicast. */
    bool flood_static; /* True if the router has at least one port configured
                        * to flood traffic.
                        */
};

struct mcast_info {

    struct hmap group_tnlids;  /* Group tunnel IDs in use on this DP. */
    uint32_t group_tnlid_hint; /* Hint for allocating next group tunnel ID. */
    struct ovs_list groups;    /* List of groups learnt on this DP. */

    union {
        struct mcast_switch_info sw;  /* Switch specific multicast info. */
        struct mcast_router_info rtr; /* Router specific multicast info. */
    };
};

struct mcast_port_info {
    bool flood;         /* True if the port should flood IP multicast traffic
                         * regardless if it's registered or not. */
    bool flood_reports; /* True if the port should flood IP multicast reports
                         * (e.g., IGMP join/leave). */
};

static void
init_mcast_port_info(struct mcast_port_info *mcast_info,
                     const struct nbrec_logical_switch_port *nbsp,
                     const struct nbrec_logical_router_port *nbrp)
{
    if (nbsp) {
        mcast_info->flood =
            smap_get_bool(&nbsp->options, "mcast_flood", false);
        mcast_info->flood_reports =
            smap_get_bool(&nbsp->options, "mcast_flood_reports",
                          false);
    } else if (nbrp) {
        /* We don't process multicast reports in any special way on logical
         * routers so just treat them as regular multicast traffic.
         */
        mcast_info->flood =
            smap_get_bool(&nbrp->options, "mcast_flood", false);
        mcast_info->flood_reports = mcast_info->flood;
    }
}

static uint32_t
ovn_mcast_group_allocate_key(struct mcast_info *mcast_info)
{
    return allocate_tnlid(&mcast_info->group_tnlids, "multicast group",
                          OVN_MIN_IP_MULTICAST, OVN_MAX_IP_MULTICAST,
                          &mcast_info->group_tnlid_hint);
}

/* The 'key' comes from nbs->header_.uuid or nbr->header_.uuid or
 * sb->external_ids:logical-switch. */
struct ovn_datapath {
    struct hmap_node key_node;  /* Index on 'key'. */
    struct uuid key;            /* (nbs/nbr)->header_.uuid. */

    const struct nbrec_logical_switch *nbs;  /* May be NULL. */
    const struct nbrec_logical_router *nbr;  /* May be NULL. */
    const struct sbrec_datapath_binding *sb; /* May be NULL. */

    struct ovs_list list;       /* In list of similar records. */

    /* Logical switch data. */
    struct ovn_port **router_ports;
    size_t n_router_ports;

    struct hmap port_tnlids;
    uint32_t port_key_hint;

    bool has_unknown;

    /* IPAM data. */
    struct ipam_info ipam_info;

    /* Multicast data. */
    struct mcast_info mcast_info;

    /* OVN northd only needs to know about the logical router gateway port for
     * NAT on a distributed router.  This "distributed gateway port" is
     * populated only when there is a "redirect-chassis" specified for one of
     * the ports on the logical router.  Otherwise this will be NULL. */
    struct ovn_port *l3dgw_port;
    /* The "derived" OVN port representing the instance of l3dgw_port on
     * the "redirect-chassis". */
    struct ovn_port *l3redirect_port;
    struct ovn_port *localnet_port;

    struct ovs_list lr_list; /* In list of logical router datapaths. */
    /* The logical router group to which this datapath belongs.
     * Valid only if it is logical router datapath. NULL otherwise. */
    struct lrouter_group *lr_group;

    /* Port groups related to the datapath, used only when nbs is NOT NULL. */
    struct hmap nb_pgs;
};

/* A group of logical router datapaths which are connected - either
 * directly or indirectly.
 * Each logical router can belong to only one group. */
struct lrouter_group {
    struct ovn_datapath **router_dps;
    int n_router_dps;
    /* Set of ha_chassis_groups which are associated with the router dps. */
    struct sset ha_chassis_groups;
};

struct macam_node {
    struct hmap_node hmap_node;
    struct eth_addr mac_addr; /* Allocated MAC address. */
};

static void
cleanup_macam(struct hmap *macam_)
{
    struct macam_node *node;
    HMAP_FOR_EACH_POP (node, hmap_node, macam_) {
        free(node);
    }
}

static struct ovn_datapath *
ovn_datapath_create(struct hmap *datapaths, const struct uuid *key,
                    const struct nbrec_logical_switch *nbs,
                    const struct nbrec_logical_router *nbr,
                    const struct sbrec_datapath_binding *sb)
{
    struct ovn_datapath *od = xzalloc(sizeof *od);
    od->key = *key;
    od->sb = sb;
    od->nbs = nbs;
    od->nbr = nbr;
    hmap_init(&od->port_tnlids);
    hmap_init(&od->nb_pgs);
    od->port_key_hint = 0;
    hmap_insert(datapaths, &od->key_node, uuid_hash(&od->key));
    od->lr_group = NULL;
    return od;
}

static void ovn_ls_port_group_destroy(struct hmap *nb_pgs);
static void destroy_mcast_info_for_datapath(struct ovn_datapath *od);

static void
ovn_datapath_destroy(struct hmap *datapaths, struct ovn_datapath *od)
{
    if (od) {
        /* Don't remove od->list.  It is used within build_datapaths() as a
         * private list and once we've exited that function it is not safe to
         * use it. */
        hmap_remove(datapaths, &od->key_node);
        destroy_tnlids(&od->port_tnlids);
        bitmap_free(od->ipam_info.allocated_ipv4s);
        free(od->router_ports);
        ovn_ls_port_group_destroy(&od->nb_pgs);
        destroy_mcast_info_for_datapath(od);

        free(od);
    }
}

/* Returns 'od''s datapath type. */
static enum ovn_datapath_type
ovn_datapath_get_type(const struct ovn_datapath *od)
{
    return od->nbs ? DP_SWITCH : DP_ROUTER;
}

static struct ovn_datapath *
ovn_datapath_find(struct hmap *datapaths, const struct uuid *uuid)
{
    struct ovn_datapath *od;

    HMAP_FOR_EACH_WITH_HASH (od, key_node, uuid_hash(uuid), datapaths) {
        if (uuid_equals(uuid, &od->key)) {
            return od;
        }
    }
    return NULL;
}

static struct ovn_datapath *
ovn_datapath_from_sbrec(struct hmap *datapaths,
                        const struct sbrec_datapath_binding *sb)
{
    struct uuid key;

    if (!smap_get_uuid(&sb->external_ids, "logical-switch", &key) &&
        !smap_get_uuid(&sb->external_ids, "logical-router", &key)) {
        return NULL;
    }
    return ovn_datapath_find(datapaths, &key);
}

static bool
lrouter_is_enabled(const struct nbrec_logical_router *lrouter)
{
    return !lrouter->enabled || *lrouter->enabled;
}

static void
init_ipam_info_for_datapath(struct ovn_datapath *od)
{
    if (!od->nbs) {
        return;
    }

    const char *subnet_str = smap_get(&od->nbs->other_config, "subnet");
    const char *ipv6_prefix = smap_get(&od->nbs->other_config, "ipv6_prefix");

    if (ipv6_prefix) {
        od->ipam_info.ipv6_prefix_set = ipv6_parse(
            ipv6_prefix, &od->ipam_info.ipv6_prefix);
    }

    if (!subnet_str) {
        if (!ipv6_prefix) {
            od->ipam_info.mac_only = smap_get_bool(&od->nbs->other_config,
                                                   "mac_only", false);
        }
        return;
    }

    ovs_be32 subnet, mask;
    char *error = ip_parse_masked(subnet_str, &subnet, &mask);
    if (error || mask == OVS_BE32_MAX || !ip_is_cidr(mask)) {
        static struct vlog_rate_limit rl
            = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "bad 'subnet' %s", subnet_str);
        free(error);
        return;
    }

    od->ipam_info.start_ipv4 = ntohl(subnet) + 1;
    od->ipam_info.total_ipv4s = ~ntohl(mask);
    od->ipam_info.allocated_ipv4s =
        bitmap_allocate(od->ipam_info.total_ipv4s);

    /* Mark first IP as taken */
    bitmap_set1(od->ipam_info.allocated_ipv4s, 0);

    /* Check if there are any reserver IPs (list) to be excluded from IPAM */
    const char *exclude_ip_list = smap_get(&od->nbs->other_config,
                                           "exclude_ips");
    if (!exclude_ip_list) {
        return;
    }

    struct lexer lexer;
    lexer_init(&lexer, exclude_ip_list);
    /* exclude_ip_list could be in the format -
    *  "10.0.0.4 10.0.0.10 10.0.0.20..10.0.0.50 10.0.0.100..10.0.0.110".
    */
    lexer_get(&lexer);
    while (lexer.token.type != LEX_T_END) {
        if (lexer.token.type != LEX_T_INTEGER) {
            lexer_syntax_error(&lexer, "expecting address");
            break;
        }
        uint32_t start = ntohl(lexer.token.value.ipv4);
        lexer_get(&lexer);

        uint32_t end = start + 1;
        if (lexer_match(&lexer, LEX_T_ELLIPSIS)) {
            if (lexer.token.type != LEX_T_INTEGER) {
                lexer_syntax_error(&lexer, "expecting address range");
                break;
            }
            end = ntohl(lexer.token.value.ipv4) + 1;
            lexer_get(&lexer);
        }

        /* Clamp start...end to fit the subnet. */
        start = MAX(od->ipam_info.start_ipv4, start);
        end = MIN(od->ipam_info.start_ipv4 + od->ipam_info.total_ipv4s, end);
        if (end > start) {
            bitmap_set_multiple(od->ipam_info.allocated_ipv4s,
                                start - od->ipam_info.start_ipv4,
                                end - start, 1);
        } else {
            lexer_error(&lexer, "excluded addresses not in subnet");
        }
    }
    if (lexer.error) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "logical switch "UUID_FMT": bad exclude_ips (%s)",
                     UUID_ARGS(&od->key), lexer.error);
    }
    lexer_destroy(&lexer);
}

static void
init_mcast_info_for_router_datapath(struct ovn_datapath *od)
{
    struct mcast_router_info *mcast_rtr_info = &od->mcast_info.rtr;

    mcast_rtr_info->relay = smap_get_bool(&od->nbr->options, "mcast_relay",
                                          false);
}

static void
init_mcast_info_for_switch_datapath(struct ovn_datapath *od)
{
    struct mcast_switch_info *mcast_sw_info = &od->mcast_info.sw;

    mcast_sw_info->enabled =
        smap_get_bool(&od->nbs->other_config, "mcast_snoop", false);
    mcast_sw_info->querier =
        smap_get_bool(&od->nbs->other_config, "mcast_querier", true);
    mcast_sw_info->flood_unregistered =
        smap_get_bool(&od->nbs->other_config, "mcast_flood_unregistered",
                      false);

    mcast_sw_info->table_size =
        smap_get_ullong(&od->nbs->other_config, "mcast_table_size",
                        OVN_MCAST_DEFAULT_MAX_ENTRIES);

    uint32_t idle_timeout =
        smap_get_ullong(&od->nbs->other_config, "mcast_idle_timeout",
                        OVN_MCAST_DEFAULT_IDLE_TIMEOUT_S);
    if (idle_timeout < OVN_MCAST_MIN_IDLE_TIMEOUT_S) {
        idle_timeout = OVN_MCAST_MIN_IDLE_TIMEOUT_S;
    } else if (idle_timeout > OVN_MCAST_MAX_IDLE_TIMEOUT_S) {
        idle_timeout = OVN_MCAST_MAX_IDLE_TIMEOUT_S;
    }
    mcast_sw_info->idle_timeout = idle_timeout;

    uint32_t query_interval =
        smap_get_ullong(&od->nbs->other_config, "mcast_query_interval",
                        mcast_sw_info->idle_timeout / 2);
    if (query_interval < OVN_MCAST_MIN_QUERY_INTERVAL_S) {
        query_interval = OVN_MCAST_MIN_QUERY_INTERVAL_S;
    } else if (query_interval > OVN_MCAST_MAX_QUERY_INTERVAL_S) {
        query_interval = OVN_MCAST_MAX_QUERY_INTERVAL_S;
    }
    mcast_sw_info->query_interval = query_interval;

    mcast_sw_info->eth_src =
        nullable_xstrdup(smap_get(&od->nbs->other_config, "mcast_eth_src"));
    mcast_sw_info->ipv4_src =
        nullable_xstrdup(smap_get(&od->nbs->other_config, "mcast_ip4_src"));

    mcast_sw_info->query_max_response =
        smap_get_ullong(&od->nbs->other_config, "mcast_query_max_response",
                        OVN_MCAST_DEFAULT_QUERY_MAX_RESPONSE_S);

    mcast_sw_info->active_flows = 0;
}

static void
init_mcast_info_for_datapath(struct ovn_datapath *od)
{
    if (!od->nbr && !od->nbs) {
        return;
    }

    hmap_init(&od->mcast_info.group_tnlids);
    od->mcast_info.group_tnlid_hint = OVN_MIN_IP_MULTICAST;
    ovs_list_init(&od->mcast_info.groups);

    if (od->nbs) {
        init_mcast_info_for_switch_datapath(od);
    } else {
        init_mcast_info_for_router_datapath(od);
    }
}

static void
destroy_mcast_info_for_switch_datapath(struct ovn_datapath *od)
{
    struct mcast_switch_info *mcast_sw_info = &od->mcast_info.sw;

    free(mcast_sw_info->eth_src);
    free(mcast_sw_info->ipv4_src);
}

static void
destroy_mcast_info_for_datapath(struct ovn_datapath *od)
{
    if (!od->nbr && !od->nbs) {
        return;
    }

    if (od->nbs) {
        destroy_mcast_info_for_switch_datapath(od);
    }

    destroy_tnlids(&od->mcast_info.group_tnlids);
}

static void
store_mcast_info_for_switch_datapath(const struct sbrec_ip_multicast *sb,
                                     struct ovn_datapath *od)
{
    struct mcast_switch_info *mcast_sw_info = &od->mcast_info.sw;

    sbrec_ip_multicast_set_datapath(sb, od->sb);
    sbrec_ip_multicast_set_enabled(sb, &mcast_sw_info->enabled, 1);
    sbrec_ip_multicast_set_querier(sb, &mcast_sw_info->querier, 1);
    sbrec_ip_multicast_set_table_size(sb, &mcast_sw_info->table_size, 1);
    sbrec_ip_multicast_set_idle_timeout(sb, &mcast_sw_info->idle_timeout, 1);
    sbrec_ip_multicast_set_query_interval(sb,
                                          &mcast_sw_info->query_interval, 1);
    sbrec_ip_multicast_set_query_max_resp(sb,
                                          &mcast_sw_info->query_max_response,
                                          1);

    if (mcast_sw_info->eth_src) {
        sbrec_ip_multicast_set_eth_src(sb, mcast_sw_info->eth_src);
    }

    if (mcast_sw_info->ipv4_src) {
        sbrec_ip_multicast_set_ip4_src(sb, mcast_sw_info->ipv4_src);
    }
}

static void
ovn_datapath_update_external_ids(struct ovn_datapath *od)
{
    /* Get the logical-switch or logical-router UUID to set in
     * external-ids. */
    char uuid_s[UUID_LEN + 1];
    sprintf(uuid_s, UUID_FMT, UUID_ARGS(&od->key));
    const char *key = od->nbs ? "logical-switch" : "logical-router";

    /* Get names to set in external-ids. */
    const char *name = od->nbs ? od->nbs->name : od->nbr->name;
    const char *name2 = (od->nbs
                         ? smap_get(&od->nbs->external_ids,
                                    "neutron:network_name")
                         : smap_get(&od->nbr->external_ids,
                                    "neutron:router_name"));

    /* Set external-ids. */
    struct smap ids = SMAP_INITIALIZER(&ids);
    smap_add(&ids, key, uuid_s);
    smap_add(&ids, "name", name);
    if (name2 && name2[0]) {
        smap_add(&ids, "name2", name2);
    }
    sbrec_datapath_binding_set_external_ids(od->sb, &ids);
    smap_destroy(&ids);
}

static void
join_datapaths(struct northd_context *ctx, struct hmap *datapaths,
               struct ovs_list *sb_only, struct ovs_list *nb_only,
               struct ovs_list *both, struct ovs_list *lr_list)
{
    ovs_list_init(sb_only);
    ovs_list_init(nb_only);
    ovs_list_init(both);

    const struct sbrec_datapath_binding *sb, *sb_next;
    SBREC_DATAPATH_BINDING_FOR_EACH_SAFE (sb, sb_next, ctx->ovnsb_idl) {
        struct uuid key;
        if (!smap_get_uuid(&sb->external_ids, "logical-switch", &key) &&
            !smap_get_uuid(&sb->external_ids, "logical-router", &key)) {
            ovsdb_idl_txn_add_comment(
                ctx->ovnsb_txn,
                "deleting Datapath_Binding "UUID_FMT" that lacks "
                "external-ids:logical-switch and "
                "external-ids:logical-router",
                UUID_ARGS(&sb->header_.uuid));
            sbrec_datapath_binding_delete(sb);
            continue;
        }

        if (ovn_datapath_find(datapaths, &key)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_INFO_RL(
                &rl, "deleting Datapath_Binding "UUID_FMT" with "
                "duplicate external-ids:logical-switch/router "UUID_FMT,
                UUID_ARGS(&sb->header_.uuid), UUID_ARGS(&key));
            sbrec_datapath_binding_delete(sb);
            continue;
        }

        struct ovn_datapath *od = ovn_datapath_create(datapaths, &key,
                                                      NULL, NULL, sb);
        ovs_list_push_back(sb_only, &od->list);
    }

    const struct nbrec_logical_switch *nbs;
    NBREC_LOGICAL_SWITCH_FOR_EACH (nbs, ctx->ovnnb_idl) {
        struct ovn_datapath *od = ovn_datapath_find(datapaths,
                                                    &nbs->header_.uuid);
        if (od) {
            od->nbs = nbs;
            ovs_list_remove(&od->list);
            ovs_list_push_back(both, &od->list);
            ovn_datapath_update_external_ids(od);
        } else {
            od = ovn_datapath_create(datapaths, &nbs->header_.uuid,
                                     nbs, NULL, NULL);
            ovs_list_push_back(nb_only, &od->list);
        }

        init_ipam_info_for_datapath(od);
        init_mcast_info_for_datapath(od);
    }

    const struct nbrec_logical_router *nbr;
    NBREC_LOGICAL_ROUTER_FOR_EACH (nbr, ctx->ovnnb_idl) {
        if (!lrouter_is_enabled(nbr)) {
            continue;
        }

        struct ovn_datapath *od = ovn_datapath_find(datapaths,
                                                    &nbr->header_.uuid);
        if (od) {
            if (!od->nbs) {
                od->nbr = nbr;
                ovs_list_remove(&od->list);
                ovs_list_push_back(both, &od->list);
                ovn_datapath_update_external_ids(od);
            } else {
                /* Can't happen! */
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_WARN_RL(&rl,
                             "duplicate UUID "UUID_FMT" in OVN_Northbound",
                             UUID_ARGS(&nbr->header_.uuid));
                continue;
            }
        } else {
            od = ovn_datapath_create(datapaths, &nbr->header_.uuid,
                                     NULL, nbr, NULL);
            ovs_list_push_back(nb_only, &od->list);
        }
        init_mcast_info_for_datapath(od);
        ovs_list_push_back(lr_list, &od->lr_list);
    }
}

static uint32_t
ovn_datapath_allocate_key(struct hmap *dp_tnlids)
{
    static uint32_t hint;
    return allocate_tnlid(dp_tnlids, "datapath", 1, (1u << 24) - 1, &hint);
}

/* Updates the southbound Datapath_Binding table so that it contains the
 * logical switches and routers specified by the northbound database.
 *
 * Initializes 'datapaths' to contain a "struct ovn_datapath" for every logical
 * switch and router. */
static void
build_datapaths(struct northd_context *ctx, struct hmap *datapaths,
                struct ovs_list *lr_list)
{
    struct ovs_list sb_only, nb_only, both;

    join_datapaths(ctx, datapaths, &sb_only, &nb_only, &both, lr_list);

    if (!ovs_list_is_empty(&nb_only)) {
        /* First index the in-use datapath tunnel IDs. */
        struct hmap dp_tnlids = HMAP_INITIALIZER(&dp_tnlids);
        struct ovn_datapath *od;
        LIST_FOR_EACH (od, list, &both) {
            add_tnlid(&dp_tnlids, od->sb->tunnel_key);
        }

        /* Add southbound record for each unmatched northbound record. */
        LIST_FOR_EACH (od, list, &nb_only) {
            uint32_t tunnel_key = ovn_datapath_allocate_key(&dp_tnlids);
            if (!tunnel_key) {
                break;
            }

            od->sb = sbrec_datapath_binding_insert(ctx->ovnsb_txn);
            ovn_datapath_update_external_ids(od);
            sbrec_datapath_binding_set_tunnel_key(od->sb, tunnel_key);
        }
        destroy_tnlids(&dp_tnlids);
    }

    /* Delete southbound records without northbound matches. */
    struct ovn_datapath *od, *next;
    LIST_FOR_EACH_SAFE (od, next, list, &sb_only) {
        ovs_list_remove(&od->list);
        sbrec_datapath_binding_delete(od->sb);
        ovn_datapath_destroy(datapaths, od);
    }
}

struct ovn_port {
    struct hmap_node key_node;  /* Index on 'key'. */
    char *key;                  /* nbs->name, nbr->name, sb->logical_port. */
    char *json_key;             /* 'key', quoted for use in JSON. */

    const struct sbrec_port_binding *sb;         /* May be NULL. */

    /* Logical switch port data. */
    const struct nbrec_logical_switch_port *nbsp; /* May be NULL. */

    struct lport_addresses *lsp_addrs;  /* Logical switch port addresses. */
    unsigned int n_lsp_addrs;

    struct lport_addresses *ps_addrs;   /* Port security addresses. */
    unsigned int n_ps_addrs;

    /* Logical router port data. */
    const struct nbrec_logical_router_port *nbrp; /* May be NULL. */

    struct lport_addresses lrp_networks;

    /* Logical port multicast data. */
    struct mcast_port_info mcast_info;

    bool derived; /* Indicates whether this is an additional port
                   * derived from nbsp or nbrp. */

    /* The port's peer:
     *
     *     - A switch port S of type "router" has a router port R as a peer,
     *       and R in turn has S has its peer.
     *
     *     - Two connected logical router ports have each other as peer. */
    struct ovn_port *peer;

    struct ovn_datapath *od;

    struct ovs_list list;       /* In list of similar records. */
};

static void
ovn_port_set_sb(struct ovn_port *op,
                const struct sbrec_port_binding *sb)
{
    op->sb = sb;
}

static void
ovn_port_set_nb(struct ovn_port *op,
                const struct nbrec_logical_switch_port *nbsp,
                const struct nbrec_logical_router_port *nbrp)
{
    op->nbsp = nbsp;
    op->nbrp = nbrp;
    init_mcast_port_info(&op->mcast_info, op->nbsp, op->nbrp);
}

static struct ovn_port *
ovn_port_create(struct hmap *ports, const char *key,
                const struct nbrec_logical_switch_port *nbsp,
                const struct nbrec_logical_router_port *nbrp,
                const struct sbrec_port_binding *sb)
{
    struct ovn_port *op = xzalloc(sizeof *op);

    struct ds json_key = DS_EMPTY_INITIALIZER;
    json_string_escape(key, &json_key);
    op->json_key = ds_steal_cstr(&json_key);

    op->key = xstrdup(key);
    ovn_port_set_sb(op, sb);
    ovn_port_set_nb(op, nbsp, nbrp);
    op->derived = false;
    hmap_insert(ports, &op->key_node, hash_string(op->key, 0));
    return op;
}

static void
ovn_port_destroy(struct hmap *ports, struct ovn_port *port)
{
    if (port) {
        /* Don't remove port->list.  It is used within build_ports() as a
         * private list and once we've exited that function it is not safe to
         * use it. */
        hmap_remove(ports, &port->key_node);

        for (int i = 0; i < port->n_lsp_addrs; i++) {
            destroy_lport_addresses(&port->lsp_addrs[i]);
        }
        free(port->lsp_addrs);

        for (int i = 0; i < port->n_ps_addrs; i++) {
            destroy_lport_addresses(&port->ps_addrs[i]);
        }
        free(port->ps_addrs);

        destroy_lport_addresses(&port->lrp_networks);
        free(port->json_key);
        free(port->key);
        free(port);
    }
}

static struct ovn_port *
ovn_port_find(const struct hmap *ports, const char *name)
{
    struct ovn_port *op;

    HMAP_FOR_EACH_WITH_HASH (op, key_node, hash_string(name, 0), ports) {
        if (!strcmp(op->key, name)) {
            return op;
        }
    }
    return NULL;
}

static uint32_t
ovn_port_allocate_key(struct ovn_datapath *od)
{
    return allocate_tnlid(&od->port_tnlids, "port",
                          1, (1u << 15) - 1, &od->port_key_hint);
}

static char *
chassis_redirect_name(const char *port_name)
{
    return xasprintf("cr-%s", port_name);
}

static bool
ipam_is_duplicate_mac(struct eth_addr *ea, uint64_t mac64, bool warn)
{
    struct macam_node *macam_node;
    HMAP_FOR_EACH_WITH_HASH (macam_node, hmap_node, hash_uint64(mac64),
                             &macam) {
        if (eth_addr_equals(*ea, macam_node->mac_addr)) {
            if (warn) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_WARN_RL(&rl, "Duplicate MAC set: "ETH_ADDR_FMT,
                             ETH_ADDR_ARGS(macam_node->mac_addr));
            }
            return true;
        }
    }
    return false;
}

static void
ipam_insert_mac(struct eth_addr *ea, bool check)
{
    if (!ea) {
        return;
    }

    uint64_t mac64 = eth_addr_to_uint64(*ea);
    uint64_t prefix = eth_addr_to_uint64(mac_prefix);

    /* If the new MAC was not assigned by this address management system or
     * check is true and the new MAC is a duplicate, do not insert it into the
     * macam hmap. */
    if (((mac64 ^ prefix) >> 24)
        || (check && ipam_is_duplicate_mac(ea, mac64, true))) {
        return;
    }

    struct macam_node *new_macam_node = xmalloc(sizeof *new_macam_node);
    new_macam_node->mac_addr = *ea;
    hmap_insert(&macam, &new_macam_node->hmap_node, hash_uint64(mac64));
}

static void
ipam_insert_ip(struct ovn_datapath *od, uint32_t ip)
{
    if (!od || !od->ipam_info.allocated_ipv4s) {
        return;
    }

    if (ip >= od->ipam_info.start_ipv4 &&
        ip < (od->ipam_info.start_ipv4 + od->ipam_info.total_ipv4s)) {
        if (bitmap_is_set(od->ipam_info.allocated_ipv4s,
                          ip - od->ipam_info.start_ipv4)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "Duplicate IP set on switch %s: "IP_FMT,
                         od->nbs->name, IP_ARGS(htonl(ip)));
        }
        bitmap_set1(od->ipam_info.allocated_ipv4s,
                    ip - od->ipam_info.start_ipv4);
    }
}

static void
ipam_insert_lsp_addresses(struct ovn_datapath *od, struct ovn_port *op,
                          char *address)
{
    if (!od || !op || !address || !strcmp(address, "unknown")
        || !strcmp(address, "router") || is_dynamic_lsp_address(address)) {
        return;
    }

    struct lport_addresses laddrs;
    if (!extract_lsp_addresses(address, &laddrs)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "Extract addresses failed.");
        return;
    }
    ipam_insert_mac(&laddrs.ea, true);

    /* IP is only added to IPAM if the switch's subnet option
     * is set, whereas MAC is always added to MACAM. */
    if (!od->ipam_info.allocated_ipv4s) {
        destroy_lport_addresses(&laddrs);
        return;
    }

    for (size_t j = 0; j < laddrs.n_ipv4_addrs; j++) {
        uint32_t ip = ntohl(laddrs.ipv4_addrs[j].addr);
        ipam_insert_ip(od, ip);
    }

    destroy_lport_addresses(&laddrs);
}

static void
ipam_add_port_addresses(struct ovn_datapath *od, struct ovn_port *op)
{
    if (!od || !op) {
        return;
    }

    if (op->nbsp) {
        /* Add all the port's addresses to address data structures. */
        for (size_t i = 0; i < op->nbsp->n_addresses; i++) {
            ipam_insert_lsp_addresses(od, op, op->nbsp->addresses[i]);
        }
    } else if (op->nbrp) {
        struct lport_addresses lrp_networks;
        if (!extract_lrp_networks(op->nbrp, &lrp_networks)) {
            static struct vlog_rate_limit rl
                = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "Extract addresses failed.");
            return;
        }
        ipam_insert_mac(&lrp_networks.ea, true);

        if (!op->peer || !op->peer->nbsp || !op->peer->od || !op->peer->od->nbs
            || !smap_get(&op->peer->od->nbs->other_config, "subnet")) {
            destroy_lport_addresses(&lrp_networks);
            return;
        }

        for (size_t i = 0; i < lrp_networks.n_ipv4_addrs; i++) {
            uint32_t ip = ntohl(lrp_networks.ipv4_addrs[i].addr);
            /* If the router has the first IP address of the subnet, don't add
             * it to IPAM. We already added this when we initialized IPAM for
             * the datapath. This will just result in an erroneous message
             * about a duplicate IP address.
             */
            if (ip != op->peer->od->ipam_info.start_ipv4) {
                ipam_insert_ip(op->peer->od, ip);
            }
        }

        destroy_lport_addresses(&lrp_networks);
    }
}

static uint64_t
ipam_get_unused_mac(ovs_be32 ip)
{
    uint32_t mac_addr_suffix, i, base_addr = ntohl(ip) & MAC_ADDR_SPACE;
    struct eth_addr mac;
    uint64_t mac64;

    for (i = 0; i < MAC_ADDR_SPACE - 1; i++) {
        /* The tentative MAC's suffix will be in the interval (1, 0xfffffe). */
        mac_addr_suffix = ((base_addr + i) % (MAC_ADDR_SPACE - 1)) + 1;
        mac64 =  eth_addr_to_uint64(mac_prefix) | mac_addr_suffix;
        eth_addr_from_uint64(mac64, &mac);
        if (!ipam_is_duplicate_mac(&mac, mac64, true)) {
            break;
        }
    }

    if (i == MAC_ADDR_SPACE) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "MAC address space exhausted.");
        mac64 = 0;
    }

    return mac64;
}

static uint32_t
ipam_get_unused_ip(struct ovn_datapath *od)
{
    if (!od || !od->ipam_info.allocated_ipv4s) {
        return 0;
    }

    size_t new_ip_index = bitmap_scan(od->ipam_info.allocated_ipv4s, 0, 0,
                                      od->ipam_info.total_ipv4s - 1);
    if (new_ip_index == od->ipam_info.total_ipv4s - 1) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL( &rl, "Subnet address space has been exhausted.");
        return 0;
    }

    return od->ipam_info.start_ipv4 + new_ip_index;
}

enum dynamic_update_type {
    NONE,    /* No change to the address */
    REMOVE,  /* Address is no longer dynamic */
    STATIC,  /* Use static address (MAC only) */
    DYNAMIC, /* Assign a new dynamic address */
};

struct dynamic_address_update {
    struct ovs_list node;       /* In build_ipam()'s list of updates. */

    struct ovn_datapath *od;
    struct ovn_port *op;

    struct lport_addresses current_addresses;
    struct eth_addr static_mac;
    ovs_be32 static_ip;
    struct in6_addr static_ipv6;
    enum dynamic_update_type mac;
    enum dynamic_update_type ipv4;
    enum dynamic_update_type ipv6;
};

static enum dynamic_update_type
dynamic_mac_changed(const char *lsp_addresses,
                    struct dynamic_address_update *update)
{
   struct eth_addr ea;

   if (ovs_scan(lsp_addresses, ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(ea))) {
       if (eth_addr_equals(ea, update->current_addresses.ea)) {
           return NONE;
       } else {
           /* MAC is still static, but it has changed */
           update->static_mac = ea;
           return STATIC;
       }
   }

   uint64_t mac64 = eth_addr_to_uint64(update->current_addresses.ea);
   uint64_t prefix = eth_addr_to_uint64(mac_prefix);

   if ((mac64 ^ prefix) >> 24) {
       return DYNAMIC;
   } else {
       return NONE;
   }
}

static enum dynamic_update_type
dynamic_ip4_changed(const char *lsp_addrs,
                    struct dynamic_address_update *update)
{
    const struct ipam_info *ipam = &update->op->od->ipam_info;
    const struct lport_addresses *cur_addresses = &update->current_addresses;
    bool dynamic_ip4 = ipam->allocated_ipv4s != NULL;

    if (!dynamic_ip4) {
        if (update->current_addresses.n_ipv4_addrs) {
            return REMOVE;
        } else {
            return NONE;
        }
    }

    if (!cur_addresses->n_ipv4_addrs) {
        /* IPv4 was previously static but now is dynamic */
        return DYNAMIC;
    }

    uint32_t ip4 = ntohl(cur_addresses->ipv4_addrs[0].addr);
    if (ip4 < ipam->start_ipv4) {
        return DYNAMIC;
    }

    uint32_t index = ip4 - ipam->start_ipv4;
    if (index > ipam->total_ipv4s ||
        bitmap_is_set(ipam->allocated_ipv4s, index)) {
        /* Previously assigned dynamic IPv4 address can no longer be used.
         * It's either outside the subnet, conflicts with an excluded IP,
         * or conflicts with a statically-assigned address on the switch
         */
        return DYNAMIC;
    } else {
        char ipv6_s[IPV6_SCAN_LEN + 1];
        ovs_be32 new_ip;
        int n = 0;

        if ((ovs_scan(lsp_addrs, "dynamic "IP_SCAN_FMT"%n",
                     IP_SCAN_ARGS(&new_ip), &n)
             && lsp_addrs[n] == '\0') ||
            (ovs_scan(lsp_addrs, "dynamic "IP_SCAN_FMT" "IPV6_SCAN_FMT"%n",
                      IP_SCAN_ARGS(&new_ip), ipv6_s, &n)
             && lsp_addrs[n] == '\0')) {
            index = ntohl(new_ip) - ipam->start_ipv4;
            if (ntohl(new_ip) < ipam->start_ipv4 ||
                index > ipam->total_ipv4s ||
                bitmap_is_set(ipam->allocated_ipv4s, index)) {
                /* new static ip is not valid */
                return DYNAMIC;
            } else if (cur_addresses->ipv4_addrs[0].addr != new_ip) {
                update->ipv4 = STATIC;
                update->static_ip = new_ip;
                return STATIC;
            }
        }
        return NONE;
    }
}

static enum dynamic_update_type
dynamic_ip6_changed(const char *lsp_addrs,
                    struct dynamic_address_update *update)
{
    bool dynamic_ip6 = update->op->od->ipam_info.ipv6_prefix_set;
    struct eth_addr ea;

    if (!dynamic_ip6) {
        if (update->current_addresses.n_ipv6_addrs) {
            /* IPv6 was dynamic but now is not */
            return REMOVE;
        } else {
            /* IPv6 has never been dynamic */
            return NONE;
        }
    }

    if (!update->current_addresses.n_ipv6_addrs ||
        ovs_scan(lsp_addrs, ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(ea))) {
        /* IPv6 was previously static but now is dynamic */
        return DYNAMIC;
    }

    const struct lport_addresses *cur_addresses;
    char ipv6_s[IPV6_SCAN_LEN + 1];
    ovs_be32 new_ip;
    int n = 0;

    if ((ovs_scan(lsp_addrs, "dynamic "IPV6_SCAN_FMT"%n",
                  ipv6_s, &n) && lsp_addrs[n] == '\0') ||
        (ovs_scan(lsp_addrs, "dynamic "IP_SCAN_FMT" "IPV6_SCAN_FMT"%n",
                  IP_SCAN_ARGS(&new_ip), ipv6_s, &n)
         && lsp_addrs[n] == '\0')) {
        struct in6_addr ipv6;

        if (!ipv6_parse(ipv6_s, &ipv6)) {
            return DYNAMIC;
        }

        struct in6_addr masked = ipv6_addr_bitand(&ipv6,
                &update->op->od->ipam_info.ipv6_prefix);
        if (!IN6_ARE_ADDR_EQUAL(&masked,
                                &update->op->od->ipam_info.ipv6_prefix)) {
            return DYNAMIC;
        }

        cur_addresses = &update->current_addresses;

        if (!IN6_ARE_ADDR_EQUAL(&cur_addresses->ipv6_addrs[0].addr,
                                &ipv6)) {
            update->static_ipv6 = ipv6;
            return STATIC;
        }
    } else if (update->mac != NONE) {
        return DYNAMIC;
    }

    return NONE;
}

/* Check previously assigned dynamic addresses for validity. This will
 * check if the assigned addresses need to change.
 *
 * Returns true if any changes to dynamic addresses are required
 */
static bool
dynamic_addresses_check_for_updates(const char *lsp_addrs,
                                    struct dynamic_address_update *update)
{
    update->mac = dynamic_mac_changed(lsp_addrs, update);
    update->ipv4 = dynamic_ip4_changed(lsp_addrs, update);
    update->ipv6 = dynamic_ip6_changed(lsp_addrs, update);
    if (update->mac == NONE &&
        update->ipv4 == NONE &&
        update->ipv6 == NONE) {
        return false;
    } else {
        return true;
    }
}

/* For addresses that do not need to be updated, go ahead and insert them
 * into IPAM. This way, their addresses will be claimed and cannot be assigned
 * elsewhere later.
 */
static void
update_unchanged_dynamic_addresses(struct dynamic_address_update *update)
{
    if (update->mac == NONE) {
        ipam_insert_mac(&update->current_addresses.ea, false);
    }
    if (update->ipv4 == NONE && update->current_addresses.n_ipv4_addrs) {
        ipam_insert_ip(update->op->od,
                       ntohl(update->current_addresses.ipv4_addrs[0].addr));
    }
}

static void
set_lsp_dynamic_addresses(const char *dynamic_addresses, struct ovn_port *op)
{
    extract_lsp_addresses(dynamic_addresses, &op->lsp_addrs[op->n_lsp_addrs]);
    op->n_lsp_addrs++;
}

/* Determines which components (MAC, IPv4, and IPv6) of dynamic
 * addresses need to be assigned. This is used exclusively for
 * ports that do not have dynamic addresses already assigned.
 */
static void
set_dynamic_updates(const char *addrspec,
                    struct dynamic_address_update *update)
{
    bool has_ipv4 = false, has_ipv6 = false;
    char ipv6_s[IPV6_SCAN_LEN + 1];
    struct eth_addr mac;
    ovs_be32 ip;
    int n = 0;
    if (ovs_scan(addrspec, ETH_ADDR_SCAN_FMT" dynamic%n",
                 ETH_ADDR_SCAN_ARGS(mac), &n)
        && addrspec[n] == '\0') {
        update->mac = STATIC;
        update->static_mac = mac;
    } else {
        update->mac = DYNAMIC;
    }

    if ((ovs_scan(addrspec, "dynamic "IP_SCAN_FMT"%n",
                 IP_SCAN_ARGS(&ip), &n) && addrspec[n] == '\0')) {
        has_ipv4 = true;
    } else if ((ovs_scan(addrspec, "dynamic "IPV6_SCAN_FMT"%n",
                         ipv6_s, &n) && addrspec[n] == '\0')) {
        has_ipv6 = true;
    } else if ((ovs_scan(addrspec, "dynamic "IP_SCAN_FMT" "IPV6_SCAN_FMT"%n",
                         IP_SCAN_ARGS(&ip), ipv6_s, &n)
               && addrspec[n] == '\0')) {
        has_ipv4 = has_ipv6 = true;
    }

    if (has_ipv4) {
        update->ipv4 = STATIC;
        update->static_ip = ip;
    } else if (update->op->od->ipam_info.allocated_ipv4s) {
        update->ipv4 = DYNAMIC;
    } else {
        update->ipv4 = NONE;
    }

    if (has_ipv6 && ipv6_parse(ipv6_s, &update->static_ipv6)) {
        update->ipv6 = STATIC;
    } else if (update->op->od->ipam_info.ipv6_prefix_set) {
        update->ipv6 = DYNAMIC;
    } else {
        update->ipv6 = NONE;
    }
}

static void
update_dynamic_addresses(struct dynamic_address_update *update)
{
    ovs_be32 ip4 = 0;
    switch (update->ipv4) {
    case NONE:
        if (update->current_addresses.n_ipv4_addrs) {
            ip4 = update->current_addresses.ipv4_addrs[0].addr;
        }
        break;
    case REMOVE:
        break;
    case STATIC:
        ip4 = update->static_ip;
        break;
    case DYNAMIC:
        ip4 = htonl(ipam_get_unused_ip(update->od));
    }

    struct eth_addr mac;
    switch (update->mac) {
    case NONE:
        mac = update->current_addresses.ea;
        break;
    case REMOVE:
        OVS_NOT_REACHED();
    case STATIC:
        mac = update->static_mac;
        break;
    case DYNAMIC:
        eth_addr_from_uint64(ipam_get_unused_mac(ip4), &mac);
        break;
    }

    struct in6_addr ip6 = in6addr_any;
    switch (update->ipv6) {
    case NONE:
        if (update->current_addresses.n_ipv6_addrs) {
            ip6 = update->current_addresses.ipv6_addrs[0].addr;
        }
        break;
    case REMOVE:
        break;
    case STATIC:
        ip6 = update->static_ipv6;
        break;
    case DYNAMIC:
        in6_generate_eui64(mac, &update->od->ipam_info.ipv6_prefix, &ip6);
        break;
    }

    struct ds new_addr = DS_EMPTY_INITIALIZER;
    ds_put_format(&new_addr, ETH_ADDR_FMT, ETH_ADDR_ARGS(mac));
    ipam_insert_mac(&mac, true);

    if (ip4) {
        ipam_insert_ip(update->od, ntohl(ip4));
        ds_put_format(&new_addr, " "IP_FMT, IP_ARGS(ip4));
    }
    if (!IN6_ARE_ADDR_EQUAL(&ip6, &in6addr_any)) {
        char ip6_s[INET6_ADDRSTRLEN + 1];
        ipv6_string_mapped(ip6_s, &ip6);
        ds_put_format(&new_addr, " %s", ip6_s);
    }
    nbrec_logical_switch_port_set_dynamic_addresses(update->op->nbsp,
                                                    ds_cstr(&new_addr));
    set_lsp_dynamic_addresses(ds_cstr(&new_addr), update->op);
    ds_destroy(&new_addr);
}

static void
build_ipam(struct hmap *datapaths, struct hmap *ports)
{
    /* IPAM generally stands for IP address management.  In non-virtualized
     * world, MAC addresses come with the hardware.  But, with virtualized
     * workloads, they need to be assigned and managed.  This function
     * does both IP address management (ipam) and MAC address management
     * (macam). */

    /* If the switch's other_config:subnet is set, allocate new addresses for
     * ports that have the "dynamic" keyword in their addresses column. */
    struct ovn_datapath *od;
    struct ovs_list updates;

    ovs_list_init(&updates);
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        for (size_t i = 0; i < od->nbs->n_ports; i++) {
            const struct nbrec_logical_switch_port *nbsp = od->nbs->ports[i];

            if (!od->ipam_info.allocated_ipv4s &&
                !od->ipam_info.ipv6_prefix_set &&
                !od->ipam_info.mac_only) {
                if (nbsp->dynamic_addresses) {
                    nbrec_logical_switch_port_set_dynamic_addresses(nbsp,
                                                                    NULL);
                }
                continue;
            }

            struct ovn_port *op = ovn_port_find(ports, nbsp->name);
            if (!op || op->nbsp != nbsp || op->peer) {
                /* Do not allocate addresses for logical switch ports that
                 * have a peer. */
                continue;
            }

            int num_dynamic_addresses = 0;
            for (size_t j = 0; j < nbsp->n_addresses; j++) {
                if (!is_dynamic_lsp_address(nbsp->addresses[j])) {
                    continue;
                }
                if (num_dynamic_addresses) {
                    static struct vlog_rate_limit rl
                        = VLOG_RATE_LIMIT_INIT(1, 1);
                    VLOG_WARN_RL(&rl, "More than one dynamic address "
                                 "configured for logical switch port '%s'",
                                 nbsp->name);
                    continue;
                }
                num_dynamic_addresses++;
                struct dynamic_address_update *update
                    = xzalloc(sizeof *update);
                update->op = op;
                update->od = od;
                if (nbsp->dynamic_addresses) {
                    bool any_changed;
                    extract_lsp_addresses(nbsp->dynamic_addresses,
                                          &update->current_addresses);
                    any_changed = dynamic_addresses_check_for_updates(
                        nbsp->addresses[j], update);
                    update_unchanged_dynamic_addresses(update);
                    if (any_changed) {
                        ovs_list_push_back(&updates, &update->node);
                    } else {
                        /* No changes to dynamic addresses */
                        set_lsp_dynamic_addresses(nbsp->dynamic_addresses, op);
                        destroy_lport_addresses(&update->current_addresses);
                        free(update);
                    }
                } else {
                    set_dynamic_updates(nbsp->addresses[j], update);
                    ovs_list_push_back(&updates, &update->node);
                }
            }

            if (!num_dynamic_addresses && nbsp->dynamic_addresses) {
                nbrec_logical_switch_port_set_dynamic_addresses(nbsp, NULL);
            }
        }

    }

    /* After retaining all unchanged dynamic addresses, now assign
     * new ones.
     */
    struct dynamic_address_update *update;
    LIST_FOR_EACH_POP (update, node, &updates) {
        update_dynamic_addresses(update);
        destroy_lport_addresses(&update->current_addresses);
        free(update);
    }
}

/* Tag allocation for nested containers.
 *
 * For a logical switch port with 'parent_name' and a request to allocate tags,
 * keeps a track of all allocated tags. */
struct tag_alloc_node {
    struct hmap_node hmap_node;
    char *parent_name;
    unsigned long *allocated_tags;  /* A bitmap to track allocated tags. */
};

static void
tag_alloc_destroy(struct hmap *tag_alloc_table)
{
    struct tag_alloc_node *node;
    HMAP_FOR_EACH_POP (node, hmap_node, tag_alloc_table) {
        bitmap_free(node->allocated_tags);
        free(node->parent_name);
        free(node);
    }
    hmap_destroy(tag_alloc_table);
}

static struct tag_alloc_node *
tag_alloc_get_node(struct hmap *tag_alloc_table, const char *parent_name)
{
    /* If a node for the 'parent_name' exists, return it. */
    struct tag_alloc_node *tag_alloc_node;
    HMAP_FOR_EACH_WITH_HASH (tag_alloc_node, hmap_node,
                             hash_string(parent_name, 0),
                             tag_alloc_table) {
        if (!strcmp(tag_alloc_node->parent_name, parent_name)) {
            return tag_alloc_node;
        }
    }

    /* Create a new node. */
    tag_alloc_node = xmalloc(sizeof *tag_alloc_node);
    tag_alloc_node->parent_name = xstrdup(parent_name);
    tag_alloc_node->allocated_tags = bitmap_allocate(MAX_OVN_TAGS);
    /* Tag 0 is invalid for nested containers. */
    bitmap_set1(tag_alloc_node->allocated_tags, 0);
    hmap_insert(tag_alloc_table, &tag_alloc_node->hmap_node,
                hash_string(parent_name, 0));

    return tag_alloc_node;
}

static void
tag_alloc_add_existing_tags(struct hmap *tag_alloc_table,
                            const struct nbrec_logical_switch_port *nbsp)
{
    /* Add the tags of already existing nested containers.  If there is no
     * 'nbsp->parent_name' or no 'nbsp->tag' set, there is nothing to do. */
    if (!nbsp->parent_name || !nbsp->parent_name[0] || !nbsp->tag) {
        return;
    }

    struct tag_alloc_node *tag_alloc_node;
    tag_alloc_node = tag_alloc_get_node(tag_alloc_table, nbsp->parent_name);
    bitmap_set1(tag_alloc_node->allocated_tags, *nbsp->tag);
}

static void
tag_alloc_create_new_tag(struct hmap *tag_alloc_table,
                         const struct nbrec_logical_switch_port *nbsp)
{
    if (!nbsp->tag_request) {
        return;
    }

    if (nbsp->parent_name && nbsp->parent_name[0]
        && *nbsp->tag_request == 0) {
        /* For nested containers that need allocation, do the allocation. */

        if (nbsp->tag) {
            /* This has already been allocated. */
            return;
        }

        struct tag_alloc_node *tag_alloc_node;
        int64_t tag;
        tag_alloc_node = tag_alloc_get_node(tag_alloc_table,
                                            nbsp->parent_name);
        tag = bitmap_scan(tag_alloc_node->allocated_tags, 0, 1, MAX_OVN_TAGS);
        if (tag == MAX_OVN_TAGS) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_ERR_RL(&rl, "out of vlans for logical switch ports with "
                        "parent %s", nbsp->parent_name);
            return;
        }
        bitmap_set1(tag_alloc_node->allocated_tags, tag);
        nbrec_logical_switch_port_set_tag(nbsp, &tag, 1);
    } else if (*nbsp->tag_request != 0) {
        /* For everything else, copy the contents of 'tag_request' to 'tag'. */
        nbrec_logical_switch_port_set_tag(nbsp, nbsp->tag_request, 1);
    }
}


static void
join_logical_ports(struct northd_context *ctx,
                   struct hmap *datapaths, struct hmap *ports,
                   struct hmap *chassis_qdisc_queues,
                   struct hmap *tag_alloc_table, struct ovs_list *sb_only,
                   struct ovs_list *nb_only, struct ovs_list *both)
{
    ovs_list_init(sb_only);
    ovs_list_init(nb_only);
    ovs_list_init(both);

    const struct sbrec_port_binding *sb;
    SBREC_PORT_BINDING_FOR_EACH (sb, ctx->ovnsb_idl) {
        struct ovn_port *op = ovn_port_create(ports, sb->logical_port,
                                              NULL, NULL, sb);
        ovs_list_push_back(sb_only, &op->list);
    }

    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (od->nbs) {
            for (size_t i = 0; i < od->nbs->n_ports; i++) {
                const struct nbrec_logical_switch_port *nbsp
                    = od->nbs->ports[i];
                struct ovn_port *op = ovn_port_find(ports, nbsp->name);
                if (op) {
                    if (op->nbsp || op->nbrp) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(5, 1);
                        VLOG_WARN_RL(&rl, "duplicate logical port %s",
                                     nbsp->name);
                        continue;
                    }
                    ovn_port_set_nb(op, nbsp, NULL);
                    ovs_list_remove(&op->list);

                    uint32_t queue_id = smap_get_int(&op->sb->options,
                                                     "qdisc_queue_id", 0);
                    if (queue_id && op->sb->chassis) {
                        add_chassis_queue(
                             chassis_qdisc_queues, &op->sb->chassis->header_.uuid,
                             queue_id);
                    }

                    ovs_list_push_back(both, &op->list);

                    /* This port exists due to a SB binding, but should
                     * not have been initialized fully. */
                    ovs_assert(!op->n_lsp_addrs && !op->n_ps_addrs);
                } else {
                    op = ovn_port_create(ports, nbsp->name, nbsp, NULL, NULL);
                    ovs_list_push_back(nb_only, &op->list);
                }

                if (!strcmp(nbsp->type, "localnet")) {
                   od->localnet_port = op;
                }

                op->lsp_addrs
                    = xmalloc(sizeof *op->lsp_addrs * nbsp->n_addresses);
                for (size_t j = 0; j < nbsp->n_addresses; j++) {
                    if (!strcmp(nbsp->addresses[j], "unknown")
                        || !strcmp(nbsp->addresses[j], "router")) {
                        continue;
                    }
                    if (is_dynamic_lsp_address(nbsp->addresses[j])) {
                        continue;
                    } else if (!extract_lsp_addresses(nbsp->addresses[j],
                                           &op->lsp_addrs[op->n_lsp_addrs])) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(1, 1);
                        VLOG_INFO_RL(&rl, "invalid syntax '%s' in logical "
                                          "switch port addresses. No MAC "
                                          "address found",
                                          op->nbsp->addresses[j]);
                        continue;
                    }
                    op->n_lsp_addrs++;
                }

                op->ps_addrs
                    = xmalloc(sizeof *op->ps_addrs * nbsp->n_port_security);
                for (size_t j = 0; j < nbsp->n_port_security; j++) {
                    if (!extract_lsp_addresses(nbsp->port_security[j],
                                               &op->ps_addrs[op->n_ps_addrs])) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(1, 1);
                        VLOG_INFO_RL(&rl, "invalid syntax '%s' in port "
                                          "security. No MAC address found",
                                          op->nbsp->port_security[j]);
                        continue;
                    }
                    op->n_ps_addrs++;
                }

                op->od = od;
                tag_alloc_add_existing_tags(tag_alloc_table, nbsp);
            }
        } else {
            for (size_t i = 0; i < od->nbr->n_ports; i++) {
                const struct nbrec_logical_router_port *nbrp
                    = od->nbr->ports[i];

                struct lport_addresses lrp_networks;
                if (!extract_lrp_networks(nbrp, &lrp_networks)) {
                    static struct vlog_rate_limit rl
                        = VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad 'mac' %s", nbrp->mac);
                    continue;
                }

                if (!lrp_networks.n_ipv4_addrs && !lrp_networks.n_ipv6_addrs) {
                    continue;
                }

                struct ovn_port *op = ovn_port_find(ports, nbrp->name);
                if (op) {
                    if (op->nbsp || op->nbrp) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(5, 1);
                        VLOG_WARN_RL(&rl, "duplicate logical router port %s",
                                     nbrp->name);
                        continue;
                    }
                    ovn_port_set_nb(op, NULL, nbrp);
                    ovs_list_remove(&op->list);
                    ovs_list_push_back(both, &op->list);

                    /* This port exists but should not have been
                     * initialized fully. */
                    ovs_assert(!op->lrp_networks.n_ipv4_addrs
                               && !op->lrp_networks.n_ipv6_addrs);
                } else {
                    op = ovn_port_create(ports, nbrp->name, NULL, nbrp, NULL);
                    ovs_list_push_back(nb_only, &op->list);
                }

                op->lrp_networks = lrp_networks;
                op->od = od;

                const char *redirect_chassis = smap_get(&op->nbrp->options,
                                                        "redirect-chassis");
                if (op->nbrp->ha_chassis_group || redirect_chassis ||
                    op->nbrp->n_gateway_chassis) {
                    /* Additional "derived" ovn_port crp represents the
                     * instance of op on the "redirect-chassis". */
                    const char *gw_chassis = smap_get(&op->od->nbr->options,
                                                   "chassis");
                    if (gw_chassis) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(1, 1);
                        VLOG_WARN_RL(&rl, "Bad configuration: "
                                     "redirect-chassis configured on port %s "
                                     "on L3 gateway router", nbrp->name);
                        continue;
                    }
                    if (od->l3dgw_port || od->l3redirect_port) {
                        static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(1, 1);
                        VLOG_WARN_RL(&rl, "Bad configuration: multiple ports "
                                     "with redirect-chassis on same logical "
                                     "router %s", od->nbr->name);
                        continue;
                    }

                    char *redirect_name = chassis_redirect_name(nbrp->name);
                    struct ovn_port *crp = ovn_port_find(ports, redirect_name);
                    if (crp) {
                        crp->derived = true;
                        ovn_port_set_nb(crp, NULL, nbrp);
                        ovs_list_remove(&crp->list);
                        ovs_list_push_back(both, &crp->list);
                    } else {
                        crp = ovn_port_create(ports, redirect_name,
                                              NULL, nbrp, NULL);
                        crp->derived = true;
                        ovs_list_push_back(nb_only, &crp->list);
                    }
                    crp->od = od;
                    free(redirect_name);

                    /* Set l3dgw_port and l3redirect_port in od, for later
                     * use during flow creation. */
                    od->l3dgw_port = op;
                    od->l3redirect_port = crp;
                }
            }
        }
    }

    /* Connect logical router ports, and logical switch ports of type "router",
     * to their peers. */
    struct ovn_port *op;
    HMAP_FOR_EACH (op, key_node, ports) {
        if (op->nbsp && !strcmp(op->nbsp->type, "router") && !op->derived) {
            const char *peer_name = smap_get(&op->nbsp->options, "router-port");
            if (!peer_name) {
                continue;
            }

            struct ovn_port *peer = ovn_port_find(ports, peer_name);
            if (!peer || !peer->nbrp) {
                continue;
            }

            peer->peer = op;
            op->peer = peer;
            op->od->router_ports = xrealloc(
                op->od->router_ports,
                sizeof *op->od->router_ports * (op->od->n_router_ports + 1));
            op->od->router_ports[op->od->n_router_ports++] = op;

            /* Fill op->lsp_addrs for op->nbsp->addresses[] with
             * contents "router", which was skipped in the loop above. */
            for (size_t j = 0; j < op->nbsp->n_addresses; j++) {
                if (!strcmp(op->nbsp->addresses[j], "router")) {
                    if (extract_lrp_networks(peer->nbrp,
                                            &op->lsp_addrs[op->n_lsp_addrs])) {
                        op->n_lsp_addrs++;
                    }
                    break;
                }
            }

            /* If the router is multicast enabled then set relay on the switch
             * datapath.
             */
            if (peer->od && peer->od->mcast_info.rtr.relay) {
                op->od->mcast_info.sw.flood_relay = true;
            }
        } else if (op->nbrp && op->nbrp->peer && !op->derived) {
            struct ovn_port *peer = ovn_port_find(ports, op->nbrp->peer);
            if (peer) {
                if (peer->nbrp) {
                    op->peer = peer;
                } else if (peer->nbsp) {
                    /* An ovn_port for a switch port of type "router" does have
                     * a router port as its peer (see the case above for
                     * "router" ports), but this is set via options:router-port
                     * in Logical_Switch_Port and does not involve the
                     * Logical_Router_Port's 'peer' column. */
                    static struct vlog_rate_limit rl =
                            VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "Bad configuration: The peer of router "
                                 "port %s is a switch port", op->key);
                }
            }
        }
    }

    /* Wait until all ports have been connected to add to IPAM since
     * it relies on proper peers to be set
     */
    HMAP_FOR_EACH (op, key_node, ports) {
        ipam_add_port_addresses(op->od, op);
    }
}

static void
ip_address_and_port_from_lb_key(const char *key, char **ip_address,
                                uint16_t *port, int *addr_family);

static void
get_router_load_balancer_ips(const struct ovn_datapath *od,
                             struct sset *all_ips, int *addr_family)
{
    if (!od->nbr) {
        return;
    }

    for (int i = 0; i < od->nbr->n_load_balancer; i++) {
        struct nbrec_load_balancer *lb = od->nbr->load_balancer[i];
        struct smap *vips = &lb->vips;
        struct smap_node *node;

        SMAP_FOR_EACH (node, vips) {
            /* node->key contains IP:port or just IP. */
            char *ip_address = NULL;
            uint16_t port;

            ip_address_and_port_from_lb_key(node->key, &ip_address, &port,
                                            addr_family);
            if (!ip_address) {
                continue;
            }

            if (!sset_contains(all_ips, ip_address)) {
                sset_add(all_ips, ip_address);
            }

            free(ip_address);
        }
    }
}

/* Returns an array of strings, each consisting of a MAC address followed
 * by one or more IP addresses, and if the port is a distributed gateway
 * port, followed by 'is_chassis_resident("LPORT_NAME")', where the
 * LPORT_NAME is the name of the L3 redirect port or the name of the
 * logical_port specified in a NAT rule.  These strings include the
 * external IP addresses of all NAT rules defined on that router, and all
 * of the IP addresses used in load balancer VIPs defined on that router.
 *
 * The caller must free each of the n returned strings with free(),
 * and must free the returned array when it is no longer needed. */
static char **
get_nat_addresses(const struct ovn_port *op, size_t *n)
{
    size_t n_nats = 0;
    struct eth_addr mac;
    if (!op->nbrp || !op->od || !op->od->nbr
        || (!op->od->nbr->n_nat && !op->od->nbr->n_load_balancer)
        || !eth_addr_from_string(op->nbrp->mac, &mac)) {
        *n = n_nats;
        return NULL;
    }

    struct ds c_addresses = DS_EMPTY_INITIALIZER;
    ds_put_format(&c_addresses, ETH_ADDR_FMT, ETH_ADDR_ARGS(mac));
    bool central_ip_address = false;

    char **addresses;
    addresses = xmalloc(sizeof *addresses * (op->od->nbr->n_nat + 1));

    /* Get NAT IP addresses. */
    for (size_t i = 0; i < op->od->nbr->n_nat; i++) {
        const struct nbrec_nat *nat = op->od->nbr->nat[i];
        ovs_be32 ip, mask;

        char *error = ip_parse_masked(nat->external_ip, &ip, &mask);
        if (error || mask != OVS_BE32_MAX) {
            free(error);
            continue;
        }

        /* Determine whether this NAT rule satisfies the conditions for
         * distributed NAT processing. */
        if (op->od->l3redirect_port && !strcmp(nat->type, "dnat_and_snat")
            && nat->logical_port && nat->external_mac) {
            /* Distributed NAT rule. */
            if (eth_addr_from_string(nat->external_mac, &mac)) {
                struct ds address = DS_EMPTY_INITIALIZER;
                ds_put_format(&address, ETH_ADDR_FMT, ETH_ADDR_ARGS(mac));
                ds_put_format(&address, " %s", nat->external_ip);
                ds_put_format(&address, " is_chassis_resident(\"%s\")",
                              nat->logical_port);
                addresses[n_nats++] = ds_steal_cstr(&address);
            }
        } else {
            /* Centralized NAT rule, either on gateway router or distributed
             * router.
             * Check if external_ip is same as router ip. If so, then there
             * is no need to add this to the nat_addresses. The router IPs
             * will be added separately. */
            bool is_router_ip = false;
            for (size_t j = 0; j < op->lrp_networks.n_ipv4_addrs; j++) {
                if (!strcmp(nat->external_ip,
                            op->lrp_networks.ipv4_addrs[j].addr_s)) {
                    is_router_ip = true;
                    break;
                }
            }
            if (!is_router_ip) {
                for (size_t j = 0; j < op->lrp_networks.n_ipv6_addrs; j++) {
                    if (!strcmp(nat->external_ip,
                                op->lrp_networks.ipv6_addrs[j].addr_s)) {
                        is_router_ip = true;
                        break;
                    }
                }
            }

            if (!is_router_ip) {
                ds_put_format(&c_addresses, " %s", nat->external_ip);
                central_ip_address = true;
            }
        }
    }

    /* A set to hold all load-balancer vips. */
    struct sset all_ips = SSET_INITIALIZER(&all_ips);
    int addr_family;
    get_router_load_balancer_ips(op->od, &all_ips, &addr_family);

    const char *ip_address;
    SSET_FOR_EACH (ip_address, &all_ips) {
        ds_put_format(&c_addresses, " %s", ip_address);
        central_ip_address = true;
    }
    sset_destroy(&all_ips);

    if (central_ip_address) {
        /* Gratuitous ARP for centralized NAT rules on distributed gateway
         * ports should be restricted to the "redirect-chassis". */
        if (op->od->l3redirect_port) {
            ds_put_format(&c_addresses, " is_chassis_resident(%s)",
                          op->od->l3redirect_port->json_key);
        }

        addresses[n_nats++] = ds_steal_cstr(&c_addresses);
    }

    *n = n_nats;

    return addresses;
}

static bool
sbpb_gw_chassis_needs_update(
    const struct sbrec_port_binding *pb,
    const struct nbrec_logical_router_port *lrp,
    struct ovsdb_idl_index *sbrec_chassis_by_name)
{
    if (!lrp || !pb) {
        return false;
    }

    if (lrp->n_gateway_chassis && !pb->ha_chassis_group) {
        /* If there are gateway chassis in the NB DB, but there is
         * no corresponding HA chassis group in SB DB we need to
         * create the HA chassis group in SB DB for this lrp. */
        return true;
    }

    if (strcmp(pb->ha_chassis_group->name, lrp->name)) {
        /* Name doesn't match. */
        return true;
    }

    if (lrp->n_gateway_chassis != pb->ha_chassis_group->n_ha_chassis) {
        return true;
    }

    for (size_t i = 0; i < lrp->n_gateway_chassis; i++) {
        struct nbrec_gateway_chassis *nbgw_ch = lrp->gateway_chassis[i];
        bool found = false;
        for (size_t j = 0; j < pb->ha_chassis_group->n_ha_chassis; j++) {
            struct sbrec_ha_chassis *sbha_ch =
                pb->ha_chassis_group->ha_chassis[j];
            const char *chassis_name = smap_get(&sbha_ch->external_ids,
                                                "chassis-name");
            if (!chassis_name) {
                return true;
            }

            if (strcmp(chassis_name, nbgw_ch->chassis_name)) {
                continue;
            }

            found = true;

            if (nbgw_ch->priority != sbha_ch->priority) {
                return true;
            }

            if (sbha_ch->chassis &&
                strcmp(nbgw_ch->chassis_name, sbha_ch->chassis->name)) {
                /* sbha_ch->chassis's name is different from the one
                 * in sbha_ch->external_ids:chassis-name. */
                return true;
            }

            if (!sbha_ch->chassis &&
                chassis_lookup_by_name(sbrec_chassis_by_name,
                                       nbgw_ch->chassis_name)) {
                /* sbha_ch->chassis is NULL, but the chassis is
                 * present in Chassis table. */
                return true;
            }
        }

        if (!found) {
            return true;
        }
    }

    /* No need to update SB DB. Its in sync. */
    return false;
}

static struct sbrec_ha_chassis *
create_sb_ha_chassis(struct northd_context *ctx,
                     const struct sbrec_chassis *chassis,
                     const char *chassis_name, int priority)
{
    struct sbrec_ha_chassis *sb_ha_chassis =
        sbrec_ha_chassis_insert(ctx->ovnsb_txn);
    sbrec_ha_chassis_set_chassis(sb_ha_chassis, chassis);
    sbrec_ha_chassis_set_priority(sb_ha_chassis, priority);
    /* Store the chassis_name in external_ids. If the chassis
     * entry doesn't exist in the Chassis table then we can
     * figure out the chassis to which this ha_chassis
     * maps to. */
    const struct smap external_ids =
        SMAP_CONST1(&external_ids, "chassis-name", chassis_name);
    sbrec_ha_chassis_set_external_ids(sb_ha_chassis, &external_ids);
    return sb_ha_chassis;
}

static bool
chassis_group_list_changed(
    const struct nbrec_ha_chassis_group *nb_ha_grp,
    const struct sbrec_ha_chassis_group *sb_ha_grp,
    struct ovsdb_idl_index *sbrec_chassis_by_name)
{
    if (nb_ha_grp->n_ha_chassis != sb_ha_grp->n_ha_chassis) {
        return true;
    }

    struct shash nb_ha_chassis_list = SHASH_INITIALIZER(&nb_ha_chassis_list);
    for (size_t i = 0; i < nb_ha_grp->n_ha_chassis; i++) {
        shash_add(&nb_ha_chassis_list,
                  nb_ha_grp->ha_chassis[i]->chassis_name,
                  nb_ha_grp->ha_chassis[i]);
    }

    bool changed = false;
    const struct sbrec_ha_chassis *sb_ha_chassis;
    const struct nbrec_ha_chassis *nb_ha_chassis;
    for (size_t i = 0; i < sb_ha_grp->n_ha_chassis; i++) {
        sb_ha_chassis = sb_ha_grp->ha_chassis[i];
        const char *chassis_name = smap_get(&sb_ha_chassis->external_ids,
                                            "chassis-name");

        if (!chassis_name) {
            changed = true;
            break;
        }

        nb_ha_chassis = shash_find_and_delete(&nb_ha_chassis_list,
                                              chassis_name);
        if (!nb_ha_chassis ||
            nb_ha_chassis->priority != sb_ha_chassis->priority) {
            changed = true;
            break;
        }

        if (sb_ha_chassis->chassis &&
            strcmp(sb_ha_chassis->chassis->name, chassis_name)) {
            /* sb_ha_chassis->chassis's name is different from the one
             * in sb_ha_chassis->external_ids:chassis-name. */
            changed = true;
            break;
        }

        if (!sb_ha_chassis->chassis &&
            chassis_lookup_by_name(sbrec_chassis_by_name,
                                   chassis_name)) {
            /* sb_ha_chassis->chassis is NULL, but the chassis is
             * present in Chassis table. */
            changed = true;
            break;
        }
    }

    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, &nb_ha_chassis_list) {
        shash_delete(&nb_ha_chassis_list, node);
        changed = true;
    }
    shash_destroy(&nb_ha_chassis_list);

    return changed;
}

static void
sync_ha_chassis_group_for_sbpb(struct northd_context *ctx,
                               const struct nbrec_ha_chassis_group *nb_ha_grp,
                               struct ovsdb_idl_index *sbrec_chassis_by_name,
                               const struct sbrec_port_binding *pb)
{
    bool new_sb_chassis_group = false;
    const struct sbrec_ha_chassis_group *sb_ha_grp =
        ha_chassis_group_lookup_by_name(
            ctx->sbrec_ha_chassis_grp_by_name, nb_ha_grp->name);

    if (!sb_ha_grp) {
        sb_ha_grp = sbrec_ha_chassis_group_insert(ctx->ovnsb_txn);
        sbrec_ha_chassis_group_set_name(sb_ha_grp, nb_ha_grp->name);
        new_sb_chassis_group = true;
    }

    if (new_sb_chassis_group ||
        chassis_group_list_changed(nb_ha_grp, sb_ha_grp,
                                   sbrec_chassis_by_name)) {
        struct sbrec_ha_chassis **sb_ha_chassis = NULL;
        size_t n_ha_chassis = nb_ha_grp->n_ha_chassis;
        sb_ha_chassis = xcalloc(n_ha_chassis, sizeof *sb_ha_chassis);
        for (size_t i = 0; i < nb_ha_grp->n_ha_chassis; i++) {
            const struct nbrec_ha_chassis *nb_ha_chassis
                = nb_ha_grp->ha_chassis[i];
            const struct sbrec_chassis *chassis =
                chassis_lookup_by_name(sbrec_chassis_by_name,
                                       nb_ha_chassis->chassis_name);
            sb_ha_chassis[i] = sbrec_ha_chassis_insert(ctx->ovnsb_txn);
            /* It's perfectly ok if the chassis is NULL. This could
             * happen when ovn-controller exits and removes its row
             * from the chassis table in OVN SB DB. */
            sbrec_ha_chassis_set_chassis(sb_ha_chassis[i], chassis);
            sbrec_ha_chassis_set_priority(sb_ha_chassis[i],
                                          nb_ha_chassis->priority);
            const struct smap external_ids =
                SMAP_CONST1(&external_ids, "chassis-name",
                            nb_ha_chassis->chassis_name);
            sbrec_ha_chassis_set_external_ids(sb_ha_chassis[i], &external_ids);
        }
        sbrec_ha_chassis_group_set_ha_chassis(sb_ha_grp, sb_ha_chassis,
                                              n_ha_chassis);
        free(sb_ha_chassis);
    }

    sbrec_port_binding_set_ha_chassis_group(pb, sb_ha_grp);
}

/* This functions translates the gw chassis on the nb database
 * to HA chassis group in the sb database entries.
 */
static void
copy_gw_chassis_from_nbrp_to_sbpb(
        struct northd_context *ctx,
        struct ovsdb_idl_index *sbrec_chassis_by_name,
        const struct nbrec_logical_router_port *lrp,
        const struct sbrec_port_binding *port_binding)
{

    /* Make use of the new HA chassis group table to support HA
     * for the distributed gateway router port. */
    const struct sbrec_ha_chassis_group *sb_ha_chassis_group =
        ha_chassis_group_lookup_by_name(
            ctx->sbrec_ha_chassis_grp_by_name, lrp->name);
    if (!sb_ha_chassis_group) {
        sb_ha_chassis_group = sbrec_ha_chassis_group_insert(ctx->ovnsb_txn);
        sbrec_ha_chassis_group_set_name(sb_ha_chassis_group, lrp->name);
    }

    struct sbrec_ha_chassis **sb_ha_chassis = xcalloc(lrp->n_gateway_chassis,
                                                      sizeof *sb_ha_chassis);
    size_t n_sb_ha_ch = 0;
    for (size_t n = 0; n < lrp->n_gateway_chassis; n++) {
        struct nbrec_gateway_chassis *lrp_gwc = lrp->gateway_chassis[n];
        if (!lrp_gwc->chassis_name) {
            continue;
        }

        const struct sbrec_chassis *chassis =
            chassis_lookup_by_name(sbrec_chassis_by_name,
                                   lrp_gwc->chassis_name);

        sb_ha_chassis[n_sb_ha_ch] =
            create_sb_ha_chassis(ctx, chassis, lrp_gwc->chassis_name,
                                 lrp_gwc->priority);
        n_sb_ha_ch++;
    }

    sbrec_ha_chassis_group_set_ha_chassis(sb_ha_chassis_group,
                                          sb_ha_chassis, n_sb_ha_ch);
    sbrec_port_binding_set_ha_chassis_group(port_binding, sb_ha_chassis_group);
    free(sb_ha_chassis);
}

static void
ovn_port_update_sbrec(struct northd_context *ctx,
                      struct ovsdb_idl_index *sbrec_chassis_by_name,
                      const struct ovn_port *op,
                      struct hmap *chassis_qdisc_queues,
                      struct sset *active_ha_chassis_grps)
{
    sbrec_port_binding_set_datapath(op->sb, op->od->sb);
    if (op->nbrp) {
        /* If the router is for l3 gateway, it resides on a chassis
         * and its port type is "l3gateway". */
        const char *chassis_name = smap_get(&op->od->nbr->options, "chassis");
        if (op->derived) {
            sbrec_port_binding_set_type(op->sb, "chassisredirect");
        } else if (chassis_name) {
            sbrec_port_binding_set_type(op->sb, "l3gateway");
        } else {
            sbrec_port_binding_set_type(op->sb, "patch");
        }

        struct smap new;
        smap_init(&new);
        if (op->derived) {
            const char *redirect_chassis = smap_get(&op->nbrp->options,
                                                    "redirect-chassis");
            const char *redirect_type = smap_get(&op->nbrp->options,
                                                 "redirect-type");

            int n_gw_options_set = 0;
            if (op->nbrp->ha_chassis_group) {
                n_gw_options_set++;
            }
            if (op->nbrp->n_gateway_chassis) {
                n_gw_options_set++;
            }
            if (redirect_chassis) {
                n_gw_options_set++;
            }
            if (n_gw_options_set > 1) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_WARN_RL(
                    &rl, "Multiple gatway options set for the logical router "
                         "port %s. The first preferred option is "
                         "ha_chassis_group; the second is gateway_chassis; "
                         "and the last is redirect-chassis.", op->nbrp->name);
            }

            if (op->nbrp->ha_chassis_group) {
                /* HA Chassis group is set. Ignore 'gateway_chassis'
                 * column and redirect-chassis option. */
                sync_ha_chassis_group_for_sbpb(ctx, op->nbrp->ha_chassis_group,
                                               sbrec_chassis_by_name, op->sb);
                sset_add(active_ha_chassis_grps,
                         op->nbrp->ha_chassis_group->name);
            } else if (op->nbrp->n_gateway_chassis) {
                /* Legacy gateway_chassis support.
                 * Create ha_chassis_group for the Northbound gateway_chassis
                 * associated with the lrp. */
                if (sbpb_gw_chassis_needs_update(op->sb, op->nbrp,
                                                 sbrec_chassis_by_name)) {
                    copy_gw_chassis_from_nbrp_to_sbpb(ctx,
                                                      sbrec_chassis_by_name,
                                                      op->nbrp, op->sb);
                }

                sset_add(active_ha_chassis_grps, op->nbrp->name);
            } else if (redirect_chassis) {
                /* Handle ports that had redirect-chassis option attached
                 * to them, and for backwards compatibility convert them
                 * to a single HA Chassis group entry */
                const struct sbrec_chassis *chassis =
                    chassis_lookup_by_name(sbrec_chassis_by_name,
                                           redirect_chassis);
                if (chassis) {
                    /* If we found the chassis, and the gw chassis on record
                     * differs from what we expect go ahead and update */
                    char *gwc_name = xasprintf("%s_%s", op->nbrp->name,
                                chassis->name);
                    const struct sbrec_ha_chassis_group *sb_ha_ch_grp;
                    sb_ha_ch_grp = ha_chassis_group_lookup_by_name(
                        ctx->sbrec_ha_chassis_grp_by_name, gwc_name);
                    if (!sb_ha_ch_grp) {
                        sb_ha_ch_grp =
                            sbrec_ha_chassis_group_insert(ctx->ovnsb_txn);
                        sbrec_ha_chassis_group_set_name(sb_ha_ch_grp,
                                                        gwc_name);
                    }

                    if (sb_ha_ch_grp->n_ha_chassis != 1) {
                        struct sbrec_ha_chassis *sb_ha_ch =
                            create_sb_ha_chassis(ctx, chassis,
                                                 chassis->name, 0);
                        sbrec_ha_chassis_group_set_ha_chassis(sb_ha_ch_grp,
                                                              &sb_ha_ch, 1);
                    }
                    sbrec_port_binding_set_ha_chassis_group(op->sb,
                                                            sb_ha_ch_grp);
                    sset_add(active_ha_chassis_grps, gwc_name);
                    free(gwc_name);
                } else {
                    VLOG_WARN("chassis name '%s' from redirect from logical "
                              " router port '%s' redirect-chassis not found",
                              redirect_chassis, op->nbrp->name);
                    if (op->sb->ha_chassis_group) {
                        sbrec_port_binding_set_ha_chassis_group(op->sb, NULL);
                    }
                }
            } else {
                /* Nothing is set. Clear ha_chassis_group  from pb. */
                if (op->sb->ha_chassis_group) {
                    sbrec_port_binding_set_ha_chassis_group(op->sb, NULL);
                }
            }

            if (op->sb->n_gateway_chassis) {
                /* Delete the legacy gateway_chassis from the pb. */
                sbrec_port_binding_set_gateway_chassis(op->sb, NULL, 0);
            }
            smap_add(&new, "distributed-port", op->nbrp->name);
            if (redirect_type) {
                smap_add(&new, "redirect-type", redirect_type);
            }
        } else {
            if (op->peer) {
                smap_add(&new, "peer", op->peer->key);
            }
            if (chassis_name) {
                smap_add(&new, "l3gateway-chassis", chassis_name);
            }
        }
        sbrec_port_binding_set_options(op->sb, &new);
        smap_destroy(&new);

        sbrec_port_binding_set_parent_port(op->sb, NULL);
        sbrec_port_binding_set_tag(op->sb, NULL, 0);

        struct ds s = DS_EMPTY_INITIALIZER;
        ds_put_cstr(&s, op->nbrp->mac);
        for (int i = 0; i < op->nbrp->n_networks; ++i) {
            ds_put_format(&s, " %s", op->nbrp->networks[i]);
        }
        const char *addresses = ds_cstr(&s);
        sbrec_port_binding_set_mac(op->sb, &addresses, 1);
        ds_destroy(&s);

        struct smap ids = SMAP_INITIALIZER(&ids);
        sbrec_port_binding_set_external_ids(op->sb, &ids);

        sbrec_port_binding_set_nat_addresses(op->sb, NULL, 0);
    } else {
        if (strcmp(op->nbsp->type, "router")) {
            uint32_t queue_id = smap_get_int(
                    &op->sb->options, "qdisc_queue_id", 0);
            bool has_qos = port_has_qos_params(&op->nbsp->options);
            const struct uuid *uuid = NULL;
            struct smap options;
            char *name = "";

            if (!strcmp(op->nbsp->type, "localnet")) {
                uuid = &op->sb->header_.uuid;
                name = "localnet";
            } else if (op->sb->chassis) {
                uuid = &op->sb->chassis->header_.uuid;
                name = op->sb->chassis->name;
            }

            if (has_qos && !queue_id) {
                queue_id = allocate_chassis_queueid(chassis_qdisc_queues,
                                                    uuid, name);
            } else if (!has_qos && queue_id) {
                free_chassis_queueid(chassis_qdisc_queues, uuid, queue_id);
                queue_id = 0;
            }

            smap_clone(&options, &op->nbsp->options);
            if (queue_id) {
                smap_add_format(&options,
                                "qdisc_queue_id", "%d", queue_id);
            }
            sbrec_port_binding_set_options(op->sb, &options);
            smap_destroy(&options);
            if (ovn_is_known_nb_lsp_type(op->nbsp->type)) {
                sbrec_port_binding_set_type(op->sb, op->nbsp->type);
            } else {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_WARN_RL(
                    &rl, "Unknown port type '%s' set on logical switch '%s'.",
                    op->nbsp->type, op->nbsp->name);
            }

            sbrec_port_binding_set_nat_addresses(op->sb, NULL, 0);

            if (!strcmp(op->nbsp->type, "external")) {
                if (op->nbsp->ha_chassis_group) {
                    sync_ha_chassis_group_for_sbpb(
                        ctx, op->nbsp->ha_chassis_group,
                        sbrec_chassis_by_name, op->sb);
                    sset_add(active_ha_chassis_grps,
                             op->nbsp->ha_chassis_group->name);
                } else {
                    sbrec_port_binding_set_ha_chassis_group(op->sb, NULL);
                }
            }
        } else {
            const char *chassis = NULL;
            if (op->peer && op->peer->od && op->peer->od->nbr) {
                chassis = smap_get(&op->peer->od->nbr->options, "chassis");
            }

            /* A switch port connected to a gateway router is also of
             * type "l3gateway". */
            if (chassis) {
                sbrec_port_binding_set_type(op->sb, "l3gateway");
            } else {
                sbrec_port_binding_set_type(op->sb, "patch");
            }

            const char *router_port = smap_get(&op->nbsp->options,
                                               "router-port");
            if (router_port || chassis) {
                struct smap new;
                smap_init(&new);
                if (router_port) {
                    smap_add(&new, "peer", router_port);
                }
                if (chassis) {
                    smap_add(&new, "l3gateway-chassis", chassis);
                }
                sbrec_port_binding_set_options(op->sb, &new);
                smap_destroy(&new);
            } else {
                sbrec_port_binding_set_options(op->sb, NULL);
            }

            const char *nat_addresses = smap_get(&op->nbsp->options,
                                           "nat-addresses");
            size_t n_nats = 0;
            char **nats = NULL;
            if (nat_addresses && !strcmp(nat_addresses, "router")) {
                if (op->peer && op->peer->od
                    && (chassis || op->peer->od->l3redirect_port)) {
                    nats = get_nat_addresses(op->peer, &n_nats);
                }
            /* Only accept manual specification of ethernet address
             * followed by IPv4 addresses on type "l3gateway" ports. */
            } else if (nat_addresses && chassis) {
                struct lport_addresses laddrs;
                if (!extract_lsp_addresses(nat_addresses, &laddrs)) {
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(1, 1);
                    VLOG_WARN_RL(&rl, "Error extracting nat-addresses.");
                } else {
                    destroy_lport_addresses(&laddrs);
                    n_nats = 1;
                    nats = xcalloc(1, sizeof *nats);
                    nats[0] = xstrdup(nat_addresses);
                }
            }

            /* Add the router mac and IPv4 addresses to
             * Port_Binding.nat_addresses so that GARP is sent for these
             * IPs by the ovn-controller on which the distributed gateway
             * router port resides if:
             *
             * -  op->peer has 'reside-on-gateway-chassis' set and the
             *    the logical router datapath has distributed router port.
             *
             * -  op->peer is distributed gateway router port.
             *
             * -  op->peer's router is a gateway router and op has a localnet
             *    port.
             *
             * Note: Port_Binding.nat_addresses column is also used for
             * sending the GARPs for the router port IPs.
             * */
            bool add_router_port_garp = false;
            if (op->peer && op->peer->nbrp && op->peer->od->l3dgw_port &&
                op->peer->od->l3redirect_port &&
                (smap_get_bool(&op->peer->nbrp->options,
                              "reside-on-redirect-chassis", false) ||
                op->peer == op->peer->od->l3dgw_port)) {
                add_router_port_garp = true;
            } else if (chassis && op->od->localnet_port) {
                add_router_port_garp = true;
            }

            if (add_router_port_garp) {
                struct ds garp_info = DS_EMPTY_INITIALIZER;
                ds_put_format(&garp_info, "%s", op->peer->lrp_networks.ea_s);
                for (size_t i = 0; i < op->peer->lrp_networks.n_ipv4_addrs;
                     i++) {
                    ds_put_format(&garp_info, " %s",
                                  op->peer->lrp_networks.ipv4_addrs[i].addr_s);
                }

                if (op->peer->od->l3redirect_port) {
                    ds_put_format(&garp_info, " is_chassis_resident(%s)",
                                  op->peer->od->l3redirect_port->json_key);
                }

                n_nats++;
                nats = xrealloc(nats, (n_nats * sizeof *nats));
                nats[n_nats - 1] = ds_steal_cstr(&garp_info);
                ds_destroy(&garp_info);
            }

            sbrec_port_binding_set_nat_addresses(op->sb,
                                                 (const char **) nats, n_nats);
            for (size_t i = 0; i < n_nats; i++) {
                free(nats[i]);
            }
            free(nats);
        }

        sbrec_port_binding_set_parent_port(op->sb, op->nbsp->parent_name);
        sbrec_port_binding_set_tag(op->sb, op->nbsp->tag, op->nbsp->n_tag);
        sbrec_port_binding_set_mac(op->sb, (const char **) op->nbsp->addresses,
                                   op->nbsp->n_addresses);

        struct smap ids = SMAP_INITIALIZER(&ids);
        smap_clone(&ids, &op->nbsp->external_ids);
        const char *name = smap_get(&ids, "neutron:port_name");
        if (name && name[0]) {
            smap_add(&ids, "name", name);
        }
        sbrec_port_binding_set_external_ids(op->sb, &ids);
        smap_destroy(&ids);
    }
}

/* Remove mac_binding entries that refer to logical_ports which are
 * deleted. */
static void
cleanup_mac_bindings(struct northd_context *ctx, struct hmap *ports)
{
    const struct sbrec_mac_binding *b, *n;
    SBREC_MAC_BINDING_FOR_EACH_SAFE (b, n, ctx->ovnsb_idl) {
        if (!ovn_port_find(ports, b->logical_port)) {
            sbrec_mac_binding_delete(b);
        }
    }
}

static void
cleanup_sb_ha_chassis_groups(struct northd_context *ctx,
                             struct sset *active_ha_chassis_groups)
{
    const struct sbrec_ha_chassis_group *b, *n;
    SBREC_HA_CHASSIS_GROUP_FOR_EACH_SAFE (b, n, ctx->ovnsb_idl) {
        if (!sset_contains(active_ha_chassis_groups, b->name)) {
            sbrec_ha_chassis_group_delete(b);
        }
    }
}

/* Updates the southbound Port_Binding table so that it contains the logical
 * switch ports specified by the northbound database.
 *
 * Initializes 'ports' to contain a "struct ovn_port" for every logical port,
 * using the "struct ovn_datapath"s in 'datapaths' to look up logical
 * datapaths. */
static void
build_ports(struct northd_context *ctx,
            struct ovsdb_idl_index *sbrec_chassis_by_name,
            struct hmap *datapaths, struct hmap *ports)
{
    struct ovs_list sb_only, nb_only, both;
    struct hmap tag_alloc_table = HMAP_INITIALIZER(&tag_alloc_table);
    struct hmap chassis_qdisc_queues = HMAP_INITIALIZER(&chassis_qdisc_queues);

    /* sset which stores the set of ha chassis group names used. */
    struct sset active_ha_chassis_grps =
        SSET_INITIALIZER(&active_ha_chassis_grps);

    join_logical_ports(ctx, datapaths, ports, &chassis_qdisc_queues,
                       &tag_alloc_table, &sb_only, &nb_only, &both);

    struct ovn_port *op, *next;
    /* For logical ports that are in both databases, update the southbound
     * record based on northbound data.  Also index the in-use tunnel_keys.
     * For logical ports that are in NB database, do any tag allocation
     * needed. */
    LIST_FOR_EACH_SAFE (op, next, list, &both) {
        if (op->nbsp) {
            tag_alloc_create_new_tag(&tag_alloc_table, op->nbsp);
        }
        ovn_port_update_sbrec(ctx, sbrec_chassis_by_name,
                              op, &chassis_qdisc_queues,
                              &active_ha_chassis_grps);
        add_tnlid(&op->od->port_tnlids, op->sb->tunnel_key);
        if (op->sb->tunnel_key > op->od->port_key_hint) {
            op->od->port_key_hint = op->sb->tunnel_key;
        }
    }

    /* Add southbound record for each unmatched northbound record. */
    LIST_FOR_EACH_SAFE (op, next, list, &nb_only) {
        uint16_t tunnel_key = ovn_port_allocate_key(op->od);
        if (!tunnel_key) {
            continue;
        }

        ovn_port_set_sb(op, sbrec_port_binding_insert(ctx->ovnsb_txn));
        ovn_port_update_sbrec(ctx, sbrec_chassis_by_name, op,
                              &chassis_qdisc_queues,
                              &active_ha_chassis_grps);
        sbrec_port_binding_set_logical_port(op->sb, op->key);
        sbrec_port_binding_set_tunnel_key(op->sb, tunnel_key);
    }

    bool remove_mac_bindings = false;
    if (!ovs_list_is_empty(&sb_only)) {
        remove_mac_bindings = true;
    }

    /* Delete southbound records without northbound matches. */
    LIST_FOR_EACH_SAFE(op, next, list, &sb_only) {
        ovs_list_remove(&op->list);
        sbrec_port_binding_delete(op->sb);
        ovn_port_destroy(ports, op);
    }
    if (remove_mac_bindings) {
        cleanup_mac_bindings(ctx, ports);
    }

    tag_alloc_destroy(&tag_alloc_table);
    destroy_chassis_queues(&chassis_qdisc_queues);
    cleanup_sb_ha_chassis_groups(ctx, &active_ha_chassis_grps);
    sset_destroy(&active_ha_chassis_grps);
}

struct multicast_group {
    const char *name;
    uint16_t key;               /* OVN_MIN_MULTICAST...OVN_MAX_MULTICAST. */
};

#define MC_FLOOD "_MC_flood"
static const struct multicast_group mc_flood =
    { MC_FLOOD, OVN_MCAST_FLOOD_TUNNEL_KEY };

#define MC_MROUTER_FLOOD "_MC_mrouter_flood"
static const struct multicast_group mc_mrouter_flood =
    { MC_MROUTER_FLOOD, OVN_MCAST_MROUTER_FLOOD_TUNNEL_KEY };

#define MC_MROUTER_STATIC "_MC_mrouter_static"
static const struct multicast_group mc_mrouter_static =
    { MC_MROUTER_STATIC, OVN_MCAST_MROUTER_STATIC_TUNNEL_KEY };

#define MC_STATIC "_MC_static"
static const struct multicast_group mc_static =
    { MC_STATIC, OVN_MCAST_STATIC_TUNNEL_KEY };

#define MC_UNKNOWN "_MC_unknown"
static const struct multicast_group mc_unknown =
    { MC_UNKNOWN, OVN_MCAST_UNKNOWN_TUNNEL_KEY };

static bool
multicast_group_equal(const struct multicast_group *a,
                      const struct multicast_group *b)
{
    return !strcmp(a->name, b->name) && a->key == b->key;
}

/* Multicast group entry. */
struct ovn_multicast {
    struct hmap_node hmap_node; /* Index on 'datapath' and 'key'. */
    struct ovn_datapath *datapath;
    const struct multicast_group *group;

    struct ovn_port **ports;
    size_t n_ports, allocated_ports;
};

static uint32_t
ovn_multicast_hash(const struct ovn_datapath *datapath,
                   const struct multicast_group *group)
{
    return hash_pointer(datapath, group->key);
}

static struct ovn_multicast *
ovn_multicast_find(struct hmap *mcgroups, struct ovn_datapath *datapath,
                   const struct multicast_group *group)
{
    struct ovn_multicast *mc;

    HMAP_FOR_EACH_WITH_HASH (mc, hmap_node,
                             ovn_multicast_hash(datapath, group), mcgroups) {
        if (mc->datapath == datapath
            && multicast_group_equal(mc->group, group)) {
            return mc;
        }
    }
    return NULL;
}

static void
ovn_multicast_add_ports(struct hmap *mcgroups, struct ovn_datapath *od,
                        const struct multicast_group *group,
                        struct ovn_port **ports, size_t n_ports)
{
    struct ovn_multicast *mc = ovn_multicast_find(mcgroups, od, group);
    if (!mc) {
        mc = xmalloc(sizeof *mc);
        hmap_insert(mcgroups, &mc->hmap_node, ovn_multicast_hash(od, group));
        mc->datapath = od;
        mc->group = group;
        mc->n_ports = 0;
        mc->allocated_ports = 4;
        mc->ports = xmalloc(mc->allocated_ports * sizeof *mc->ports);
    }

    size_t n_ports_total = mc->n_ports + n_ports;

    if (n_ports_total > 2 * mc->allocated_ports) {
        mc->allocated_ports = n_ports_total;
        mc->ports = xrealloc(mc->ports,
                             mc->allocated_ports * sizeof *mc->ports);
    } else if (n_ports_total > mc->allocated_ports) {
        mc->ports = x2nrealloc(mc->ports, &mc->allocated_ports,
                               sizeof *mc->ports);
    }

    memcpy(&mc->ports[mc->n_ports], &ports[0], n_ports * sizeof *ports);
    mc->n_ports += n_ports;
}

static void
ovn_multicast_add(struct hmap *mcgroups, const struct multicast_group *group,
                  struct ovn_port *port)
{
    ovn_multicast_add_ports(mcgroups, port->od, group, &port, 1);
}

static void
ovn_multicast_destroy(struct hmap *mcgroups, struct ovn_multicast *mc)
{
    if (mc) {
        hmap_remove(mcgroups, &mc->hmap_node);
        free(mc->ports);
        free(mc);
    }
}

static void
ovn_multicast_update_sbrec(const struct ovn_multicast *mc,
                           const struct sbrec_multicast_group *sb)
{
    struct sbrec_port_binding **ports = xmalloc(mc->n_ports * sizeof *ports);
    for (size_t i = 0; i < mc->n_ports; i++) {
        ports[i] = CONST_CAST(struct sbrec_port_binding *, mc->ports[i]->sb);
    }
    sbrec_multicast_group_set_ports(sb, ports, mc->n_ports);
    free(ports);
}

/*
 * IGMP group entry (1:1 mapping to SB database).
 */
struct ovn_igmp_group_entry {
    struct ovs_list list_node; /* Linkage in the list of entries. */
    size_t n_ports;
    struct ovn_port **ports;
};

/*
 * IGMP group entry (aggregate of all entries from the SB database
 * corresponding to the multicast group).
 */
struct ovn_igmp_group {
    struct hmap_node hmap_node; /* Index on 'datapath' and 'address'. */
    struct ovs_list list_node;  /* Linkage in the per-dp igmp group list. */

    struct ovn_datapath *datapath;
    struct in6_addr address; /* Multicast IPv6-mapped-IPv4 or IPv4 address. */
    struct multicast_group mcgroup;

    struct ovs_list entries; /* List of SB entries for this group. */
};

static uint32_t
ovn_igmp_group_hash(const struct ovn_datapath *datapath,
                    const struct in6_addr *address)
{
    return hash_pointer(datapath, hash_bytes(address, sizeof *address, 0));
}

static struct ovn_igmp_group *
ovn_igmp_group_find(struct hmap *igmp_groups,
                    const struct ovn_datapath *datapath,
                    const struct in6_addr *address)
{
    struct ovn_igmp_group *group;

    HMAP_FOR_EACH_WITH_HASH (group, hmap_node,
                             ovn_igmp_group_hash(datapath, address),
                             igmp_groups) {
        if (group->datapath == datapath &&
                ipv6_addr_equals(&group->address, address)) {
            return group;
        }
    }
    return NULL;
}

static struct ovn_igmp_group *
ovn_igmp_group_add(struct northd_context *ctx, struct hmap *igmp_groups,
                   struct ovn_datapath *datapath,
                   const struct in6_addr *address,
                   const char *address_s)
{
    struct ovn_igmp_group *igmp_group =
        ovn_igmp_group_find(igmp_groups, datapath, address);

    if (!igmp_group) {
        igmp_group = xmalloc(sizeof *igmp_group);

        const struct sbrec_multicast_group *mcgroup =
            mcast_group_lookup(ctx->sbrec_mcast_group_by_name_dp, address_s,
                               datapath->sb);

        igmp_group->datapath = datapath;
        igmp_group->address = *address;
        if (mcgroup) {
            igmp_group->mcgroup.key = mcgroup->tunnel_key;
            add_tnlid(&datapath->mcast_info.group_tnlids, mcgroup->tunnel_key);
        } else {
            igmp_group->mcgroup.key = 0;
        }
        igmp_group->mcgroup.name = address_s;
        ovs_list_init(&igmp_group->entries);

        hmap_insert(igmp_groups, &igmp_group->hmap_node,
                    ovn_igmp_group_hash(datapath, address));
        ovs_list_push_back(&datapath->mcast_info.groups,
                           &igmp_group->list_node);
    }

    return igmp_group;
}

static bool
ovn_igmp_group_get_address(const struct sbrec_igmp_group *sb_igmp_group,
                           struct in6_addr *address)
{
    ovs_be32 ipv4;

    if (ip_parse(sb_igmp_group->address, &ipv4)) {
        *address = in6_addr_mapped_ipv4(ipv4);
        return true;
    }
    if (!ipv6_parse(sb_igmp_group->address, address)) {
        return false;
    }
    return true;
}

static struct ovn_port **
ovn_igmp_group_get_ports(const struct sbrec_igmp_group *sb_igmp_group,
                         size_t *n_ports, struct hmap *ovn_ports)
{
    struct ovn_port **ports = xmalloc(sb_igmp_group->n_ports * sizeof *ports);

     *n_ports = 0;
     for (size_t i = 0; i < sb_igmp_group->n_ports; i++) {
        struct ovn_port *port =
            ovn_port_find(ovn_ports, sb_igmp_group->ports[i]->logical_port);

        /* If this is already a flood port skip it for the group. */
        if (port->mcast_info.flood) {
            continue;
        }

        /* If this is already a port of a router on which relay is enabled,
         * skip it for the group. Traffic is flooded there anyway.
         */
        if (port->peer && port->peer->od &&
                port->peer->od->mcast_info.rtr.relay) {
            continue;
        }

        ports[(*n_ports)] = port;
            ovn_port_find(ovn_ports, sb_igmp_group->ports[i]->logical_port);
        if (ports[(*n_ports)]) {
            (*n_ports)++;
        }
    }

    return ports;
}

static void
ovn_igmp_group_add_entry(struct ovn_igmp_group *igmp_group,
                         struct ovn_port **ports, size_t n_ports)
{
    struct ovn_igmp_group_entry *entry = xmalloc(sizeof *entry);

    entry->ports = ports;
    entry->n_ports = n_ports;
    ovs_list_push_back(&igmp_group->entries, &entry->list_node);
}

static void
ovn_igmp_group_destroy_entry(struct ovn_igmp_group_entry *entry)
{
    free(entry->ports);
}

static bool
ovn_igmp_group_allocate_id(struct ovn_igmp_group *igmp_group)
{
    if (igmp_group->mcgroup.key == 0) {
        struct mcast_info *mcast_info = &igmp_group->datapath->mcast_info;
        igmp_group->mcgroup.key = ovn_mcast_group_allocate_key(mcast_info);
    }

    if (igmp_group->mcgroup.key == 0) {
        return false;
    }

    return true;
}

static void
ovn_igmp_group_aggregate_ports(struct ovn_igmp_group *igmp_group,
                               struct hmap *mcast_groups)
{
    struct ovn_igmp_group_entry *entry;

    LIST_FOR_EACH_POP (entry, list_node, &igmp_group->entries) {
        ovn_multicast_add_ports(mcast_groups, igmp_group->datapath,
                                &igmp_group->mcgroup, entry->ports,
                                entry->n_ports);

        ovn_igmp_group_destroy_entry(entry);
        free(entry);
    }
}

static void
ovn_igmp_group_destroy(struct hmap *igmp_groups,
                       struct ovn_igmp_group *igmp_group)
{
    if (igmp_group) {
        struct ovn_igmp_group_entry *entry;

        LIST_FOR_EACH_POP (entry, list_node, &igmp_group->entries) {
            ovn_igmp_group_destroy_entry(entry);
            free(entry);
        }
        hmap_remove(igmp_groups, &igmp_group->hmap_node);
        ovs_list_remove(&igmp_group->list_node);
        free(igmp_group);
    }
}

/* Logical flow generation.
 *
 * This code generates the Logical_Flow table in the southbound database, as a
 * function of most of the northbound database.
 */

struct ovn_lflow {
    struct hmap_node hmap_node;

    struct ovn_datapath *od;
    enum ovn_stage stage;
    uint16_t priority;
    char *match;
    char *actions;
    char *stage_hint;
    const char *where;
};

static size_t
ovn_lflow_hash(const struct ovn_lflow *lflow)
{
    return ovn_logical_flow_hash(&lflow->od->sb->header_.uuid,
                                 ovn_stage_get_table(lflow->stage),
                                 ovn_stage_get_pipeline_name(lflow->stage),
                                 lflow->priority, lflow->match,
                                 lflow->actions);
}

static bool
ovn_lflow_equal(const struct ovn_lflow *a, const struct ovn_lflow *b)
{
    return (a->od == b->od
            && a->stage == b->stage
            && a->priority == b->priority
            && !strcmp(a->match, b->match)
            && !strcmp(a->actions, b->actions));
}

static void
ovn_lflow_init(struct ovn_lflow *lflow, struct ovn_datapath *od,
               enum ovn_stage stage, uint16_t priority,
               char *match, char *actions, char *stage_hint,
               const char *where)
{
    lflow->od = od;
    lflow->stage = stage;
    lflow->priority = priority;
    lflow->match = match;
    lflow->actions = actions;
    lflow->stage_hint = stage_hint;
    lflow->where = where;
}

/* Adds a row with the specified contents to the Logical_Flow table. */
static void
ovn_lflow_add_at(struct hmap *lflow_map, struct ovn_datapath *od,
                 enum ovn_stage stage, uint16_t priority,
                 const char *match, const char *actions,
                 const char *stage_hint, const char *where)
{
    ovs_assert(ovn_stage_to_datapath_type(stage) == ovn_datapath_get_type(od));

    struct ovn_lflow *lflow = xmalloc(sizeof *lflow);
    ovn_lflow_init(lflow, od, stage, priority,
                   xstrdup(match), xstrdup(actions),
                   nullable_xstrdup(stage_hint), where);
    hmap_insert(lflow_map, &lflow->hmap_node, ovn_lflow_hash(lflow));
}

/* Adds a row with the specified contents to the Logical_Flow table. */
#define ovn_lflow_add_with_hint(LFLOW_MAP, OD, STAGE, PRIORITY, MATCH, \
                                ACTIONS, STAGE_HINT) \
    ovn_lflow_add_at(LFLOW_MAP, OD, STAGE, PRIORITY, MATCH, ACTIONS, \
                     STAGE_HINT, OVS_SOURCE_LOCATOR)

#define ovn_lflow_add(LFLOW_MAP, OD, STAGE, PRIORITY, MATCH, ACTIONS) \
    ovn_lflow_add_with_hint(LFLOW_MAP, OD, STAGE, PRIORITY, MATCH, \
                            ACTIONS, NULL)

static struct ovn_lflow *
ovn_lflow_find(struct hmap *lflows, struct ovn_datapath *od,
               enum ovn_stage stage, uint16_t priority,
               const char *match, const char *actions, uint32_t hash)
{
    struct ovn_lflow target;
    ovn_lflow_init(&target, od, stage, priority,
                   CONST_CAST(char *, match), CONST_CAST(char *, actions),
                   NULL, NULL);

    struct ovn_lflow *lflow;
    HMAP_FOR_EACH_WITH_HASH (lflow, hmap_node, hash, lflows) {
        if (ovn_lflow_equal(lflow, &target)) {
            return lflow;
        }
    }
    return NULL;
}

static void
ovn_lflow_destroy(struct hmap *lflows, struct ovn_lflow *lflow)
{
    if (lflow) {
        hmap_remove(lflows, &lflow->hmap_node);
        free(lflow->match);
        free(lflow->actions);
        free(lflow->stage_hint);
        free(lflow);
    }
}

/* Appends port security constraints on L2 address field 'eth_addr_field'
 * (e.g. "eth.src" or "eth.dst") to 'match'.  'ps_addrs', with 'n_ps_addrs'
 * elements, is the collection of port_security constraints from an
 * OVN_NB Logical_Switch_Port row generated by extract_lsp_addresses(). */
static void
build_port_security_l2(const char *eth_addr_field,
                       struct lport_addresses *ps_addrs,
                       unsigned int n_ps_addrs,
                       struct ds *match)
{
    if (!n_ps_addrs) {
        return;
    }

    ds_put_format(match, " && %s == {", eth_addr_field);

    for (size_t i = 0; i < n_ps_addrs; i++) {
        ds_put_format(match, "%s ", ps_addrs[i].ea_s);
    }
    ds_chomp(match, ' ');
    ds_put_cstr(match, "}");
}

static void
build_port_security_ipv6_nd_flow(
    struct ds *match, struct eth_addr ea, struct ipv6_netaddr *ipv6_addrs,
    int n_ipv6_addrs)
{
    ds_put_format(match, " && ip6 && nd && ((nd.sll == "ETH_ADDR_FMT" || "
                  "nd.sll == "ETH_ADDR_FMT") || ((nd.tll == "ETH_ADDR_FMT" || "
                  "nd.tll == "ETH_ADDR_FMT")", ETH_ADDR_ARGS(eth_addr_zero),
                  ETH_ADDR_ARGS(ea), ETH_ADDR_ARGS(eth_addr_zero),
                  ETH_ADDR_ARGS(ea));
    if (!n_ipv6_addrs) {
        ds_put_cstr(match, "))");
        return;
    }

    char ip6_str[INET6_ADDRSTRLEN + 1];
    struct in6_addr lla;
    in6_generate_lla(ea, &lla);
    memset(ip6_str, 0, sizeof(ip6_str));
    ipv6_string_mapped(ip6_str, &lla);
    ds_put_format(match, " && (nd.target == %s", ip6_str);

    for(int i = 0; i < n_ipv6_addrs; i++) {
        memset(ip6_str, 0, sizeof(ip6_str));
        ipv6_string_mapped(ip6_str, &ipv6_addrs[i].addr);
        ds_put_format(match, " || nd.target == %s", ip6_str);
    }

    ds_put_format(match, ")))");
}

static void
build_port_security_ipv6_flow(
    enum ovn_pipeline pipeline, struct ds *match, struct eth_addr ea,
    struct ipv6_netaddr *ipv6_addrs, int n_ipv6_addrs)
{
    char ip6_str[INET6_ADDRSTRLEN + 1];

    ds_put_format(match, " && %s == {",
                  pipeline == P_IN ? "ip6.src" : "ip6.dst");

    /* Allow link-local address. */
    struct in6_addr lla;
    in6_generate_lla(ea, &lla);
    ipv6_string_mapped(ip6_str, &lla);
    ds_put_format(match, "%s, ", ip6_str);

    /* Allow ip6.dst=ff00::/8 for multicast packets */
    if (pipeline == P_OUT) {
        ds_put_cstr(match, "ff00::/8, ");
    }
    for(int i = 0; i < n_ipv6_addrs; i++) {
        ipv6_string_mapped(ip6_str, &ipv6_addrs[i].addr);
        ds_put_format(match, "%s, ", ip6_str);
    }
    /* Replace ", " by "}". */
    ds_chomp(match, ' ');
    ds_chomp(match, ',');
    ds_put_cstr(match, "}");
}

/**
 * Build port security constraints on ARP and IPv6 ND fields
 * and add logical flows to S_SWITCH_IN_PORT_SEC_ND stage.
 *
 * For each port security of the logical port, following
 * logical flows are added
 *   - If the port security has no IP (both IPv4 and IPv6) or
 *     if it has IPv4 address(es)
 *      - Priority 90 flow to allow ARP packets for known MAC addresses
 *        in the eth.src and arp.spa fields. If the port security
 *        has IPv4 addresses, allow known IPv4 addresses in the arp.tpa field.
 *
 *   - If the port security has no IP (both IPv4 and IPv6) or
 *     if it has IPv6 address(es)
 *     - Priority 90 flow to allow IPv6 ND packets for known MAC addresses
 *       in the eth.src and nd.sll/nd.tll fields. If the port security
 *       has IPv6 addresses, allow known IPv6 addresses in the nd.target field
 *       for IPv6 Neighbor Advertisement packet.
 *
 *   - Priority 80 flow to drop ARP and IPv6 ND packets.
 */
static void
build_port_security_nd(struct ovn_port *op, struct hmap *lflows)
{
    struct ds match = DS_EMPTY_INITIALIZER;

    for (size_t i = 0; i < op->n_ps_addrs; i++) {
        struct lport_addresses *ps = &op->ps_addrs[i];

        bool no_ip = !(ps->n_ipv4_addrs || ps->n_ipv6_addrs);

        ds_clear(&match);
        if (ps->n_ipv4_addrs || no_ip) {
            ds_put_format(&match,
                          "inport == %s && eth.src == %s && arp.sha == %s",
                          op->json_key, ps->ea_s, ps->ea_s);

            if (ps->n_ipv4_addrs) {
                ds_put_cstr(&match, " && arp.spa == {");
                for (size_t j = 0; j < ps->n_ipv4_addrs; j++) {
                    /* When the netmask is applied, if the host portion is
                     * non-zero, the host can only use the specified
                     * address in the arp.spa.  If zero, the host is allowed
                     * to use any address in the subnet. */
                    if (ps->ipv4_addrs[j].plen == 32
                        || ps->ipv4_addrs[j].addr & ~ps->ipv4_addrs[j].mask) {
                        ds_put_cstr(&match, ps->ipv4_addrs[j].addr_s);
                    } else {
                        ds_put_format(&match, "%s/%d",
                                      ps->ipv4_addrs[j].network_s,
                                      ps->ipv4_addrs[j].plen);
                    }
                    ds_put_cstr(&match, ", ");
                }
                ds_chomp(&match, ' ');
                ds_chomp(&match, ',');
                ds_put_cstr(&match, "}");
            }
            ovn_lflow_add(lflows, op->od, S_SWITCH_IN_PORT_SEC_ND, 90,
                          ds_cstr(&match), "next;");
        }

        if (ps->n_ipv6_addrs || no_ip) {
            ds_clear(&match);
            ds_put_format(&match, "inport == %s && eth.src == %s",
                          op->json_key, ps->ea_s);
            build_port_security_ipv6_nd_flow(&match, ps->ea, ps->ipv6_addrs,
                                             ps->n_ipv6_addrs);
            ovn_lflow_add(lflows, op->od, S_SWITCH_IN_PORT_SEC_ND, 90,
                          ds_cstr(&match), "next;");
        }
    }

    ds_clear(&match);
    ds_put_format(&match, "inport == %s && (arp || nd)", op->json_key);
    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_PORT_SEC_ND, 80,
                  ds_cstr(&match), "drop;");
    ds_destroy(&match);
}

/**
 * Build port security constraints on IPv4 and IPv6 src and dst fields
 * and add logical flows to S_SWITCH_(IN/OUT)_PORT_SEC_IP stage.
 *
 * For each port security of the logical port, following
 * logical flows are added
 *   - If the port security has IPv4 addresses,
 *     - Priority 90 flow to allow IPv4 packets for known IPv4 addresses
 *
 *   - If the port security has IPv6 addresses,
 *     - Priority 90 flow to allow IPv6 packets for known IPv6 addresses
 *
 *   - If the port security has IPv4 addresses or IPv6 addresses or both
 *     - Priority 80 flow to drop all IPv4 and IPv6 traffic
 */
static void
build_port_security_ip(enum ovn_pipeline pipeline, struct ovn_port *op,
                       struct hmap *lflows)
{
    char *port_direction;
    enum ovn_stage stage;
    if (pipeline == P_IN) {
        port_direction = "inport";
        stage = S_SWITCH_IN_PORT_SEC_IP;
    } else {
        port_direction = "outport";
        stage = S_SWITCH_OUT_PORT_SEC_IP;
    }

    for (size_t i = 0; i < op->n_ps_addrs; i++) {
        struct lport_addresses *ps = &op->ps_addrs[i];

        if (!(ps->n_ipv4_addrs || ps->n_ipv6_addrs)) {
            continue;
        }

        if (ps->n_ipv4_addrs) {
            struct ds match = DS_EMPTY_INITIALIZER;
            if (pipeline == P_IN) {
                /* Permit use of the unspecified address for DHCP discovery */
                struct ds dhcp_match = DS_EMPTY_INITIALIZER;
                ds_put_format(&dhcp_match, "inport == %s"
                              " && eth.src == %s"
                              " && ip4.src == 0.0.0.0"
                              " && ip4.dst == 255.255.255.255"
                              " && udp.src == 68 && udp.dst == 67",
                              op->json_key, ps->ea_s);
                ovn_lflow_add(lflows, op->od, stage, 90,
                              ds_cstr(&dhcp_match), "next;");
                ds_destroy(&dhcp_match);
                ds_put_format(&match, "inport == %s && eth.src == %s"
                              " && ip4.src == {", op->json_key,
                              ps->ea_s);
            } else {
                ds_put_format(&match, "outport == %s && eth.dst == %s"
                              " && ip4.dst == {255.255.255.255, 224.0.0.0/4, ",
                              op->json_key, ps->ea_s);
            }

            for (int j = 0; j < ps->n_ipv4_addrs; j++) {
                ovs_be32 mask = ps->ipv4_addrs[j].mask;
                /* When the netmask is applied, if the host portion is
                 * non-zero, the host can only use the specified
                 * address.  If zero, the host is allowed to use any
                 * address in the subnet.
                 */
                if (ps->ipv4_addrs[j].plen == 32
                    || ps->ipv4_addrs[j].addr & ~mask) {
                    ds_put_format(&match, "%s", ps->ipv4_addrs[j].addr_s);
                    if (pipeline == P_OUT && ps->ipv4_addrs[j].plen != 32) {
                        /* Host is also allowed to receive packets to the
                         * broadcast address in the specified subnet. */
                        ds_put_format(&match, ", %s",
                                      ps->ipv4_addrs[j].bcast_s);
                    }
                } else {
                    /* host portion is zero */
                    ds_put_format(&match, "%s/%d", ps->ipv4_addrs[j].network_s,
                                  ps->ipv4_addrs[j].plen);
                }
                ds_put_cstr(&match, ", ");
            }

            /* Replace ", " by "}". */
            ds_chomp(&match, ' ');
            ds_chomp(&match, ',');
            ds_put_cstr(&match, "}");
            ovn_lflow_add(lflows, op->od, stage, 90, ds_cstr(&match), "next;");
            ds_destroy(&match);
        }

        if (ps->n_ipv6_addrs) {
            struct ds match = DS_EMPTY_INITIALIZER;
            if (pipeline == P_IN) {
                /* Permit use of unspecified address for duplicate address
                 * detection */
                struct ds dad_match = DS_EMPTY_INITIALIZER;
                ds_put_format(&dad_match, "inport == %s"
                              " && eth.src == %s"
                              " && ip6.src == ::"
                              " && ip6.dst == ff02::/16"
                              " && icmp6.type == {131, 135, 143}", op->json_key,
                              ps->ea_s);
                ovn_lflow_add(lflows, op->od, stage, 90,
                              ds_cstr(&dad_match), "next;");
                ds_destroy(&dad_match);
            }
            ds_put_format(&match, "%s == %s && %s == %s",
                          port_direction, op->json_key,
                          pipeline == P_IN ? "eth.src" : "eth.dst", ps->ea_s);
            build_port_security_ipv6_flow(pipeline, &match, ps->ea,
                                          ps->ipv6_addrs, ps->n_ipv6_addrs);
            ovn_lflow_add(lflows, op->od, stage, 90,
                          ds_cstr(&match), "next;");
            ds_destroy(&match);
        }

        char *match = xasprintf("%s == %s && %s == %s && ip",
                                port_direction, op->json_key,
                                pipeline == P_IN ? "eth.src" : "eth.dst",
                                ps->ea_s);
        ovn_lflow_add(lflows, op->od, stage, 80, match, "drop;");
        free(match);
    }

}

/* Returns true if the logical switch port 'enabled' column is empty or
 * set to true.  Otherwise, returns false. */
static bool
lsp_is_enabled(const struct nbrec_logical_switch_port *lsp)
{
    return !lsp->n_enabled || *lsp->enabled;
}

/* Returns true only if the logical switch port 'up' column is set to true.
 * Otherwise, if the column is not set or set to false, returns false. */
static bool
lsp_is_up(const struct nbrec_logical_switch_port *lsp)
{
    return lsp->n_up && *lsp->up;
}

static bool
lsp_is_external(const struct nbrec_logical_switch_port *nbsp)
{
    return !strcmp(nbsp->type, "external");
}

static bool
build_dhcpv4_action(struct ovn_port *op, ovs_be32 offer_ip,
                    struct ds *options_action, struct ds *response_action,
                    struct ds *ipv4_addr_match)
{
    if (!op->nbsp->dhcpv4_options) {
        /* CMS has disabled native DHCPv4 for this lport. */
        return false;
    }

    ovs_be32 host_ip, mask;
    char *error = ip_parse_masked(op->nbsp->dhcpv4_options->cidr, &host_ip,
                                  &mask);
    if (error || ((offer_ip ^ host_ip) & mask)) {
       /* Either
        *  - cidr defined is invalid or
        *  - the offer ip of the logical port doesn't belong to the cidr
        *    defined in the DHCPv4 options.
        *  */
        free(error);
        return false;
    }

    const char *server_ip = smap_get(
        &op->nbsp->dhcpv4_options->options, "server_id");
    const char *server_mac = smap_get(
        &op->nbsp->dhcpv4_options->options, "server_mac");
    const char *lease_time = smap_get(
        &op->nbsp->dhcpv4_options->options, "lease_time");

    if (!(server_ip && server_mac && lease_time)) {
        /* "server_id", "server_mac" and "lease_time" should be
         * present in the dhcp_options. */
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
        VLOG_WARN_RL(&rl, "Required DHCPv4 options not defined for lport - %s",
                     op->json_key);
        return false;
    }

    struct smap dhcpv4_options = SMAP_INITIALIZER(&dhcpv4_options);
    smap_clone(&dhcpv4_options, &op->nbsp->dhcpv4_options->options);

    /* server_mac is not DHCPv4 option, delete it from the smap. */
    smap_remove(&dhcpv4_options, "server_mac");
    char *netmask = xasprintf(IP_FMT, IP_ARGS(mask));
    smap_add(&dhcpv4_options, "netmask", netmask);
    free(netmask);

    ds_put_format(options_action,
                  REGBIT_DHCP_OPTS_RESULT" = put_dhcp_opts(offerip = "
                  IP_FMT", ", IP_ARGS(offer_ip));

    /* We're not using SMAP_FOR_EACH because we want a consistent order of the
     * options on different architectures (big or little endian, SSE4.2) */
    const struct smap_node **sorted_opts = smap_sort(&dhcpv4_options);
    for (size_t i = 0; i < smap_count(&dhcpv4_options); i++) {
        const struct smap_node *node = sorted_opts[i];
        ds_put_format(options_action, "%s = %s, ", node->key, node->value);
    }
    free(sorted_opts);

    ds_chomp(options_action, ' ');
    ds_chomp(options_action, ',');
    ds_put_cstr(options_action, "); next;");

    ds_put_format(response_action, "eth.dst = eth.src; eth.src = %s; "
                  "ip4.dst = "IP_FMT"; ip4.src = %s; udp.src = 67; "
                  "udp.dst = 68; outport = inport; flags.loopback = 1; "
                  "output;",
                  server_mac, IP_ARGS(offer_ip), server_ip);

    ds_put_format(ipv4_addr_match,
                  "ip4.src == "IP_FMT" && ip4.dst == {%s, 255.255.255.255}",
                  IP_ARGS(offer_ip), server_ip);
    smap_destroy(&dhcpv4_options);
    return true;
}

static bool
build_dhcpv6_action(struct ovn_port *op, struct in6_addr *offer_ip,
                    struct ds *options_action, struct ds *response_action)
{
    if (!op->nbsp->dhcpv6_options) {
        /* CMS has disabled native DHCPv6 for this lport. */
        return false;
    }

    struct in6_addr host_ip, mask;

    char *error = ipv6_parse_masked(op->nbsp->dhcpv6_options->cidr, &host_ip,
                                        &mask);
    if (error) {
        free(error);
        return false;
    }
    struct in6_addr ip6_mask = ipv6_addr_bitxor(offer_ip, &host_ip);
    ip6_mask = ipv6_addr_bitand(&ip6_mask, &mask);
    if (!ipv6_mask_is_any(&ip6_mask)) {
        /* offer_ip doesn't belongs to the cidr defined in lport's DHCPv6
         * options.*/
        return false;
    }

    const struct smap *options_map = &op->nbsp->dhcpv6_options->options;
    /* "server_id" should be the MAC address. */
    const char *server_mac = smap_get(options_map, "server_id");
    struct eth_addr ea;
    if (!server_mac || !eth_addr_from_string(server_mac, &ea)) {
        /* "server_id" should be present in the dhcpv6_options. */
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "server_id not present in the DHCPv6 options"
                          " for lport %s", op->json_key);
        return false;
    }

    /* Get the link local IP of the DHCPv6 server from the server MAC. */
    struct in6_addr lla;
    in6_generate_lla(ea, &lla);

    char server_ip[INET6_ADDRSTRLEN + 1];
    ipv6_string_mapped(server_ip, &lla);

    char ia_addr[INET6_ADDRSTRLEN + 1];
    ipv6_string_mapped(ia_addr, offer_ip);

    ds_put_format(options_action,
                  REGBIT_DHCP_OPTS_RESULT" = put_dhcpv6_opts(");

    /* Check whether the dhcpv6 options should be configured as stateful.
     * Only reply with ia_addr option for dhcpv6 stateful address mode. */
    if (!smap_get_bool(options_map, "dhcpv6_stateless", false)) {
        ipv6_string_mapped(ia_addr, offer_ip);
        ds_put_format(options_action, "ia_addr = %s, ", ia_addr);
    }

    /* We're not using SMAP_FOR_EACH because we want a consistent order of the
     * options on different architectures (big or little endian, SSE4.2) */
    const struct smap_node **sorted_opts = smap_sort(options_map);
    for (size_t i = 0; i < smap_count(options_map); i++) {
        const struct smap_node *node = sorted_opts[i];
        if (strcmp(node->key, "dhcpv6_stateless")) {
            ds_put_format(options_action, "%s = %s, ", node->key, node->value);
        }
    }
    free(sorted_opts);

    ds_chomp(options_action, ' ');
    ds_chomp(options_action, ',');
    ds_put_cstr(options_action, "); next;");

    ds_put_format(response_action, "eth.dst = eth.src; eth.src = %s; "
                  "ip6.dst = ip6.src; ip6.src = %s; udp.src = 547; "
                  "udp.dst = 546; outport = inport; flags.loopback = 1; "
                  "output;",
                  server_mac, server_ip);

    return true;
}

struct ovn_port_group_ls {
    struct hmap_node key_node;  /* Index on 'key'. */
    struct uuid key;            /* nb_ls->header_.uuid. */
    const struct nbrec_logical_switch *nb_ls;
};

struct ovn_port_group {
    struct hmap_node key_node;  /* Index on 'key'. */
    struct uuid key;            /* nb_pg->header_.uuid. */
    const struct nbrec_port_group *nb_pg;
    struct hmap nb_lswitches;   /* NB lswitches related to the port group */
};

static void
ovn_port_group_ls_add(struct ovn_port_group *pg,
                      const struct nbrec_logical_switch *nb_ls)
{
    struct ovn_port_group_ls *pg_ls = xzalloc(sizeof *pg_ls);
    pg_ls->key = nb_ls->header_.uuid;
    pg_ls->nb_ls = nb_ls;
    hmap_insert(&pg->nb_lswitches, &pg_ls->key_node, uuid_hash(&pg_ls->key));
}

static struct ovn_port_group_ls *
ovn_port_group_ls_find(struct ovn_port_group *pg, const struct uuid *ls_uuid)
{
    struct ovn_port_group_ls *pg_ls;

    HMAP_FOR_EACH_WITH_HASH (pg_ls, key_node, uuid_hash(ls_uuid),
                             &pg->nb_lswitches) {
        if (uuid_equals(ls_uuid, &pg_ls->key)) {
            return pg_ls;
        }
    }
    return NULL;
}

struct ovn_ls_port_group {
    struct hmap_node key_node;  /* Index on 'key'. */
    struct uuid key;            /* nb_pg->header_.uuid. */
    const struct nbrec_port_group *nb_pg;
};

static void
ovn_ls_port_group_add(struct hmap *nb_pgs,
                      const struct nbrec_port_group *nb_pg)
{
    struct ovn_ls_port_group *ls_pg = xzalloc(sizeof *ls_pg);
    ls_pg->key = nb_pg->header_.uuid;
    ls_pg->nb_pg = nb_pg;
    hmap_insert(nb_pgs, &ls_pg->key_node, uuid_hash(&ls_pg->key));
}

static void
ovn_ls_port_group_destroy(struct hmap *nb_pgs)
{
    struct ovn_ls_port_group *ls_pg;
    HMAP_FOR_EACH_POP (ls_pg, key_node, nb_pgs) {
        free(ls_pg);
    }
    hmap_destroy(nb_pgs);
}

static bool
has_stateful_acl(struct ovn_datapath *od)
{
    for (size_t i = 0; i < od->nbs->n_acls; i++) {
        struct nbrec_acl *acl = od->nbs->acls[i];
        if (!strcmp(acl->action, "allow-related")) {
            return true;
        }
    }

    struct ovn_ls_port_group *ls_pg;
    HMAP_FOR_EACH (ls_pg, key_node, &od->nb_pgs) {
        for (size_t i = 0; i < ls_pg->nb_pg->n_acls; i++) {
            struct nbrec_acl *acl = ls_pg->nb_pg->acls[i];
            if (!strcmp(acl->action, "allow-related")) {
                return true;
            }
        }
    }

    return false;
}

static void
build_lswitch_input_port_sec(struct hmap *ports, struct hmap *datapaths,
                             struct hmap *lflows)
{
    /* Logical switch ingress table 0: Ingress port security - L2
     *  (priority 50).
     *  Ingress table 1: Ingress port security - IP (priority 90 and 80)
     *  Ingress table 2: Ingress port security - ND (priority 90 and 80)
     */
    struct ds actions = DS_EMPTY_INITIALIZER;
    struct ds match = DS_EMPTY_INITIALIZER;
    struct ovn_port *op;

    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp) {
            continue;
        }

        if (!lsp_is_enabled(op->nbsp)) {
            /* Drop packets from disabled logical ports (since logical flow
             * tables are default-drop). */
            continue;
        }

        if (lsp_is_external(op->nbsp)) {
            continue;
        }

        ds_clear(&match);
        ds_clear(&actions);
        ds_put_format(&match, "inport == %s", op->json_key);
        build_port_security_l2("eth.src", op->ps_addrs, op->n_ps_addrs,
                               &match);

        const char *queue_id = smap_get(&op->sb->options, "qdisc_queue_id");
        if (queue_id) {
            ds_put_format(&actions, "set_queue(%s); ", queue_id);
        }
        ds_put_cstr(&actions, "next;");
        ovn_lflow_add(lflows, op->od, S_SWITCH_IN_PORT_SEC_L2, 50,
                      ds_cstr(&match), ds_cstr(&actions));

        if (op->nbsp->n_port_security) {
            build_port_security_ip(P_IN, op, lflows);
            build_port_security_nd(op, lflows);
        }
    }

    /* Ingress table 1 and 2: Port security - IP and ND, by default
     * goto next. (priority 0)
     */
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_SWITCH_IN_PORT_SEC_ND, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PORT_SEC_IP, 0, "1", "next;");
    }

    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
build_lswitch_output_port_sec(struct hmap *ports, struct hmap *datapaths,
                              struct hmap *lflows)
{
    struct ds actions = DS_EMPTY_INITIALIZER;
    struct ds match = DS_EMPTY_INITIALIZER;
    struct ovn_port *op;

    /* Egress table 8: Egress port security - IP (priorities 90 and 80)
     * if port security enabled.
     *
     * Egress table 9: Egress port security - L2 (priorities 50 and 150).
     *
     * Priority 50 rules implement port security for enabled logical port.
     *
     * Priority 150 rules drop packets to disabled logical ports, so that
     * they don't even receive multicast or broadcast packets.
     */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp || lsp_is_external(op->nbsp)) {
            continue;
        }

        ds_clear(&actions);
        ds_clear(&match);

        ds_put_format(&match, "outport == %s", op->json_key);
        if (lsp_is_enabled(op->nbsp)) {
            build_port_security_l2("eth.dst", op->ps_addrs, op->n_ps_addrs,
                                   &match);

            if (!strcmp(op->nbsp->type, "localnet")) {
                const char *queue_id = smap_get(&op->sb->options,
                                                "qdisc_queue_id");
                if (queue_id) {
                    ds_put_format(&actions, "set_queue(%s); ", queue_id);
                }
            }
            ds_put_cstr(&actions, "output;");
            ovn_lflow_add(lflows, op->od, S_SWITCH_OUT_PORT_SEC_L2, 50,
                              ds_cstr(&match), ds_cstr(&actions));
        } else {
            ovn_lflow_add(lflows, op->od, S_SWITCH_OUT_PORT_SEC_L2, 150,
                          ds_cstr(&match), "drop;");
        }

        if (op->nbsp->n_port_security) {
            build_port_security_ip(P_OUT, op, lflows);
        }
    }

    /* Egress tables 8: Egress port security - IP (priority 0)
     * Egress table 9: Egress port security L2 - multicast/broadcast
     *                 (priority 100). */
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_SWITCH_OUT_PORT_SEC_IP, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_PORT_SEC_L2, 100, "eth.mcast",
                      "output;");
    }

    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
build_pre_acls(struct ovn_datapath *od, struct hmap *lflows)
{
    bool has_stateful = has_stateful_acl(od);

    /* Ingress and Egress Pre-ACL Table (Priority 0): Packets are
     * allowed by default. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_ACL, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_ACL, 0, "1", "next;");

    /* If there are any stateful ACL rules in this datapath, we must
     * send all IP packets through the conntrack action, which handles
     * defragmentation, in order to match L4 headers. */
    if (has_stateful) {
        for (size_t i = 0; i < od->n_router_ports; i++) {
            struct ovn_port *op = od->router_ports[i];
            /* Can't use ct() for router ports. Consider the
             * following configuration: lp1(10.0.0.2) on
             * hostA--ls1--lr0--ls2--lp2(10.0.1.2) on hostB, For a
             * ping from lp1 to lp2, First, the response will go
             * through ct() with a zone for lp2 in the ls2 ingress
             * pipeline on hostB.  That ct zone knows about this
             * connection. Next, it goes through ct() with the zone
             * for the router port in the egress pipeline of ls2 on
             * hostB.  This zone does not know about the connection,
             * as the icmp request went through the logical router
             * on hostA, not hostB. This would only work with
             * distributed conntrack state across all chassis. */
            struct ds match_in = DS_EMPTY_INITIALIZER;
            struct ds match_out = DS_EMPTY_INITIALIZER;

            ds_put_format(&match_in, "ip && inport == %s", op->json_key);
            ds_put_format(&match_out, "ip && outport == %s", op->json_key);
            ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_ACL, 110,
                          ds_cstr(&match_in), "next;");
            ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_ACL, 110,
                          ds_cstr(&match_out), "next;");

            ds_destroy(&match_in);
            ds_destroy(&match_out);
        }
        if (od->localnet_port) {
            struct ds match_in = DS_EMPTY_INITIALIZER;
            struct ds match_out = DS_EMPTY_INITIALIZER;

            ds_put_format(&match_in, "ip && inport == %s",
                          od->localnet_port->json_key);
            ds_put_format(&match_out, "ip && outport == %s",
                          od->localnet_port->json_key);
            ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_ACL, 110,
                          ds_cstr(&match_in), "next;");
            ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_ACL, 110,
                          ds_cstr(&match_out), "next;");

            ds_destroy(&match_in);
            ds_destroy(&match_out);
        }

        /* Ingress and Egress Pre-ACL Table (Priority 110).
         *
         * Not to do conntrack on ND and ICMP destination
         * unreachable packets. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_ACL, 110,
                      "nd || nd_rs || nd_ra || icmp4.type == 3 || "
                      "icmp6.type == 1 || (tcp && tcp.flags == 4)",
                      "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_ACL, 110,
                      "nd || nd_rs || nd_ra || icmp4.type == 3 || "
                      "icmp6.type == 1 || (tcp && tcp.flags == 4)",
                      "next;");

        /* Ingress and Egress Pre-ACL Table (Priority 100).
         *
         * Regardless of whether the ACL is "from-lport" or "to-lport",
         * we need rules in both the ingress and egress table, because
         * the return traffic needs to be followed.
         *
         * 'REGBIT_CONNTRACK_DEFRAG' is set to let the pre-stateful table send
         * it to conntrack for tracking and defragmentation. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_ACL, 100, "ip",
                      REGBIT_CONNTRACK_DEFRAG" = 1; next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_ACL, 100, "ip",
                      REGBIT_CONNTRACK_DEFRAG" = 1; next;");
    }
}

/* For a 'key' of the form "IP:port" or just "IP", sets 'port' and
 * 'ip_address'.  The caller must free() the memory allocated for
 * 'ip_address'. */
static void
ip_address_and_port_from_lb_key(const char *key, char **ip_address,
                                uint16_t *port, int *addr_family)
{
    struct sockaddr_storage ss;
    if (!inet_parse_active(key, 0, &ss, false)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "bad ip address or port for load balancer key %s",
                     key);
        return;
    }

    struct ds s = DS_EMPTY_INITIALIZER;
    ss_format_address_nobracks(&ss, &s);
    *ip_address = ds_steal_cstr(&s);

    *port = ss_get_port(&ss);

    *addr_family = ss.ss_family;
}

/*
 * Returns true if logical switch is configured with DNS records, false
 * otherwise.
 */
static bool
ls_has_dns_records(const struct nbrec_logical_switch *nbs)
{
    for (size_t i = 0; i < nbs->n_dns_records; i++) {
        if (!smap_is_empty(&nbs->dns_records[i]->records)) {
            return true;
        }
    }

    return false;
}

static void
build_empty_lb_event_flow(struct ovn_datapath *od, struct hmap *lflows,
                          struct smap_node *node, char *ip_address,
                          struct nbrec_load_balancer *lb, uint16_t port,
                          int addr_family, int pl, struct shash *meter_groups)
{
    if (!controller_event_en || node->value[0]) {
        return;
    }

    struct ds match = DS_EMPTY_INITIALIZER;
    char *meter = "", *action;

    if (meter_groups && shash_find(meter_groups, "event-elb")) {
        meter = "event-elb";
    }

    if (addr_family == AF_INET) {
        ds_put_format(&match, "ip4.dst == %s && %s",
                      ip_address, lb->protocol);
    } else {
        ds_put_format(&match, "ip6.dst == %s && %s",
                      ip_address, lb->protocol);
    }
    if (port) {
        ds_put_format(&match, " && %s.dst == %u", lb->protocol,
                      port);
    }
    action = xasprintf("trigger_event(event = \"%s\", "
                       "meter = \"%s\", vip = \"%s\", "
                       "protocol = \"%s\", "
                       "load_balancer = \"" UUID_FMT "\");",
                       event_to_string(OVN_EVENT_EMPTY_LB_BACKENDS),
                       meter, node->key, lb->protocol,
                       UUID_ARGS(&lb->header_.uuid));
    ovn_lflow_add(lflows, od, pl, 130, ds_cstr(&match), action);
    ds_destroy(&match);
    free(action);
}

static void
build_pre_lb(struct ovn_datapath *od, struct hmap *lflows,
             struct shash *meter_groups)
{
    /* Do not send ND packets to conntrack */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_LB, 110,
                  "nd || nd_rs || nd_ra", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_LB, 110,
                  "nd || nd_rs || nd_ra", "next;");

    /* Allow all packets to go to next tables by default. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_LB, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_LB, 0, "1", "next;");

    struct sset all_ips = SSET_INITIALIZER(&all_ips);
    bool vip_configured = false;
    int addr_family = AF_INET;
    for (int i = 0; i < od->nbs->n_load_balancer; i++) {
        struct nbrec_load_balancer *lb = od->nbs->load_balancer[i];
        struct smap *vips = &lb->vips;
        struct smap_node *node;

        SMAP_FOR_EACH (node, vips) {
            vip_configured = true;

            /* node->key contains IP:port or just IP. */
            char *ip_address = NULL;
            uint16_t port;
            ip_address_and_port_from_lb_key(node->key, &ip_address, &port,
                                            &addr_family);
            if (!ip_address) {
                continue;
            }

            if (!sset_contains(&all_ips, ip_address)) {
                sset_add(&all_ips, ip_address);
            }

            build_empty_lb_event_flow(od, lflows, node, ip_address, lb,
                                      port, addr_family, S_SWITCH_IN_PRE_LB,
                                      meter_groups);

            free(ip_address);

            /* Ignore L4 port information in the key because fragmented packets
             * may not have L4 information.  The pre-stateful table will send
             * the packet through ct() action to de-fragment. In stateful
             * table, we will eventually look at L4 information. */
        }
    }

    /* 'REGBIT_CONNTRACK_DEFRAG' is set to let the pre-stateful table send
     * packet to conntrack for defragmentation. */
    const char *ip_address;
    SSET_FOR_EACH(ip_address, &all_ips) {
        char *match;

        if (addr_family == AF_INET) {
            match = xasprintf("ip && ip4.dst == %s", ip_address);
        } else {
            match = xasprintf("ip && ip6.dst == %s", ip_address);
        }
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_LB,
                      100, match, REGBIT_CONNTRACK_DEFRAG" = 1; next;");
        free(match);
    }

    sset_destroy(&all_ips);

    if (vip_configured) {
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_LB,
                      100, "ip", REGBIT_CONNTRACK_DEFRAG" = 1; next;");
    }
}

static void
build_pre_stateful(struct ovn_datapath *od, struct hmap *lflows)
{
    /* Ingress and Egress pre-stateful Table (Priority 0): Packets are
     * allowed by default. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_STATEFUL, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_STATEFUL, 0, "1", "next;");

    /* If REGBIT_CONNTRACK_DEFRAG is set as 1, then the packets should be
     * sent to conntrack for tracking and defragmentation. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_PRE_STATEFUL, 100,
                  REGBIT_CONNTRACK_DEFRAG" == 1", "ct_next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_PRE_STATEFUL, 100,
                  REGBIT_CONNTRACK_DEFRAG" == 1", "ct_next;");
}

static void
build_acl_log(struct ds *actions, const struct nbrec_acl *acl)
{
    if (!acl->log) {
        return;
    }

    ds_put_cstr(actions, "log(");

    if (acl->name) {
        ds_put_format(actions, "name=\"%s\", ", acl->name);
    }

    /* If a severity level isn't specified, default to "info". */
    if (acl->severity) {
        ds_put_format(actions, "severity=%s, ", acl->severity);
    } else {
        ds_put_format(actions, "severity=info, ");
    }

    if (!strcmp(acl->action, "drop")) {
        ds_put_cstr(actions, "verdict=drop, ");
    } else if (!strcmp(acl->action, "reject")) {
        ds_put_cstr(actions, "verdict=reject, ");
    } else if (!strcmp(acl->action, "allow")
        || !strcmp(acl->action, "allow-related")) {
        ds_put_cstr(actions, "verdict=allow, ");
    }

    if (acl->meter) {
        ds_put_format(actions, "meter=\"%s\", ", acl->meter);
    }

    ds_chomp(actions, ' ');
    ds_chomp(actions, ',');
    ds_put_cstr(actions, "); ");
}

static void
build_reject_acl_rules(struct ovn_datapath *od, struct hmap *lflows,
                       enum ovn_stage stage, struct nbrec_acl *acl,
                       struct ds *extra_match, struct ds *extra_actions)
{
    struct ds match = DS_EMPTY_INITIALIZER;
    struct ds actions = DS_EMPTY_INITIALIZER;
    bool ingress = (stage == S_SWITCH_IN_ACL);

    /* TCP */
    build_acl_log(&actions, acl);
    if (extra_match->length > 0) {
        ds_put_format(&match, "(%s) && ", extra_match->string);
    }
    ds_put_format(&match, "ip4 && tcp && (%s)", acl->match);
    ds_put_format(&actions, "reg0 = 0; "
                  "eth.dst <-> eth.src; ip4.dst <-> ip4.src; "
                  "tcp_reset { outport <-> inport; %s };",
                  ingress ? "output;" : "next(pipeline=ingress,table=0);");
    ovn_lflow_add(lflows, od, stage, acl->priority + OVN_ACL_PRI_OFFSET + 10,
                  ds_cstr(&match), ds_cstr(&actions));
    ds_clear(&match);
    ds_clear(&actions);
    build_acl_log(&actions, acl);
    if (extra_match->length > 0) {
        ds_put_format(&match, "(%s) && ", extra_match->string);
    }
    ds_put_format(&match, "ip6 && tcp && (%s)", acl->match);
    ds_put_format(&actions, "reg0 = 0; "
                  "eth.dst <-> eth.src; ip6.dst <-> ip6.src; "
                  "tcp_reset { outport <-> inport; %s };",
                  ingress ? "output;" : "next(pipeline=ingress,table=0);");
    ovn_lflow_add(lflows, od, stage, acl->priority + OVN_ACL_PRI_OFFSET + 10,
                  ds_cstr(&match), ds_cstr(&actions));

    /* IP traffic */
    ds_clear(&match);
    ds_clear(&actions);
    build_acl_log(&actions, acl);
    if (extra_match->length > 0) {
        ds_put_format(&match, "(%s) && ", extra_match->string);
    }
    ds_put_format(&match, "ip4 && (%s)", acl->match);
    if (extra_actions->length > 0) {
        ds_put_format(&actions, "%s ", extra_actions->string);
    }
    ds_put_format(&actions, "reg0 = 0; "
                  "eth.dst <-> eth.src; ip4.dst <-> ip4.src; "
                  "icmp4 { outport <-> inport; %s };",
                  ingress ? "output;" : "next(pipeline=ingress,table=0);");
    ovn_lflow_add(lflows, od, stage, acl->priority + OVN_ACL_PRI_OFFSET,
                  ds_cstr(&match), ds_cstr(&actions));
    ds_clear(&match);
    ds_clear(&actions);
    build_acl_log(&actions, acl);
    if (extra_match->length > 0) {
        ds_put_format(&match, "(%s) && ", extra_match->string);
    }
    ds_put_format(&match, "ip6 && (%s)", acl->match);
    if (extra_actions->length > 0) {
        ds_put_format(&actions, "%s ", extra_actions->string);
    }
    ds_put_format(&actions, "reg0 = 0; icmp6 { "
                  "eth.dst <-> eth.src; ip6.dst <-> ip6.src; "
                  "outport <-> inport; %s };",
                  ingress ? "output;" : "next(pipeline=ingress,table=0);");
    ovn_lflow_add(lflows, od, stage, acl->priority + OVN_ACL_PRI_OFFSET,
                  ds_cstr(&match), ds_cstr(&actions));

    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
consider_acl(struct hmap *lflows, struct ovn_datapath *od,
             struct nbrec_acl *acl, bool has_stateful)
{
    bool ingress = !strcmp(acl->direction, "from-lport") ? true :false;
    enum ovn_stage stage = ingress ? S_SWITCH_IN_ACL : S_SWITCH_OUT_ACL;

    char *stage_hint = xasprintf("%08x", acl->header_.uuid.parts[0]);
    if (!strcmp(acl->action, "allow")
        || !strcmp(acl->action, "allow-related")) {
        /* If there are any stateful flows, we must even commit "allow"
         * actions.  This is because, while the initiater's
         * direction may not have any stateful rules, the server's
         * may and then its return traffic would not have an
         * associated conntrack entry and would return "+invalid". */
        if (!has_stateful) {
            struct ds actions = DS_EMPTY_INITIALIZER;
            build_acl_log(&actions, acl);
            ds_put_cstr(&actions, "next;");
            ovn_lflow_add_with_hint(lflows, od, stage,
                                    acl->priority + OVN_ACL_PRI_OFFSET,
                                    acl->match, ds_cstr(&actions),
                                    stage_hint);
            ds_destroy(&actions);
        } else {
            struct ds match = DS_EMPTY_INITIALIZER;
            struct ds actions = DS_EMPTY_INITIALIZER;

            /* Commit the connection tracking entry if it's a new
             * connection that matches this ACL.  After this commit,
             * the reply traffic is allowed by a flow we create at
             * priority 65535, defined earlier.
             *
             * It's also possible that a known connection was marked for
             * deletion after a policy was deleted, but the policy was
             * re-added while that connection is still known.  We catch
             * that case here and un-set ct_label.blocked (which will be done
             * by ct_commit in the "stateful" stage) to indicate that the
             * connection should be allowed to resume.
             */
            ds_put_format(&match, "((ct.new && !ct.est)"
                                  " || (!ct.new && ct.est && !ct.rpl "
                                       "&& ct_label.blocked == 1)) "
                                  "&& (%s)", acl->match);
            ds_put_cstr(&actions, REGBIT_CONNTRACK_COMMIT" = 1; ");
            build_acl_log(&actions, acl);
            ds_put_cstr(&actions, "next;");
            ovn_lflow_add_with_hint(lflows, od, stage,
                                    acl->priority + OVN_ACL_PRI_OFFSET,
                                    ds_cstr(&match),
                                    ds_cstr(&actions),
                                    stage_hint);

            /* Match on traffic in the request direction for an established
             * connection tracking entry that has not been marked for
             * deletion.  There is no need to commit here, so we can just
             * proceed to the next table. We use this to ensure that this
             * connection is still allowed by the currently defined
             * policy. */
            ds_clear(&match);
            ds_clear(&actions);
            ds_put_format(&match,
                          "!ct.new && ct.est && !ct.rpl"
                          " && ct_label.blocked == 0 && (%s)",
                          acl->match);

            build_acl_log(&actions, acl);
            ds_put_cstr(&actions, "next;");
            ovn_lflow_add_with_hint(lflows, od, stage,
                                    acl->priority + OVN_ACL_PRI_OFFSET,
                                    ds_cstr(&match), ds_cstr(&actions),
                                    stage_hint);

            ds_destroy(&match);
            ds_destroy(&actions);
        }
    } else if (!strcmp(acl->action, "drop")
               || !strcmp(acl->action, "reject")) {
        struct ds match = DS_EMPTY_INITIALIZER;
        struct ds actions = DS_EMPTY_INITIALIZER;

        /* The implementation of "drop" differs if stateful ACLs are in
         * use for this datapath.  In that case, the actions differ
         * depending on whether the connection was previously committed
         * to the connection tracker with ct_commit. */
        if (has_stateful) {
            /* If the packet is not part of an established connection, then
             * we can simply reject/drop it. */
            ds_put_cstr(&match,
                        "(!ct.est || (ct.est && ct_label.blocked == 1))");
            if (!strcmp(acl->action, "reject")) {
                build_reject_acl_rules(od, lflows, stage, acl, &match,
                                       &actions);
            } else {
                ds_put_format(&match, " && (%s)", acl->match);
                build_acl_log(&actions, acl);
                ds_put_cstr(&actions, "/* drop */");
                ovn_lflow_add(lflows, od, stage,
                              acl->priority + OVN_ACL_PRI_OFFSET,
                              ds_cstr(&match), ds_cstr(&actions));
            }
            /* For an existing connection without ct_label set, we've
             * encountered a policy change. ACLs previously allowed
             * this connection and we committed the connection tracking
             * entry.  Current policy says that we should drop this
             * connection.  First, we set bit 0 of ct_label to indicate
             * that this connection is set for deletion.  By not
             * specifying "next;", we implicitly drop the packet after
             * updating conntrack state.  We would normally defer
             * ct_commit() to the "stateful" stage, but since we're
             * rejecting/dropping the packet, we go ahead and do it here.
             */
            ds_clear(&match);
            ds_clear(&actions);
            ds_put_cstr(&match, "ct.est && ct_label.blocked == 0");
            ds_put_cstr(&actions, "ct_commit(ct_label=1/1); ");
            if (!strcmp(acl->action, "reject")) {
                build_reject_acl_rules(od, lflows, stage, acl, &match,
                                       &actions);
            } else {
                ds_put_format(&match, " && (%s)", acl->match);
                build_acl_log(&actions, acl);
                ds_put_cstr(&actions, "/* drop */");
                ovn_lflow_add(lflows, od, stage,
                              acl->priority + OVN_ACL_PRI_OFFSET,
                              ds_cstr(&match), ds_cstr(&actions));
            }
        } else {
            /* There are no stateful ACLs in use on this datapath,
             * so a "reject/drop" ACL is simply the "reject/drop"
             * logical flow action in all cases. */
            if (!strcmp(acl->action, "reject")) {
                build_reject_acl_rules(od, lflows, stage, acl, &match,
                                       &actions);
            } else {
                build_acl_log(&actions, acl);
                ds_put_cstr(&actions, "/* drop */");
                ovn_lflow_add(lflows, od, stage,
                              acl->priority + OVN_ACL_PRI_OFFSET,
                              acl->match, ds_cstr(&actions));
            }
        }
        ds_destroy(&match);
        ds_destroy(&actions);
    }
    free(stage_hint);
}

static struct ovn_port_group *
ovn_port_group_create(struct hmap *pgs,
                      const struct nbrec_port_group *nb_pg)
{
    struct ovn_port_group *pg = xzalloc(sizeof *pg);
    pg->key = nb_pg->header_.uuid;
    pg->nb_pg = nb_pg;
    hmap_init(&pg->nb_lswitches);
    hmap_insert(pgs, &pg->key_node, uuid_hash(&pg->key));
    return pg;
}

static void
ovn_port_group_destroy(struct hmap *pgs, struct ovn_port_group *pg)
{
    if (pg) {
        hmap_remove(pgs, &pg->key_node);
        struct ovn_port_group_ls *ls;
        HMAP_FOR_EACH_POP (ls, key_node, &pg->nb_lswitches) {
            free(ls);
        }
        hmap_destroy(&pg->nb_lswitches);
        free(pg);
    }
}

static void
build_port_group_lswitches(struct northd_context *ctx, struct hmap *pgs,
                           struct hmap *ports)
{
    hmap_init(pgs);

    const struct nbrec_port_group *nb_pg;
    NBREC_PORT_GROUP_FOR_EACH (nb_pg, ctx->ovnnb_idl) {
        struct ovn_port_group *pg = ovn_port_group_create(pgs, nb_pg);
        for (size_t i = 0; i < nb_pg->n_ports; i++) {
            struct ovn_port *op = ovn_port_find(ports, nb_pg->ports[i]->name);
            if (!op) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_ERR_RL(&rl, "lport %s in port group %s not found.",
                            nb_pg->ports[i]->name,
                            nb_pg->name);
                continue;
            }

            if (!op->od->nbs) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_WARN_RL(&rl, "lport %s in port group %s has no lswitch.",
                             nb_pg->ports[i]->name,
                             nb_pg->name);
                continue;
            }

            struct ovn_port_group_ls *pg_ls =
                ovn_port_group_ls_find(pg, &op->od->nbs->header_.uuid);
            if (!pg_ls) {
                ovn_port_group_ls_add(pg, op->od->nbs);
                ovn_ls_port_group_add(&op->od->nb_pgs, nb_pg);
            }
        }
    }
}

static void
build_acls(struct ovn_datapath *od, struct hmap *lflows,
           struct hmap *port_groups)
{
    bool has_stateful = has_stateful_acl(od);

    /* Ingress and Egress ACL Table (Priority 0): Packets are allowed by
     * default.  A related rule at priority 1 is added below if there
     * are any stateful ACLs in this datapath. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, 0, "1", "next;");

    if (has_stateful) {
        /* Ingress and Egress ACL Table (Priority 1).
         *
         * By default, traffic is allowed.  This is partially handled by
         * the Priority 0 ACL flows added earlier, but we also need to
         * commit IP flows.  This is because, while the initiater's
         * direction may not have any stateful rules, the server's may
         * and then its return traffic would not have an associated
         * conntrack entry and would return "+invalid".
         *
         * We use "ct_commit" for a connection that is not already known
         * by the connection tracker.  Once a connection is committed,
         * subsequent packets will hit the flow at priority 0 that just
         * uses "next;"
         *
         * We also check for established connections that have ct_label.blocked
         * set on them.  That's a connection that was disallowed, but is
         * now allowed by policy again since it hit this default-allow flow.
         * We need to set ct_label.blocked=0 to let the connection continue,
         * which will be done by ct_commit() in the "stateful" stage.
         * Subsequent packets will hit the flow at priority 0 that just
         * uses "next;". */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, 1,
                      "ip && (!ct.est || (ct.est && ct_label.blocked == 1))",
                       REGBIT_CONNTRACK_COMMIT" = 1; next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, 1,
                      "ip && (!ct.est || (ct.est && ct_label.blocked == 1))",
                       REGBIT_CONNTRACK_COMMIT" = 1; next;");

        /* Ingress and Egress ACL Table (Priority 65535).
         *
         * Always drop traffic that's in an invalid state.  Also drop
         * reply direction packets for connections that have been marked
         * for deletion (bit 0 of ct_label is set).
         *
         * This is enforced at a higher priority than ACLs can be defined. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, UINT16_MAX,
                      "ct.inv || (ct.est && ct.rpl && ct_label.blocked == 1)",
                      "drop;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, UINT16_MAX,
                      "ct.inv || (ct.est && ct.rpl && ct_label.blocked == 1)",
                      "drop;");

        /* Ingress and Egress ACL Table (Priority 65535).
         *
         * Allow reply traffic that is part of an established
         * conntrack entry that has not been marked for deletion
         * (bit 0 of ct_label).  We only match traffic in the
         * reply direction because we want traffic in the request
         * direction to hit the currently defined policy from ACLs.
         *
         * This is enforced at a higher priority than ACLs can be defined. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, UINT16_MAX,
                      "ct.est && !ct.rel && !ct.new && !ct.inv "
                      "&& ct.rpl && ct_label.blocked == 0",
                      "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, UINT16_MAX,
                      "ct.est && !ct.rel && !ct.new && !ct.inv "
                      "&& ct.rpl && ct_label.blocked == 0",
                      "next;");

        /* Ingress and Egress ACL Table (Priority 65535).
         *
         * Allow traffic that is related to an existing conntrack entry that
         * has not been marked for deletion (bit 0 of ct_label).
         *
         * This is enforced at a higher priority than ACLs can be defined.
         *
         * NOTE: This does not support related data sessions (eg,
         * a dynamically negotiated FTP data channel), but will allow
         * related traffic such as an ICMP Port Unreachable through
         * that's generated from a non-listening UDP port.  */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, UINT16_MAX,
                      "!ct.est && ct.rel && !ct.new && !ct.inv "
                      "&& ct_label.blocked == 0",
                      "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, UINT16_MAX,
                      "!ct.est && ct.rel && !ct.new && !ct.inv "
                      "&& ct_label.blocked == 0",
                      "next;");

        /* Ingress and Egress ACL Table (Priority 65535).
         *
         * Not to do conntrack on ND packets. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_ACL, UINT16_MAX, "nd", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_ACL, UINT16_MAX, "nd", "next;");
    }

    /* Ingress or Egress ACL Table (Various priorities). */
    for (size_t i = 0; i < od->nbs->n_acls; i++) {
        struct nbrec_acl *acl = od->nbs->acls[i];
        consider_acl(lflows, od, acl, has_stateful);
    }
    struct ovn_port_group *pg;
    HMAP_FOR_EACH (pg, key_node, port_groups) {
        if (ovn_port_group_ls_find(pg, &od->nbs->header_.uuid)) {
            for (size_t i = 0; i < pg->nb_pg->n_acls; i++) {
                consider_acl(lflows, od, pg->nb_pg->acls[i], has_stateful);
            }
        }
    }

    /* Add 34000 priority flow to allow DHCP reply from ovn-controller to all
     * logical ports of the datapath if the CMS has configured DHCPv4 options.
     * */
    for (size_t i = 0; i < od->nbs->n_ports; i++) {
        if (lsp_is_external(od->nbs->ports[i])) {
            continue;
        }

        if (od->nbs->ports[i]->dhcpv4_options) {
            const char *server_id = smap_get(
                &od->nbs->ports[i]->dhcpv4_options->options, "server_id");
            const char *server_mac = smap_get(
                &od->nbs->ports[i]->dhcpv4_options->options, "server_mac");
            const char *lease_time = smap_get(
                &od->nbs->ports[i]->dhcpv4_options->options, "lease_time");
            if (server_id && server_mac && lease_time) {
                struct ds match = DS_EMPTY_INITIALIZER;
                const char *actions =
                    has_stateful ? "ct_commit; next;" : "next;";
                ds_put_format(&match, "outport == \"%s\" && eth.src == %s "
                              "&& ip4.src == %s && udp && udp.src == 67 "
                              "&& udp.dst == 68", od->nbs->ports[i]->name,
                              server_mac, server_id);
                ovn_lflow_add(
                    lflows, od, S_SWITCH_OUT_ACL, 34000, ds_cstr(&match),
                    actions);
                ds_destroy(&match);
            }
        }

        if (od->nbs->ports[i]->dhcpv6_options) {
            const char *server_mac = smap_get(
                &od->nbs->ports[i]->dhcpv6_options->options, "server_id");
            struct eth_addr ea;
            if (server_mac && eth_addr_from_string(server_mac, &ea)) {
                /* Get the link local IP of the DHCPv6 server from the
                 * server MAC. */
                struct in6_addr lla;
                in6_generate_lla(ea, &lla);

                char server_ip[INET6_ADDRSTRLEN + 1];
                ipv6_string_mapped(server_ip, &lla);

                struct ds match = DS_EMPTY_INITIALIZER;
                const char *actions = has_stateful ? "ct_commit; next;" :
                    "next;";
                ds_put_format(&match, "outport == \"%s\" && eth.src == %s "
                              "&& ip6.src == %s && udp && udp.src == 547 "
                              "&& udp.dst == 546", od->nbs->ports[i]->name,
                              server_mac, server_ip);
                ovn_lflow_add(
                    lflows, od, S_SWITCH_OUT_ACL, 34000, ds_cstr(&match),
                    actions);
                ds_destroy(&match);
            }
        }
    }

    /* Add a 34000 priority flow to advance the DNS reply from ovn-controller,
     * if the CMS has configured DNS records for the datapath.
     */
    if (ls_has_dns_records(od->nbs)) {
        const char *actions = has_stateful ? "ct_commit; next;" : "next;";
        ovn_lflow_add(
            lflows, od, S_SWITCH_OUT_ACL, 34000, "udp.src == 53",
            actions);
    }
}

static void
build_qos(struct ovn_datapath *od, struct hmap *lflows) {
    ovn_lflow_add(lflows, od, S_SWITCH_IN_QOS_MARK, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_QOS_MARK, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_IN_QOS_METER, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_QOS_METER, 0, "1", "next;");

    for (size_t i = 0; i < od->nbs->n_qos_rules; i++) {
        struct nbrec_qos *qos = od->nbs->qos_rules[i];
        bool ingress = !strcmp(qos->direction, "from-lport") ? true :false;
        enum ovn_stage stage = ingress ? S_SWITCH_IN_QOS_MARK : S_SWITCH_OUT_QOS_MARK;
        int64_t rate = 0;
        int64_t burst = 0;

        for (size_t j = 0; j < qos->n_action; j++) {
            if (!strcmp(qos->key_action[j], "dscp")) {
                struct ds dscp_action = DS_EMPTY_INITIALIZER;

                ds_put_format(&dscp_action, "ip.dscp = %"PRId64"; next;",
                              qos->value_action[j]);
                ovn_lflow_add(lflows, od, stage,
                              qos->priority,
                              qos->match, ds_cstr(&dscp_action));
                ds_destroy(&dscp_action);
            }
        }

        for (size_t n = 0; n < qos->n_bandwidth; n++) {
            if (!strcmp(qos->key_bandwidth[n], "rate")) {
                rate = qos->value_bandwidth[n];
            } else if (!strcmp(qos->key_bandwidth[n], "burst")) {
                burst = qos->value_bandwidth[n];
            }
        }
        if (rate) {
            struct ds meter_action = DS_EMPTY_INITIALIZER;
            stage = ingress ? S_SWITCH_IN_QOS_METER : S_SWITCH_OUT_QOS_METER;
            if (burst) {
                ds_put_format(&meter_action,
                              "set_meter(%"PRId64", %"PRId64"); next;",
                              rate, burst);
            } else {
                ds_put_format(&meter_action,
                              "set_meter(%"PRId64"); next;",
                              rate);
            }

            /* Ingress and Egress QoS Meter Table.
             *
             * We limit the bandwidth of this flow by adding a meter table.
             */
            ovn_lflow_add(lflows, od, stage,
                          qos->priority,
                          qos->match, ds_cstr(&meter_action));
            ds_destroy(&meter_action);
        }
    }
}

static void
build_lb(struct ovn_datapath *od, struct hmap *lflows)
{
    /* Ingress and Egress LB Table (Priority 0): Packets are allowed by
     * default.  */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_LB, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_LB, 0, "1", "next;");

    if (od->nbs->load_balancer) {
        /* Ingress and Egress LB Table (Priority 65535).
         *
         * Send established traffic through conntrack for just NAT. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_LB, UINT16_MAX,
                      "ct.est && !ct.rel && !ct.new && !ct.inv",
                      REGBIT_CONNTRACK_NAT" = 1; next;");
        ovn_lflow_add(lflows, od, S_SWITCH_OUT_LB, UINT16_MAX,
                      "ct.est && !ct.rel && !ct.new && !ct.inv",
                      REGBIT_CONNTRACK_NAT" = 1; next;");
    }
}

static void
build_stateful(struct ovn_datapath *od, struct hmap *lflows)
{
    /* Ingress and Egress stateful Table (Priority 0): Packets are
     * allowed by default. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_STATEFUL, 0, "1", "next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_STATEFUL, 0, "1", "next;");

    /* If REGBIT_CONNTRACK_COMMIT is set as 1, then the packets should be
     * committed to conntrack. We always set ct_label.blocked to 0 here as
     * any packet that makes it this far is part of a connection we
     * want to allow to continue. */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_STATEFUL, 100,
                  REGBIT_CONNTRACK_COMMIT" == 1", "ct_commit(ct_label=0/1); next;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_STATEFUL, 100,
                  REGBIT_CONNTRACK_COMMIT" == 1", "ct_commit(ct_label=0/1); next;");

    /* If REGBIT_CONNTRACK_NAT is set as 1, then packets should just be sent
     * through nat (without committing).
     *
     * REGBIT_CONNTRACK_COMMIT is set for new connections and
     * REGBIT_CONNTRACK_NAT is set for established connections. So they
     * don't overlap.
     */
    ovn_lflow_add(lflows, od, S_SWITCH_IN_STATEFUL, 100,
                  REGBIT_CONNTRACK_NAT" == 1", "ct_lb;");
    ovn_lflow_add(lflows, od, S_SWITCH_OUT_STATEFUL, 100,
                  REGBIT_CONNTRACK_NAT" == 1", "ct_lb;");

    /* Load balancing rules for new connections get committed to conntrack
     * table.  So even if REGBIT_CONNTRACK_COMMIT is set in a previous table
     * a higher priority rule for load balancing below also commits the
     * connection, so it is okay if we do not hit the above match on
     * REGBIT_CONNTRACK_COMMIT. */
    for (int i = 0; i < od->nbs->n_load_balancer; i++) {
        struct nbrec_load_balancer *lb = od->nbs->load_balancer[i];
        struct smap *vips = &lb->vips;
        struct smap_node *node;

        SMAP_FOR_EACH (node, vips) {
            uint16_t port = 0;
            int addr_family;

            /* node->key contains IP:port or just IP. */
            char *ip_address = NULL;
            ip_address_and_port_from_lb_key(node->key, &ip_address, &port,
                                            &addr_family);
            if (!ip_address) {
                continue;
            }

            /* New connections in Ingress table. */
            char *action = xasprintf("ct_lb(%s);", node->value);
            struct ds match = DS_EMPTY_INITIALIZER;
            if (addr_family == AF_INET) {
                ds_put_format(&match, "ct.new && ip4.dst == %s", ip_address);
            } else {
                ds_put_format(&match, "ct.new && ip6.dst == %s", ip_address);
            }
            if (port) {
                if (lb->protocol && !strcmp(lb->protocol, "udp")) {
                    ds_put_format(&match, " && udp.dst == %d", port);
                } else {
                    ds_put_format(&match, " && tcp.dst == %d", port);
                }
                ovn_lflow_add(lflows, od, S_SWITCH_IN_STATEFUL,
                              120, ds_cstr(&match), action);
            } else {
                ovn_lflow_add(lflows, od, S_SWITCH_IN_STATEFUL,
                              110, ds_cstr(&match), action);
            }

            free(ip_address);
            ds_destroy(&match);
            free(action);
       }
    }
}

static void
build_lrouter_groups__(struct hmap *ports, struct ovn_datapath *od)
{
    ovs_assert((od && od->nbr && od->lr_group));

    if (od->l3dgw_port && od->l3redirect_port) {
        /* It's a logical router with gateway port. If it
         * has HA_Chassis_Group associated to it in SB DB, then store the
         * ha chassis group name. */
        if (od->l3redirect_port->sb->ha_chassis_group) {
            sset_add(&od->lr_group->ha_chassis_groups,
                     od->l3redirect_port->sb->ha_chassis_group->name);
        }
    }

    for (size_t i = 0; i < od->nbr->n_ports; i++) {
        struct ovn_port *router_port =
            ovn_port_find(ports, od->nbr->ports[i]->name);

        if (!router_port || !router_port->peer) {
            continue;
        }

        /* Get the peer logical switch/logical router datapath. */
        struct ovn_datapath *peer_dp = router_port->peer->od;
        if (peer_dp->nbr) {
            if (!peer_dp->lr_group) {
                peer_dp->lr_group = od->lr_group;
                od->lr_group->router_dps[od->lr_group->n_router_dps++]
                    = peer_dp;
                build_lrouter_groups__(ports, peer_dp);
            }
        } else {
            for (size_t j = 0; j < peer_dp->n_router_ports; j++) {
                if (!peer_dp->router_ports[j]->peer) {
                    /* If there is no peer port connecting to the
                    * router port, ignore it. */
                    continue;
                }

                struct ovn_datapath *router_dp;
                router_dp = peer_dp->router_ports[j]->peer->od;
                if (router_dp == od) {
                    continue;
                }

                if (router_dp->lr_group == od->lr_group) {
                    /* 'router_dp' and 'od' already belong to the same
                    * lrouter group. Nothing to be done. */
                    continue;
                }

                router_dp->lr_group = od->lr_group;
                od->lr_group->router_dps[od->lr_group->n_router_dps++]
                    = router_dp;
                build_lrouter_groups__(ports, router_dp);
            }
        }
    }
}

/* Adds each logical router into a logical router group. All the
 * logical routers which belong to a group are connected to
 * each other either directly or indirectly (via transit logical switches
 * in between).
 *
 * Suppose if 'lr_list' has lr0, lr1, lr2, lr3, lr4, lr5
 * and the topology is like
 *  sw0 <-> lr0 <-> sw1 <-> lr1 <->sw2 <-> lr2
 *  sw3 <-> lr3 <-> lr4 <-> sw5
 *  sw6 <-> lr5 <-> sw7
 * Then 3 groups are created.
 * Group 1 -> lr0, lr1 and lr2
 *            lr0, lr1 and lr2's ovn_datapath->lr_group will point to this
 *            group. This means sw0's logical ports can send packets to sw2's
 *            logical ports if proper static route's are added.
 * Group 2 -> lr3 and lr4
 *            lr3 and lr4's ovn_datapath->lr_group will point to this group.
 * Group 3 -> lr5
 *
 * Each logical router can belong to only one group.
 */
static void
build_lrouter_groups(struct hmap *ports, struct ovs_list *lr_list)
{
    struct ovn_datapath *od;
    size_t n_router_dps = ovs_list_size(lr_list);

    LIST_FOR_EACH (od, lr_list, lr_list) {
        if (!od->lr_group) {
            od->lr_group = xzalloc(sizeof *od->lr_group);
            /* Each logical router group can have max
             * 'n_router_dps'. So allocate enough memory. */
            od->lr_group->router_dps = xcalloc(sizeof *od, n_router_dps);
            od->lr_group->router_dps[0] = od;
            od->lr_group->n_router_dps = 1;
            sset_init(&od->lr_group->ha_chassis_groups);
            build_lrouter_groups__(ports, od);
        }
    }
}

static void
build_lswitch_flows(struct hmap *datapaths, struct hmap *ports,
                    struct hmap *port_groups, struct hmap *lflows,
                    struct hmap *mcgroups, struct hmap *igmp_groups,
                    struct shash *meter_groups)
{
    /* This flow table structure is documented in ovn-northd(8), so please
     * update ovn-northd.8.xml if you change anything. */

    struct ds match = DS_EMPTY_INITIALIZER;
    struct ds actions = DS_EMPTY_INITIALIZER;

    /* Build pre-ACL and ACL tables for both ingress and egress.
     * Ingress tables 3 through 10.  Egress tables 0 through 7. */
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        build_pre_acls(od, lflows);
        build_pre_lb(od, lflows, meter_groups);
        build_pre_stateful(od, lflows);
        build_acls(od, lflows, port_groups);
        build_qos(od, lflows);
        build_lb(od, lflows);
        build_stateful(od, lflows);
    }

    /* Logical switch ingress table 0: Admission control framework (priority
     * 100). */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        /* Logical VLANs not supported. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PORT_SEC_L2, 100, "vlan.present",
                      "drop;");

        /* Broadcast/multicast source address is invalid. */
        ovn_lflow_add(lflows, od, S_SWITCH_IN_PORT_SEC_L2, 100, "eth.src[40]",
                      "drop;");

        /* Port security flows have priority 50 (see below) and will continue
         * to the next table if packet source is acceptable. */
    }

    build_lswitch_input_port_sec(ports, datapaths, lflows);

    /* Ingress table 11: ARP/ND responder, skip requests coming from localnet
     * and vtep ports. (priority 100); see ovn-northd.8.xml for the
     * rationale. */
    struct ovn_port *op;
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp) {
            continue;
        }

        if ((!strcmp(op->nbsp->type, "localnet")) ||
            (!strcmp(op->nbsp->type, "vtep"))) {
            ds_clear(&match);
            ds_put_format(&match, "inport == %s", op->json_key);
            ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 100,
                          ds_cstr(&match), "next;");
        }
    }

    /* Ingress table 11: ARP/ND responder, reply for known IPs.
     * (priority 50). */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp) {
            continue;
        }

        if (!strcmp(op->nbsp->type, "virtual")) {
            /* Handle
             *  - GARPs for virtual ip which belongs to a logical port
             *    of type 'virtual' and bind that port.
             *
             *  - ARP reply from the virtual ip which belongs to a logical
             *    port of type 'virtual' and bind that port.
             * */
            ovs_be32 ip;
            const char *virtual_ip = smap_get(&op->nbsp->options,
                                              "virtual-ip");
            const char *virtual_parents = smap_get(&op->nbsp->options,
                                                   "virtual-parents");
            if (!virtual_ip || !virtual_parents ||
                !ip_parse(virtual_ip, &ip)) {
                continue;
            }

            char *tokstr = xstrdup(virtual_parents);
            char *save_ptr = NULL;
            char *vparent;
            for (vparent = strtok_r(tokstr, ",", &save_ptr); vparent != NULL;
                 vparent = strtok_r(NULL, ",", &save_ptr)) {
                struct ovn_port *vp = ovn_port_find(ports, vparent);
                if (!vp || vp->od != op->od) {
                    /* vparent name should be valid and it should belong
                     * to the same logical switch. */
                    continue;
                }

                ds_clear(&match);
                ds_put_format(&match, "inport == \"%s\" && "
                              "((arp.op == 1 && arp.spa == %s && "
                              "arp.tpa == %s) || (arp.op == 2 && "
                              "arp.spa == %s))",
                              vparent, virtual_ip, virtual_ip,
                              virtual_ip);
                ds_clear(&actions);
                ds_put_format(&actions,
                    "bind_vport(%s, inport); "
                    "next;",
                    op->json_key);
                ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 100,
                              ds_cstr(&match), ds_cstr(&actions));
            }

            free(tokstr);
        } else {
            /*
             * Add ARP/ND reply flows if either the
             *  - port is up or
             *  - port type is router or
             *  - port type is localport
             */
            if (!lsp_is_up(op->nbsp) && strcmp(op->nbsp->type, "router") &&
                strcmp(op->nbsp->type, "localport")) {
                continue;
            }

            if (lsp_is_external(op->nbsp)) {
                continue;
            }

            for (size_t i = 0; i < op->n_lsp_addrs; i++) {
                for (size_t j = 0; j < op->lsp_addrs[i].n_ipv4_addrs; j++) {
                    ds_clear(&match);
                    ds_put_format(&match, "arp.tpa == %s && arp.op == 1",
                                op->lsp_addrs[i].ipv4_addrs[j].addr_s);
                    ds_clear(&actions);
                    ds_put_format(&actions,
                        "eth.dst = eth.src; "
                        "eth.src = %s; "
                        "arp.op = 2; /* ARP reply */ "
                        "arp.tha = arp.sha; "
                        "arp.sha = %s; "
                        "arp.tpa = arp.spa; "
                        "arp.spa = %s; "
                        "outport = inport; "
                        "flags.loopback = 1; "
                        "output;",
                        op->lsp_addrs[i].ea_s, op->lsp_addrs[i].ea_s,
                        op->lsp_addrs[i].ipv4_addrs[j].addr_s);
                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 50,
                                ds_cstr(&match), ds_cstr(&actions));

                    /* Do not reply to an ARP request from the port that owns
                     * the address (otherwise a DHCP client that ARPs to check
                     * for a duplicate address will fail).  Instead, forward
                     * it the usual way.
                     *
                     * (Another alternative would be to simply drop the packet.
                     * If everything is working as it is configured, then this
                     * would produce equivalent results, since no one should
                     * reply to the request.  But ARPing for one's own IP
                     * address is intended to detect situations where the
                     * network is not working as configured, so dropping the
                     * request would frustrate that intent.) */
                    ds_put_format(&match, " && inport == %s", op->json_key);
                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 100,
                                ds_cstr(&match), "next;");
                }

                /* For ND solicitations, we need to listen for both the
                 * unicast IPv6 address and its all-nodes multicast address,
                 * but always respond with the unicast IPv6 address. */
                for (size_t j = 0; j < op->lsp_addrs[i].n_ipv6_addrs; j++) {
                    ds_clear(&match);
                    ds_put_format(&match,
                            "nd_ns && ip6.dst == {%s, %s} && nd.target == %s",
                            op->lsp_addrs[i].ipv6_addrs[j].addr_s,
                            op->lsp_addrs[i].ipv6_addrs[j].sn_addr_s,
                            op->lsp_addrs[i].ipv6_addrs[j].addr_s);

                    ds_clear(&actions);
                    ds_put_format(&actions,
                            "%s { "
                            "eth.src = %s; "
                            "ip6.src = %s; "
                            "nd.target = %s; "
                            "nd.tll = %s; "
                            "outport = inport; "
                            "flags.loopback = 1; "
                            "output; "
                            "};",
                            !strcmp(op->nbsp->type, "router") ?
                                "nd_na_router" : "nd_na",
                            op->lsp_addrs[i].ea_s,
                            op->lsp_addrs[i].ipv6_addrs[j].addr_s,
                            op->lsp_addrs[i].ipv6_addrs[j].addr_s,
                            op->lsp_addrs[i].ea_s);
                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 50,
                                ds_cstr(&match), ds_cstr(&actions));

                    /* Do not reply to a solicitation from the port that owns
                     * the address (otherwise DAD detection will fail). */
                    ds_put_format(&match, " && inport == %s", op->json_key);
                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_ARP_ND_RSP, 100,
                                ds_cstr(&match), "next;");
                }
            }
        }
    }

    /* Ingress table 11: ARP/ND responder, by default goto next.
     * (priority 0)*/
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_SWITCH_IN_ARP_ND_RSP, 0, "1", "next;");
    }

    /* Logical switch ingress table 12 and 13: DHCP options and response
         * priority 100 flows. */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp) {
           continue;
        }

        if (!lsp_is_enabled(op->nbsp) || !strcmp(op->nbsp->type, "router")) {
            /* Don't add the DHCP flows if the port is not enabled or if the
             * port is a router port. */
            continue;
        }

        if (!op->nbsp->dhcpv4_options && !op->nbsp->dhcpv6_options) {
            /* CMS has disabled both native DHCPv4 and DHCPv6 for this lport.
             */
            continue;
        }

        bool is_external = lsp_is_external(op->nbsp);
        if (is_external && (!op->od->localnet_port ||
                            !op->nbsp->ha_chassis_group)) {
            /* If it's an external port and there is no localnet port
             * and if it doesn't belong to an HA chassis group ignore it. */
            continue;
        }

        for (size_t i = 0; i < op->n_lsp_addrs; i++) {
            for (size_t j = 0; j < op->lsp_addrs[i].n_ipv4_addrs; j++) {
                struct ds options_action = DS_EMPTY_INITIALIZER;
                struct ds response_action = DS_EMPTY_INITIALIZER;
                struct ds ipv4_addr_match = DS_EMPTY_INITIALIZER;
                if (build_dhcpv4_action(
                        op, op->lsp_addrs[i].ipv4_addrs[j].addr,
                        &options_action, &response_action, &ipv4_addr_match)) {
                    ds_clear(&match);
                    ds_put_format(
                        &match, "inport == %s && eth.src == %s && "
                        "ip4.src == 0.0.0.0 && ip4.dst == 255.255.255.255 && "
                        "udp.src == 68 && udp.dst == 67",
                        is_external ? op->od->localnet_port->json_key :
                            op->json_key,
                        op->lsp_addrs[i].ea_s);

                    if (is_external) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->json_key);
                    }

                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_DHCP_OPTIONS,
                                  100, ds_cstr(&match),
                                  ds_cstr(&options_action));
                    ds_clear(&match);
                    /* Allow ip4.src = OFFER_IP and
                     * ip4.dst = {SERVER_IP, 255.255.255.255} for the below
                     * cases
                     *  -  When the client wants to renew the IP by sending
                     *     the DHCPREQUEST to the server ip.
                     *  -  When the client wants to renew the IP by
                     *     broadcasting the DHCPREQUEST.
                     */
                    ds_put_format(
                        &match, "inport == %s && eth.src == %s && "
                        "%s && udp.src == 68 && udp.dst == 67",
                        is_external ? op->od->localnet_port->json_key :
                            op->json_key,
                        op->lsp_addrs[i].ea_s, ds_cstr(&ipv4_addr_match));

                    if (is_external) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->json_key);
                    }

                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_DHCP_OPTIONS,
                                  100, ds_cstr(&match),
                                  ds_cstr(&options_action));
                    ds_clear(&match);

                    /* If REGBIT_DHCP_OPTS_RESULT is set, it means the
                     * put_dhcp_opts action  is successful. */
                    ds_put_format(
                        &match, "inport == %s && eth.src == %s && "
                        "ip4 && udp.src == 68 && udp.dst == 67"
                        " && "REGBIT_DHCP_OPTS_RESULT,
                        is_external ? op->od->localnet_port->json_key :
                            op->json_key,
                        op->lsp_addrs[i].ea_s);

                    if (is_external) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->json_key);
                    }

                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_DHCP_RESPONSE,
                                  100, ds_cstr(&match),
                                  ds_cstr(&response_action));
                    ds_destroy(&options_action);
                    ds_destroy(&response_action);
                    ds_destroy(&ipv4_addr_match);
                    break;
                }
            }

            for (size_t j = 0; j < op->lsp_addrs[i].n_ipv6_addrs; j++) {
                struct ds options_action = DS_EMPTY_INITIALIZER;
                struct ds response_action = DS_EMPTY_INITIALIZER;
                if (build_dhcpv6_action(
                        op, &op->lsp_addrs[i].ipv6_addrs[j].addr,
                        &options_action, &response_action)) {
                    ds_clear(&match);
                    ds_put_format(
                        &match, "inport == %s && eth.src == %s"
                        " && ip6.dst == ff02::1:2 && udp.src == 546 &&"
                        " udp.dst == 547",
                        is_external ? op->od->localnet_port->json_key :
                            op->json_key,
                        op->lsp_addrs[i].ea_s);

                    if (is_external) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->json_key);
                    }

                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_DHCP_OPTIONS, 100,
                                  ds_cstr(&match), ds_cstr(&options_action));

                    /* If REGBIT_DHCP_OPTS_RESULT is set to 1, it means the
                     * put_dhcpv6_opts action is successful */
                    ds_put_cstr(&match, " && "REGBIT_DHCP_OPTS_RESULT);
                    ovn_lflow_add(lflows, op->od, S_SWITCH_IN_DHCP_RESPONSE, 100,
                                  ds_cstr(&match), ds_cstr(&response_action));
                    ds_destroy(&options_action);
                    ds_destroy(&response_action);
                    break;
                }
            }
        }
    }

    /* Logical switch ingress table 14 and 15: DNS lookup and response
     * priority 100 flows.
     */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs || !ls_has_dns_records(od->nbs)) {
           continue;
        }

        struct ds action = DS_EMPTY_INITIALIZER;

        ds_clear(&match);
        ds_put_cstr(&match, "udp.dst == 53");
        ds_put_format(&action,
                      REGBIT_DNS_LOOKUP_RESULT" = dns_lookup(); next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DNS_LOOKUP, 100,
                      ds_cstr(&match), ds_cstr(&action));
        ds_clear(&action);
        ds_put_cstr(&match, " && "REGBIT_DNS_LOOKUP_RESULT);
        ds_put_format(&action, "eth.dst <-> eth.src; ip4.src <-> ip4.dst; "
                      "udp.dst = udp.src; udp.src = 53; outport = inport; "
                      "flags.loopback = 1; output;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DNS_RESPONSE, 100,
                      ds_cstr(&match), ds_cstr(&action));
        ds_clear(&action);
        ds_put_format(&action, "eth.dst <-> eth.src; ip6.src <-> ip6.dst; "
                      "udp.dst = udp.src; udp.src = 53; outport = inport; "
                      "flags.loopback = 1; output;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DNS_RESPONSE, 100,
                      ds_cstr(&match), ds_cstr(&action));
        ds_destroy(&action);
    }

    /* Ingress table 12 and 13: DHCP options and response, by default goto
     * next. (priority 0).
     * Ingress table 14 and 15: DNS lookup and response, by default goto next.
     * (priority 0).
     * Ingress table 16 - External port handling, by default goto next.
     * (priority 0). */

    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_SWITCH_IN_DHCP_OPTIONS, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DHCP_RESPONSE, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DNS_LOOKUP, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_DNS_RESPONSE, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_SWITCH_IN_EXTERNAL_PORT, 0, "1", "next;");
    }

    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp || !lsp_is_external(op->nbsp) ||
            !op->od->localnet_port) {
           continue;
        }

        /* Table 16: External port. Drop ARP request for router ips from
         * external ports  on chassis not binding those ports.
         * This makes the router pipeline to be run only on the chassis
         * binding the external ports. */

        for (size_t i = 0; i < op->n_lsp_addrs; i++) {
            for (size_t j = 0; j < op->od->n_router_ports; j++) {
                struct ovn_port *rp = op->od->router_ports[j];
                for (size_t k = 0; k < rp->n_lsp_addrs; k++) {
                    for (size_t l = 0; l < rp->lsp_addrs[k].n_ipv4_addrs;
                         l++) {
                        ds_clear(&match);
                        ds_put_format(
                            &match, "inport == %s && eth.src == %s"
                            " && !is_chassis_resident(%s)"
                            " && arp.tpa == %s && arp.op == 1",
                            op->od->localnet_port->json_key,
                            op->lsp_addrs[i].ea_s, op->json_key,
                            rp->lsp_addrs[k].ipv4_addrs[l].addr_s);
                        ovn_lflow_add(lflows, op->od,
                                      S_SWITCH_IN_EXTERNAL_PORT, 100,
                                      ds_cstr(&match), "drop;");
                    }
                    for (size_t l = 0; l < rp->lsp_addrs[k].n_ipv6_addrs;
                         l++) {
                        ds_clear(&match);
                        ds_put_format(
                            &match, "inport == %s && eth.src == %s"
                            " && !is_chassis_resident(%s)"
                            " && nd_ns && ip6.dst == {%s, %s} && "
                            "nd.target == %s",
                            op->od->localnet_port->json_key,
                            op->lsp_addrs[i].ea_s, op->json_key,
                            rp->lsp_addrs[k].ipv6_addrs[l].addr_s,
                            rp->lsp_addrs[k].ipv6_addrs[l].sn_addr_s,
                            rp->lsp_addrs[k].ipv6_addrs[l].addr_s);
                        ovn_lflow_add(lflows, op->od,
                                      S_SWITCH_IN_EXTERNAL_PORT, 100,
                                      ds_cstr(&match), "drop;");
                    }
                }
            }
        }
    }

    /* Ingress table 17: Destination lookup, broadcast and multicast handling
     * (priority 70 - 100). */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        struct mcast_switch_info *mcast_sw_info = &od->mcast_info.sw;

        if (mcast_sw_info->enabled) {
            ds_clear(&actions);
            if (mcast_sw_info->flood_reports) {
                ds_put_cstr(&actions,
                            "clone { "
                                "outport = \""MC_MROUTER_STATIC"\"; "
                                "output; "
                            "};");
            }
            ds_put_cstr(&actions, "igmp;");
            /* Punt IGMP traffic to controller. */
            ovn_lflow_add(lflows, od, S_SWITCH_IN_L2_LKUP, 100,
                          "ip4 && ip.proto == 2", ds_cstr(&actions));

            /* Flood all IP multicast traffic destined to 224.0.0.X to all
             * ports - RFC 4541, section 2.1.2, item 2.
             */
            ovn_lflow_add(lflows, od, S_SWITCH_IN_L2_LKUP, 85,
                          "ip4 && ip4.dst == 224.0.0.0/24",
                          "outport = \""MC_FLOOD"\"; output;");

            /* Forward uregistered IP multicast to routers with relay enabled
             * and to any ports configured to flood IP multicast traffic.
             * If configured to flood unregistered traffic this will be
             * handled by the L2 multicast flow.
             */
            if (!mcast_sw_info->flood_unregistered) {
                ds_clear(&actions);

                if (mcast_sw_info->flood_relay) {
                    ds_put_cstr(&actions,
                                "clone { "
                                    "outport = \""MC_MROUTER_FLOOD"\"; "
                                    "output; "
                                "}; ");
                }

                if (mcast_sw_info->flood_static) {
                    ds_put_cstr(&actions, "outport =\""MC_STATIC"\"; output;");
                }

                /* Explicitly drop the traffic if relay or static flooding
                 * is not configured.
                 */
                if (!mcast_sw_info->flood_relay &&
                        !mcast_sw_info->flood_static) {
                    ds_put_cstr(&actions, "drop;");
                }

                ovn_lflow_add(lflows, od, S_SWITCH_IN_L2_LKUP, 80,
                              "ip4 && ip4.mcast", ds_cstr(&actions));
            }
        }

        ovn_lflow_add(lflows, od, S_SWITCH_IN_L2_LKUP, 70, "eth.mcast",
                      "outport = \""MC_FLOOD"\"; output;");
    }

    /* Ingress table 17: Add IP multicast flows learnt from IGMP
     * (priority 90). */
    struct ovn_igmp_group *igmp_group;

    HMAP_FOR_EACH (igmp_group, hmap_node, igmp_groups) {
        if (!igmp_group->datapath) {
            continue;
        }

        struct mcast_switch_info *mcast_sw_info =
            &igmp_group->datapath->mcast_info.sw;

        if (mcast_sw_info->active_flows >= mcast_sw_info->table_size) {
            continue;
        }
        mcast_sw_info->active_flows++;

        ds_clear(&match);
        ds_clear(&actions);

        ds_put_format(&match, "eth.mcast && ip4 && ip4.dst == %s ",
                      igmp_group->mcgroup.name);

        /* Also flood traffic to all multicast routers with relay enabled. */
        if (mcast_sw_info->flood_relay) {
            ds_put_cstr(&actions,
                        "clone { "
                            "outport = \""MC_MROUTER_FLOOD "\"; "
                            "output; "
                        "};");
        }
        if (mcast_sw_info->flood_static) {
            ds_put_cstr(&actions,
                        "clone { "
                            "outport =\""MC_STATIC"\"; "
                            "output; "
                        "};");
        }
        ds_put_format(&actions, "outport = \"%s\"; output; ",
                      igmp_group->mcgroup.name);

        ovn_lflow_add(lflows, igmp_group->datapath, S_SWITCH_IN_L2_LKUP, 90,
                      ds_cstr(&match), ds_cstr(&actions));
    }

    /* Ingress table 17: Destination lookup, unicast handling (priority 50), */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbsp || lsp_is_external(op->nbsp)) {
            continue;
        }

        for (size_t i = 0; i < op->nbsp->n_addresses; i++) {
            /* Addresses are owned by the logical port.
             * Ethernet address followed by zero or more IPv4
             * or IPv6 addresses (or both). */
            struct eth_addr mac;
            if (ovs_scan(op->nbsp->addresses[i],
                        ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(mac))) {
                ds_clear(&match);
                ds_put_format(&match, "eth.dst == "ETH_ADDR_FMT,
                              ETH_ADDR_ARGS(mac));

                ds_clear(&actions);
                ds_put_format(&actions, "outport = %s; output;", op->json_key);
                ovn_lflow_add(lflows, op->od, S_SWITCH_IN_L2_LKUP, 50,
                              ds_cstr(&match), ds_cstr(&actions));
            } else if (!strcmp(op->nbsp->addresses[i], "unknown")) {
                if (lsp_is_enabled(op->nbsp)) {
                    ovn_multicast_add(mcgroups, &mc_unknown, op);
                    op->od->has_unknown = true;
                }
            } else if (is_dynamic_lsp_address(op->nbsp->addresses[i])) {
                if (!op->nbsp->dynamic_addresses
                    || !ovs_scan(op->nbsp->dynamic_addresses,
                            ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(mac))) {
                    continue;
                }
                ds_clear(&match);
                ds_put_format(&match, "eth.dst == "ETH_ADDR_FMT,
                              ETH_ADDR_ARGS(mac));

                ds_clear(&actions);
                ds_put_format(&actions, "outport = %s; output;", op->json_key);
                ovn_lflow_add(lflows, op->od, S_SWITCH_IN_L2_LKUP, 50,
                              ds_cstr(&match), ds_cstr(&actions));
            } else if (!strcmp(op->nbsp->addresses[i], "router")) {
                if (!op->peer || !op->peer->nbrp
                    || !ovs_scan(op->peer->nbrp->mac,
                            ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(mac))) {
                    continue;
                }
                ds_clear(&match);
                ds_put_format(&match, "eth.dst == "ETH_ADDR_FMT,
                              ETH_ADDR_ARGS(mac));
                if (op->peer->od->l3dgw_port
                    && op->peer->od->l3redirect_port
                    && op->od->localnet_port) {
                    bool add_chassis_resident_check = false;
                    if (op->peer == op->peer->od->l3dgw_port) {
                        /* The peer of this port represents a distributed
                         * gateway port. The destination lookup flow for the
                         * router's distributed gateway port MAC address should
                         * only be programmed on the "redirect-chassis". */
                        add_chassis_resident_check = true;
                    } else {
                        /* Check if the option 'reside-on-redirect-chassis'
                         * is set to true on the peer port. If set to true
                         * and if the logical switch has a localnet port, it
                         * means the router pipeline for the packets from
                         * this logical switch should be run on the chassis
                         * hosting the gateway port.
                         */
                        add_chassis_resident_check = smap_get_bool(
                            &op->peer->nbrp->options,
                            "reside-on-redirect-chassis", false);
                    }

                    if (add_chassis_resident_check) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->peer->od->l3redirect_port->json_key);
                    }
                }

                ds_clear(&actions);
                ds_put_format(&actions, "outport = %s; output;", op->json_key);
                ovn_lflow_add(lflows, op->od, S_SWITCH_IN_L2_LKUP, 50,
                              ds_cstr(&match), ds_cstr(&actions));

                /* Add ethernet addresses specified in NAT rules on
                 * distributed logical routers. */
                if (op->peer->od->l3dgw_port
                    && op->peer == op->peer->od->l3dgw_port) {
                    for (int j = 0; j < op->peer->od->nbr->n_nat; j++) {
                        const struct nbrec_nat *nat
                                                  = op->peer->od->nbr->nat[j];
                        if (!strcmp(nat->type, "dnat_and_snat")
                            && nat->logical_port && nat->external_mac
                            && eth_addr_from_string(nat->external_mac, &mac)) {

                            ds_clear(&match);
                            ds_put_format(&match, "eth.dst == "ETH_ADDR_FMT
                                          " && is_chassis_resident(\"%s\")",
                                          ETH_ADDR_ARGS(mac),
                                          nat->logical_port);

                            ds_clear(&actions);
                            ds_put_format(&actions, "outport = %s; output;",
                                          op->json_key);
                            ovn_lflow_add(lflows, op->od, S_SWITCH_IN_L2_LKUP,
                                          50, ds_cstr(&match),
                                          ds_cstr(&actions));
                        }
                    }
                }
            } else {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

                VLOG_INFO_RL(&rl,
                             "%s: invalid syntax '%s' in addresses column",
                             op->nbsp->name, op->nbsp->addresses[i]);
            }
        }
    }

    /* Ingress table 17: Destination lookup for unknown MACs (priority 0). */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        if (od->has_unknown) {
            ovn_lflow_add(lflows, od, S_SWITCH_IN_L2_LKUP, 0, "1",
                          "outport = \""MC_UNKNOWN"\"; output;");
        }
    }

    build_lswitch_output_port_sec(ports, datapaths, lflows);

    ds_destroy(&match);
    ds_destroy(&actions);
}

static bool
lrport_is_enabled(const struct nbrec_logical_router_port *lrport)
{
    return !lrport->enabled || *lrport->enabled;
}

/* Returns a string of the IP address of the router port 'op' that
 * overlaps with 'ip_s".  If one is not found, returns NULL.
 *
 * The caller must not free the returned string. */
static const char *
find_lrp_member_ip(const struct ovn_port *op, const char *ip_s)
{
    bool is_ipv4 = strchr(ip_s, '.') ? true : false;

    if (is_ipv4) {
        ovs_be32 ip;

        if (!ip_parse(ip_s, &ip)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad ip address %s", ip_s);
            return NULL;
        }

        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            const struct ipv4_netaddr *na = &op->lrp_networks.ipv4_addrs[i];

            if (!((na->network ^ ip) & na->mask)) {
                /* There should be only 1 interface that matches the
                 * supplied IP.  Otherwise, it's a configuration error,
                 * because subnets of a router's interfaces should NOT
                 * overlap. */
                return na->addr_s;
            }
        }
    } else {
        struct in6_addr ip6;

        if (!ipv6_parse(ip_s, &ip6)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad ipv6 address %s", ip_s);
            return NULL;
        }

        for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            const struct ipv6_netaddr *na = &op->lrp_networks.ipv6_addrs[i];
            struct in6_addr xor_addr = ipv6_addr_bitxor(&na->network, &ip6);
            struct in6_addr and_addr = ipv6_addr_bitand(&xor_addr, &na->mask);

            if (ipv6_is_zero(&and_addr)) {
                /* There should be only 1 interface that matches the
                 * supplied IP.  Otherwise, it's a configuration error,
                 * because subnets of a router's interfaces should NOT
                 * overlap. */
                return na->addr_s;
            }
        }
    }

    return NULL;
}

static struct ovn_port*
get_outport_for_routing_policy_nexthop(struct ovn_datapath *od,
                                       struct hmap *ports,
                                       int priority, const char *nexthop)
{
    if (nexthop == NULL) {
        return NULL;
    }

    /* Find the router port matching the next hop. */
    for (int i = 0; i < od->nbr->n_ports; i++) {
       struct nbrec_logical_router_port *lrp = od->nbr->ports[i];

       struct ovn_port *out_port = ovn_port_find(ports, lrp->name);
       if (out_port && find_lrp_member_ip(out_port, nexthop)) {
           return out_port;
       }
    }

    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
    VLOG_WARN_RL(&rl, "No path for routing policy priority %d; next hop %s",
                 priority, nexthop);
    return NULL;
}

static void
build_routing_policy_flow(struct hmap *lflows, struct ovn_datapath *od,
                          struct hmap *ports,
                          const struct nbrec_logical_router_policy *rule)
{
    struct ds match = DS_EMPTY_INITIALIZER;
    struct ds actions = DS_EMPTY_INITIALIZER;

    if (!strcmp(rule->action, "reroute")) {
        struct ovn_port *out_port = get_outport_for_routing_policy_nexthop(
             od, ports, rule->priority, rule->nexthop);
        if (!out_port) {
            return;
        }

        const char *lrp_addr_s = find_lrp_member_ip(out_port, rule->nexthop);
        if (!lrp_addr_s) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "lrp_addr not found for routing policy "
                         " priority %"PRId64" nexthop %s",
                         rule->priority, rule->nexthop);
            return;
        }
        bool is_ipv4 = strchr(rule->nexthop, '.') ? true : false;
        ds_put_format(&actions, "%sreg0 = %s; "
                      "%sreg1 = %s; "
                      "eth.src = %s; "
                      "outport = %s; "
                      "flags.loopback = 1; "
                      "next;",
                      is_ipv4 ? "" : "xx",
                      rule->nexthop,
                      is_ipv4 ? "" : "xx",
                      lrp_addr_s,
                      out_port->lrp_networks.ea_s,
                      out_port->json_key);

    } else if (!strcmp(rule->action, "drop")) {
        ds_put_cstr(&actions, "drop;");
    } else if (!strcmp(rule->action, "allow")) {
        ds_put_cstr(&actions, "next;");
    }
    ds_put_format(&match, "%s", rule->match);
    ovn_lflow_add(lflows, od, S_ROUTER_IN_POLICY, rule->priority,
                  ds_cstr(&match), ds_cstr(&actions));
    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
add_distributed_nat_routes(struct hmap *lflows, const struct ovn_port *op)
{
    struct ds actions = DS_EMPTY_INITIALIZER;
    struct ds match = DS_EMPTY_INITIALIZER;

    if (!op->od->l3dgw_port) {
        return;
    }

    if (!op->peer || !op->peer->od->nbs) {
        return;
    }

    for (size_t i = 0; i < op->od->nbr->n_nat; i++) {
        const struct nbrec_nat *nat = op->od->nbr->nat[i];
        bool found = false;

        if (strcmp(nat->type, "dnat_and_snat") ||
            !nat->external_mac  || !nat->external_ip) {
            continue;
        }

        const struct ovn_datapath *peer_dp = op->peer->od;
        for (size_t j = 0; j < peer_dp->nbs->n_ports; j++) {
            if (!strcmp(peer_dp->nbs->ports[j]->name, nat->logical_port)) {
                found = true;
                break;
            }
        }
        if (!found) {
            continue;
        }

        /* Determine if we need to create IPv4 or IPv6 flows */
        ovs_be32 ip;
        struct in6_addr ipv6;
        int family = AF_INET;
        if (!ip_parse(nat->external_ip, &ip) || !ip) {
            family = AF_INET6;
            if (!ipv6_parse(nat->external_ip, &ipv6)) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_WARN_RL(&rl, "bad ip address %s in nat configuration "
                             "for router %s", nat->external_ip, op->key);
                /* We'll create IPv6 flows anyway, but the address
                 * is probably bogus ... */
            }
        }

        ds_put_format(&match, "inport == %s && "
                      "ip%s.src == %s && ip%s.dst == %s",
                       op->json_key,
                       family == AF_INET ? "4" : "6",
                       nat->logical_ip,
                       family == AF_INET ? "4" : "6",
                       nat->external_ip);
        ds_put_format(&actions, "outport = %s; eth.dst = %s; "
                      REGBIT_DISTRIBUTED_NAT" = 1; "
                      REGBIT_NAT_REDIRECT" = 0; next;",
                      op->od->l3dgw_port->json_key,
                      nat->external_mac);
        ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_ROUTING, 400,
                      ds_cstr(&match), ds_cstr(&actions));
        ds_clear(&match);
        ds_clear(&actions);

        for (size_t j = 0; j < op->od->nbr->n_nat; j++) {
            const struct nbrec_nat *nat2 = op->od->nbr->nat[j];

            if (nat == nat2 || strcmp(nat2->type, "dnat_and_snat") ||
                !nat2->external_mac || !nat2->external_ip)
                continue;

            family = AF_INET;
            if (!ip_parse(nat2->external_ip, &ip) || !ip) {
                family = AF_INET6;
                if (!ipv6_parse(nat2->external_ip, &ipv6)) {
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad ip address %s in nat configuration "
                                 "for router %s", nat2->external_ip, op->key);
                    /* We'll create IPv6 flows anyway, but the address
                     * is probably bogus ... */
                }
            }

            ds_put_format(&match, "inport == %s && "
                          "ip%s.src == %s && ip%s.dst == %s",
                          op->json_key,
                          family == AF_INET ? "4" : "6",
                          nat->logical_ip,
                          family == AF_INET ? "4" : "6",
                          nat2->external_ip);
            ds_put_format(&actions, "outport = %s; "
                          "eth.src = %s; eth.dst = %s; "
                          "%sreg0 = ip%s.dst; %sreg1 = %s; "
                          REGBIT_DISTRIBUTED_NAT" = 1; "
                          REGBIT_NAT_REDIRECT" = 0; next;",
                          op->od->l3dgw_port->json_key,
                          op->od->l3dgw_port->lrp_networks.ea_s,
                          nat2->external_mac,
                          family == AF_INET ? "" : "xx",
                          family == AF_INET ? "4" : "6",
                          family == AF_INET ? "" : "xx",
                          nat->external_ip);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_ROUTING, 400,
                          ds_cstr(&match), ds_cstr(&actions));
            ds_clear(&match);
            ds_clear(&actions);
        }
    }
    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
add_route(struct hmap *lflows, const struct ovn_port *op,
          const char *lrp_addr_s, const char *network_s, int plen,
          const char *gateway, const char *policy)
{
    bool is_ipv4 = strchr(network_s, '.') ? true : false;
    struct ds match = DS_EMPTY_INITIALIZER;
    const char *dir;
    uint16_t priority;

    if (policy && !strcmp(policy, "src-ip")) {
        dir = "src";
        priority = plen * 2;
    } else {
        dir = "dst";
        priority = (plen * 2) + 1;
    }

    /* IPv6 link-local addresses must be scoped to the local router port. */
    if (!is_ipv4) {
        struct in6_addr network;
        ovs_assert(ipv6_parse(network_s, &network));
        if (in6_is_lla(&network)) {
            ds_put_format(&match, "inport == %s && ", op->json_key);
        }
    }
    ds_put_format(&match, "ip%s.%s == %s/%d", is_ipv4 ? "4" : "6", dir,
                  network_s, plen);

    struct ds actions = DS_EMPTY_INITIALIZER;
    ds_put_format(&actions, "ip.ttl--; %sreg0 = ", is_ipv4 ? "" : "xx");

    if (gateway) {
        ds_put_cstr(&actions, gateway);
    } else {
        ds_put_format(&actions, "ip%s.dst", is_ipv4 ? "4" : "6");
    }
    ds_put_format(&actions, "; "
                  "%sreg1 = %s; "
                  "eth.src = %s; "
                  "outport = %s; "
                  "flags.loopback = 1; "
                  "next;",
                  is_ipv4 ? "" : "xx",
                  lrp_addr_s,
                  op->lrp_networks.ea_s,
                  op->json_key);

    /* The priority here is calculated to implement longest-prefix-match
     * routing. */
    ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_ROUTING, priority,
                  ds_cstr(&match), ds_cstr(&actions));
    ds_destroy(&match);
    ds_destroy(&actions);
}

static void
build_static_route_flow(struct hmap *lflows, struct ovn_datapath *od,
                        struct hmap *ports,
                        const struct nbrec_logical_router_static_route *route)
{
    ovs_be32 nexthop;
    const char *lrp_addr_s = NULL;
    unsigned int plen;
    bool is_ipv4;

    /* Verify that the next hop is an IP address with an all-ones mask. */
    char *error = ip_parse_cidr(route->nexthop, &nexthop, &plen);
    if (!error) {
        if (plen != 32) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad next hop mask %s", route->nexthop);
            return;
        }
        is_ipv4 = true;
    } else {
        free(error);

        struct in6_addr ip6;
        error = ipv6_parse_cidr(route->nexthop, &ip6, &plen);
        if (!error) {
            if (plen != 128) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_WARN_RL(&rl, "bad next hop mask %s", route->nexthop);
                return;
            }
            is_ipv4 = false;
        } else {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad next hop ip address %s", route->nexthop);
            free(error);
            return;
        }
    }

    char *prefix_s;
    if (is_ipv4) {
        ovs_be32 prefix;
        /* Verify that ip prefix is a valid IPv4 address. */
        error = ip_parse_cidr(route->ip_prefix, &prefix, &plen);
        if (error) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad 'ip_prefix' in static routes %s",
                         route->ip_prefix);
            free(error);
            return;
        }
        prefix_s = xasprintf(IP_FMT, IP_ARGS(prefix & be32_prefix_mask(plen)));
    } else {
        /* Verify that ip prefix is a valid IPv6 address. */
        struct in6_addr prefix;
        error = ipv6_parse_cidr(route->ip_prefix, &prefix, &plen);
        if (error) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "bad 'ip_prefix' in static routes %s",
                         route->ip_prefix);
            free(error);
            return;
        }
        struct in6_addr mask = ipv6_create_mask(plen);
        struct in6_addr network = ipv6_addr_bitand(&prefix, &mask);
        prefix_s = xmalloc(INET6_ADDRSTRLEN);
        inet_ntop(AF_INET6, &network, prefix_s, INET6_ADDRSTRLEN);
    }

    /* Find the outgoing port. */
    struct ovn_port *out_port = NULL;
    if (route->output_port) {
        out_port = ovn_port_find(ports, route->output_port);
        if (!out_port) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, "Bad out port %s for static route %s",
                         route->output_port, route->ip_prefix);
            goto free_prefix_s;
        }
        lrp_addr_s = find_lrp_member_ip(out_port, route->nexthop);
        if (!lrp_addr_s) {
            /* There are no IP networks configured on the router's port via
             * which 'route->nexthop' is theoretically reachable.  But since
             * 'out_port' has been specified, we honor it by trying to reach
             * 'route->nexthop' via the first IP address of 'out_port'.
             * (There are cases, e.g in GCE, where each VM gets a /32 IP
             * address and the default gateway is still reachable from it.) */
            if (is_ipv4) {
                if (out_port->lrp_networks.n_ipv4_addrs) {
                    lrp_addr_s = out_port->lrp_networks.ipv4_addrs[0].addr_s;
                }
            } else {
                if (out_port->lrp_networks.n_ipv6_addrs) {
                    lrp_addr_s = out_port->lrp_networks.ipv6_addrs[0].addr_s;
                }
            }
        }
    } else {
        /* output_port is not specified, find the
         * router port matching the next hop. */
        int i;
        for (i = 0; i < od->nbr->n_ports; i++) {
            struct nbrec_logical_router_port *lrp = od->nbr->ports[i];
            out_port = ovn_port_find(ports, lrp->name);
            if (!out_port) {
                /* This should not happen. */
                continue;
            }

            lrp_addr_s = find_lrp_member_ip(out_port, route->nexthop);
            if (lrp_addr_s) {
                break;
            }
        }
    }

    if (!out_port || !lrp_addr_s) {
        /* There is no matched out port. */
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "No path for static route %s; next hop %s",
                     route->ip_prefix, route->nexthop);
        goto free_prefix_s;
    }

    char *policy = route->policy ? route->policy : "dst-ip";
    add_route(lflows, out_port, lrp_addr_s, prefix_s, plen, route->nexthop,
              policy);

free_prefix_s:
    free(prefix_s);
}

static void
op_put_v4_networks(struct ds *ds, const struct ovn_port *op, bool add_bcast)
{
    if (!add_bcast && op->lrp_networks.n_ipv4_addrs == 1) {
        ds_put_format(ds, "%s", op->lrp_networks.ipv4_addrs[0].addr_s);
        return;
    }

    ds_put_cstr(ds, "{");
    for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
        ds_put_format(ds, "%s, ", op->lrp_networks.ipv4_addrs[i].addr_s);
        if (add_bcast) {
            ds_put_format(ds, "%s, ", op->lrp_networks.ipv4_addrs[i].bcast_s);
        }
    }
    ds_chomp(ds, ' ');
    ds_chomp(ds, ',');
    ds_put_cstr(ds, "}");
}

static void
op_put_v6_networks(struct ds *ds, const struct ovn_port *op)
{
    if (op->lrp_networks.n_ipv6_addrs == 1) {
        ds_put_format(ds, "%s", op->lrp_networks.ipv6_addrs[0].addr_s);
        return;
    }

    ds_put_cstr(ds, "{");
    for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
        ds_put_format(ds, "%s, ", op->lrp_networks.ipv6_addrs[i].addr_s);
    }
    ds_chomp(ds, ' ');
    ds_chomp(ds, ',');
    ds_put_cstr(ds, "}");
}

static const char *
get_force_snat_ip(struct ovn_datapath *od, const char *key_type,
                  struct v46_ip *ip)
{
    char *key = xasprintf("%s_force_snat_ip", key_type);
    const char *ip_address = smap_get(&od->nbr->options, key);
    free(key);

    if (ip_address) {
        ovs_be32 mask;
        ip->family = AF_INET;
        char *error = ip_parse_masked(ip_address, &ip->ipv4, &mask);
        if (error || mask != OVS_BE32_MAX) {
            free(error);
            struct in6_addr mask_v6, v6_exact = IN6ADDR_EXACT_INIT;
            ip->family = AF_INET6;
            error = ipv6_parse_masked(ip_address, &ip->ipv6, &mask_v6);
            if (error || memcmp(&mask_v6, &v6_exact, sizeof(mask_v6))) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_WARN_RL(&rl, "bad ip %s in options of router "UUID_FMT"",
                             ip_address, UUID_ARGS(&od->key));
                memset(ip, 0, sizeof *ip);
                ip->family = AF_UNSPEC;
                return NULL;
            }
        }
        return ip_address;
    }

    memset(ip, 0, sizeof *ip);
    ip->family = AF_UNSPEC;
    return NULL;
}

static void
add_router_lb_flow(struct hmap *lflows, struct ovn_datapath *od,
                   struct ds *match, struct ds *actions, int priority,
                   const char *lb_force_snat_ip, struct smap_node *lb_info,
                   bool is_udp, int addr_family, char *ip_addr,
                   uint16_t l4_port, struct nbrec_load_balancer *lb,
                   struct shash *meter_groups)
{
    char *backend_ips = lb_info->value;

    build_empty_lb_event_flow(od, lflows, lb_info, ip_addr, lb,
                              l4_port, addr_family, S_ROUTER_IN_DNAT,
                              meter_groups);

    /* A match and actions for new connections. */
    char *new_match = xasprintf("ct.new && %s", ds_cstr(match));
    if (lb_force_snat_ip) {
        char *new_actions = xasprintf("flags.force_snat_for_lb = 1; %s",
                                      ds_cstr(actions));
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, priority, new_match,
                      new_actions);
        free(new_actions);
    } else {
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, priority, new_match,
                      ds_cstr(actions));
    }

    /* A match and actions for established connections. */
    char *est_match = xasprintf("ct.est && %s", ds_cstr(match));
    if (lb_force_snat_ip) {
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, priority, est_match,
                      "flags.force_snat_for_lb = 1; ct_dnat;");
    } else {
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, priority, est_match,
                      "ct_dnat;");
    }

    free(new_match);
    free(est_match);

    if (!od->l3dgw_port || !od->l3redirect_port || !backend_ips) {
        return;
    }

    /* Add logical flows to UNDNAT the load balanced reverse traffic in
     * the router egress pipleine stage - S_ROUTER_OUT_UNDNAT if the logical
     * router has a gateway router port associated.
     */
    struct ds undnat_match = DS_EMPTY_INITIALIZER;
    if (addr_family == AF_INET) {
        ds_put_cstr(&undnat_match, "ip4 && (");
    } else {
        ds_put_cstr(&undnat_match, "ip6 && (");
    }
    char *start, *next, *ip_str;
    start = next = xstrdup(backend_ips);
    ip_str = strsep(&next, ",");
    bool backend_ips_found = false;
    while (ip_str && ip_str[0]) {
        char *ip_address = NULL;
        uint16_t port = 0;
        int addr_family_;
        ip_address_and_port_from_lb_key(ip_str, &ip_address, &port,
                                        &addr_family_);
        if (!ip_address) {
            break;
        }

        if (addr_family_ == AF_INET) {
            ds_put_format(&undnat_match, "(ip4.src == %s", ip_address);
        } else {
            ds_put_format(&undnat_match, "(ip6.src == %s", ip_address);
        }
        free(ip_address);
        if (port) {
            ds_put_format(&undnat_match, " && %s.src == %d) || ",
                          is_udp ? "udp" : "tcp", port);
        } else {
            ds_put_cstr(&undnat_match, ") || ");
        }
        ip_str = strsep(&next, ",");
        backend_ips_found = true;
    }

    free(start);
    if (!backend_ips_found) {
        ds_destroy(&undnat_match);
        return;
    }
    ds_chomp(&undnat_match, ' ');
    ds_chomp(&undnat_match, '|');
    ds_chomp(&undnat_match, '|');
    ds_chomp(&undnat_match, ' ');
    ds_put_format(&undnat_match, ") && outport == %s && "
                 "is_chassis_resident(%s)", od->l3dgw_port->json_key,
                 od->l3redirect_port->json_key);
    if (lb_force_snat_ip) {
        ovn_lflow_add(lflows, od, S_ROUTER_OUT_UNDNAT, 120,
                      ds_cstr(&undnat_match),
                      "flags.force_snat_for_lb = 1; ct_dnat;");
    } else {
        ovn_lflow_add(lflows, od, S_ROUTER_OUT_UNDNAT, 120,
                      ds_cstr(&undnat_match), "ct_dnat;");
    }

    ds_destroy(&undnat_match);
}

#define ND_RA_MAX_INTERVAL_MAX 1800
#define ND_RA_MAX_INTERVAL_MIN 4

#define ND_RA_MIN_INTERVAL_MAX(max) ((max) * 3 / 4)
#define ND_RA_MIN_INTERVAL_MIN 3

static void
copy_ra_to_sb(struct ovn_port *op, const char *address_mode)
{
    struct smap options;
    smap_clone(&options, &op->sb->options);

    smap_add(&options, "ipv6_ra_send_periodic", "true");
    smap_add(&options, "ipv6_ra_address_mode", address_mode);

    int max_interval = smap_get_int(&op->nbrp->ipv6_ra_configs,
            "max_interval", ND_RA_MAX_INTERVAL_DEFAULT);
    if (max_interval > ND_RA_MAX_INTERVAL_MAX) {
        max_interval = ND_RA_MAX_INTERVAL_MAX;
    }
    if (max_interval < ND_RA_MAX_INTERVAL_MIN) {
        max_interval = ND_RA_MAX_INTERVAL_MIN;
    }
    smap_add_format(&options, "ipv6_ra_max_interval", "%d", max_interval);

    int min_interval = smap_get_int(&op->nbrp->ipv6_ra_configs,
            "min_interval", nd_ra_min_interval_default(max_interval));
    if (min_interval > ND_RA_MIN_INTERVAL_MAX(max_interval)) {
        min_interval = ND_RA_MIN_INTERVAL_MAX(max_interval);
    }
    if (min_interval < ND_RA_MIN_INTERVAL_MIN) {
        min_interval = ND_RA_MIN_INTERVAL_MIN;
    }
    smap_add_format(&options, "ipv6_ra_min_interval", "%d", min_interval);

    int mtu = smap_get_int(&op->nbrp->ipv6_ra_configs, "mtu", ND_MTU_DEFAULT);
    /* RFC 2460 requires the MTU for IPv6 to be at least 1280 */
    if (mtu && mtu >= 1280) {
        smap_add_format(&options, "ipv6_ra_mtu", "%d", mtu);
    }

    struct ds s = DS_EMPTY_INITIALIZER;
    for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; ++i) {
        struct ipv6_netaddr *addrs = &op->lrp_networks.ipv6_addrs[i];
        if (in6_is_lla(&addrs->network)) {
            smap_add(&options, "ipv6_ra_src_addr", addrs->addr_s);
            continue;
        }
        ds_put_format(&s, "%s/%u ", addrs->network_s, addrs->plen);
    }
    /* Remove trailing space */
    ds_chomp(&s, ' ');
    smap_add(&options, "ipv6_ra_prefixes", ds_cstr(&s));
    ds_destroy(&s);

    const char *rdnss = smap_get(&op->nbrp->ipv6_ra_configs, "rdnss");
    if (rdnss) {
        smap_add(&options, "ipv6_ra_rdnss", rdnss);
    }
    const char *dnssl = smap_get(&op->nbrp->ipv6_ra_configs, "dnssl");
    if (dnssl) {
        smap_add(&options, "ipv6_ra_dnssl", dnssl);
    }

    smap_add(&options, "ipv6_ra_src_eth", op->lrp_networks.ea_s);

    sbrec_port_binding_set_options(op->sb, &options);
    smap_destroy(&options);
}

static inline bool
lrouter_nat_is_stateless(const struct nbrec_nat *nat)
{
    const char *stateless = smap_get(&nat->options, "stateless");

    if (stateless && !strcmp(stateless, "true")) {
        return true;
    }

    return false;
}

static void
build_lrouter_flows(struct hmap *datapaths, struct hmap *ports,
                    struct hmap *lflows, struct shash *meter_groups)
{
    /* This flow table structure is documented in ovn-northd(8), so please
     * update ovn-northd.8.xml if you change anything. */

    struct ds match = DS_EMPTY_INITIALIZER;
    struct ds actions = DS_EMPTY_INITIALIZER;

    /* Logical router ingress table 0: Admission control framework. */
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        /* Logical VLANs not supported.
         * Broadcast/multicast source address is invalid. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_ADMISSION, 100,
                      "vlan.present || eth.src[40]", "drop;");
    }

    /* Logical router ingress table 0: match (priority 50). */
    struct ovn_port *op;
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        if (!lrport_is_enabled(op->nbrp)) {
            /* Drop packets from disabled logical ports (since logical flow
             * tables are default-drop). */
            continue;
        }

        if (op->derived) {
            /* No ingress packets should be received on a chassisredirect
             * port. */
            continue;
        }

        ds_clear(&match);
        ds_put_format(&match, "eth.mcast && inport == %s", op->json_key);
        ovn_lflow_add(lflows, op->od, S_ROUTER_IN_ADMISSION, 50,
                      ds_cstr(&match), "next;");

        ds_clear(&match);
        ds_put_format(&match, "eth.dst == %s && inport == %s",
                      op->lrp_networks.ea_s, op->json_key);
        if (op->od->l3dgw_port && op == op->od->l3dgw_port
            && op->od->l3redirect_port) {
            /* Traffic with eth.dst = l3dgw_port->lrp_networks.ea_s
             * should only be received on the "redirect-chassis". */
            ds_put_format(&match, " && is_chassis_resident(%s)",
                          op->od->l3redirect_port->json_key);
        }
        ovn_lflow_add(lflows, op->od, S_ROUTER_IN_ADMISSION, 50,
                      ds_cstr(&match), "next;");
    }

    /* Logical router ingress table 1: LOOKUP_NEIGHBOR and
     * table 2: LEARN_NEIGHBOR. */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        /* Learn MAC bindings from ARP/IPv6 ND.
         *
         * For ARP packets, table LOOKUP_NEIGHBOR does a lookup for the
         * (arp.spa, arp.sha) in the mac binding table using the 'lookup_arp'
         * action and stores the result in REGBIT_LOOKUP_NEIGHBOR_RESULT bit.
         *
         * For IPv6 ND NA packets, table LOOKUP_NEIGHBOR does a lookup
         * for the (nd.target, nd.tll) in the mac binding table using the
         * 'lookup_nd' action and stores the result in
         * REGBIT_LOOKUP_NEIGHBOR_RESULT bit.
         *
         * For IPv6 ND NS packets, table LOOKUP_NEIGHBOR does a lookup
         * for the (ip6.src, nd.sll) in the mac binding table using the
         * 'lookup_nd' action and stores the result in
         * REGBIT_LOOKUP_NEIGHBOR_RESULT bit.
         *
         * Table LEARN_NEIGHBOR learns the mac-binding using the action
         * - 'put_arp/put_nd' only if REGBIT_LOOKUP_NEIGHBOR_RESULT bit
         * is not set.
         *
         * */

        /* Flows for LOOKUP_NEIGHBOR. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_LOOKUP_NEIGHBOR, 100,
                      "arp.op == 2",
                      REGBIT_LOOKUP_NEIGHBOR_RESULT" = "
                      "lookup_arp(inport, arp.spa, arp.sha); next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_LOOKUP_NEIGHBOR, 100, "nd_na",
                      REGBIT_LOOKUP_NEIGHBOR_RESULT" = "
                      "lookup_nd(inport, nd.target, nd.tll); next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_LOOKUP_NEIGHBOR, 100, "nd_ns",
                      REGBIT_LOOKUP_NEIGHBOR_RESULT" = "
                      "lookup_nd(inport, ip6.src, nd.sll); next;");

        /* For other packet types, we can skip neighbor learning.
         * So set REGBIT_SKIP_LOOKUP_NEIGHBOR to 1. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_LOOKUP_NEIGHBOR, 0, "1",
                      REGBIT_SKIP_LOOKUP_NEIGHBOR" = 1; next;");

        /* Flows for LEARN_NEIGHBOR. */
        /* Skip Neighbor learning if not required. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_LEARN_NEIGHBOR, 100,
                      REGBIT_SKIP_LOOKUP_NEIGHBOR" == 1 || "
                      REGBIT_LOOKUP_NEIGHBOR_RESULT" == 1", "next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_LEARN_NEIGHBOR, 90,
                      "arp", "put_arp(inport, arp.spa, arp.sha); next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_LEARN_NEIGHBOR, 90,
                      "nd_na", "put_nd(inport, nd.target, nd.tll); next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_LEARN_NEIGHBOR, 90,
                      "nd_ns", "put_nd(inport, ip6.src, nd.sll); next;");
    }

    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        /* Check if we need to learn mac-binding from ARP requests. */
        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            ds_clear(&match);
            ds_put_format(&match,
                          "inport == %s && arp.spa == %s/%u && arp.op == 1",
                          op->json_key,
                          op->lrp_networks.ipv4_addrs[i].network_s,
                          op->lrp_networks.ipv4_addrs[i].plen);
            if (op->od->l3dgw_port && op == op->od->l3dgw_port
                && op->od->l3redirect_port) {
                ds_put_format(&match, " && is_chassis_resident(%s)",
                              op->od->l3redirect_port->json_key);
            }
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_LOOKUP_NEIGHBOR, 100,
                          ds_cstr(&match),
                          REGBIT_LOOKUP_NEIGHBOR_RESULT" = "
                          "lookup_arp(inport, arp.spa, arp.sha); next;");
        }
    }

    /* Logical router ingress table 3: IP Input. */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        /* L3 admission control: drop multicast and broadcast source, localhost
         * source or destination, and zero network source or destination
         * (priority 100). */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 100,
                      "ip4.src_mcast ||"
                      "ip4.src == 255.255.255.255 || "
                      "ip4.src == 127.0.0.0/8 || "
                      "ip4.dst == 127.0.0.0/8 || "
                      "ip4.src == 0.0.0.0/8 || "
                      "ip4.dst == 0.0.0.0/8",
                      "drop;");

        /* Allow multicast if relay enabled (priority 95). */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 95, "ip4.mcast",
                      od->mcast_info.rtr.relay ? "next;" : "drop;");

        /* Drop ARP packets (priority 85). ARP request packets for router's own
         * IPs are handled with priority-90 flows.
         * Drop IPv6 ND packets (priority 85). ND NA packets for router's own
         * IPs are handled with priority-90 flows.
         */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 85,
                      "arp || nd", "drop;");

        /* Drop Ethernet local broadcast.  By definition this traffic should
         * not be forwarded.*/
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 50,
                      "eth.bcast", "drop;");

        /* TTL discard */
        ds_clear(&match);
        ds_put_cstr(&match, "ip4 && ip.ttl == {0, 1}");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 30,
                      ds_cstr(&match), "drop;");

        /* Pass other traffic not already handled to the next table for
         * routing. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_INPUT, 0, "1", "next;");
    }

    /* Logical router ingress table 3: IP Input for IPv4. */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        if (op->derived) {
            /* No ingress packets are accepted on a chassisredirect
             * port, so no need to program flows for that port. */
            continue;
        }

        if (op->lrp_networks.n_ipv4_addrs) {
            /* L3 admission control: drop packets that originate from an
             * IPv4 address owned by the router or a broadcast address
             * known to the router (priority 100). */
            ds_clear(&match);
            ds_put_cstr(&match, "ip4.src == ");
            op_put_v4_networks(&match, op, true);
            ds_put_cstr(&match, " && "REGBIT_EGRESS_LOOPBACK" == 0");
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 100,
                          ds_cstr(&match), "drop;");

            /* ICMP echo reply.  These flows reply to ICMP echo requests
             * received for the router's IP address. Since packets only
             * get here as part of the logical router datapath, the inport
             * (i.e. the incoming locally attached net) does not matter.
             * The ip.ttl also does not matter (RFC1812 section 4.2.2.9) */
            ds_clear(&match);
            ds_put_cstr(&match, "ip4.dst == ");
            op_put_v4_networks(&match, op, false);
            ds_put_cstr(&match, " && icmp4.type == 8 && icmp4.code == 0");

            ds_clear(&actions);
            ds_put_format(&actions,
                "ip4.dst <-> ip4.src; "
                "ip.ttl = 255; "
                "icmp4.type = 0; "
                "flags.loopback = 1; "
                "next; ");
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* ICMP time exceeded */
        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            ds_clear(&match);
            ds_clear(&actions);

            ds_put_format(&match,
                          "inport == %s && ip4 && "
                          "ip.ttl == {0, 1} && !ip.later_frag", op->json_key);
            ds_put_format(&actions,
                          "icmp4 {"
                          "eth.dst <-> eth.src; "
                          "icmp4.type = 11; /* Time exceeded */ "
                          "icmp4.code = 0; /* TTL exceeded in transit */ "
                          "ip4.dst = ip4.src; "
                          "ip4.src = %s; "
                          "ip.ttl = 255; "
                          "next; };",
                          op->lrp_networks.ipv4_addrs[i].addr_s);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 40,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* ARP reply.  These flows reply to ARP requests for the router's own
         * IP address. */
        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            ds_clear(&match);
            ds_put_format(&match,
                          "inport == %s && arp.spa == %s/%u && arp.tpa == %s"
                          " && arp.op == 1",
                          op->json_key,
                          op->lrp_networks.ipv4_addrs[i].network_s,
                          op->lrp_networks.ipv4_addrs[i].plen,
                          op->lrp_networks.ipv4_addrs[i].addr_s);

            if (op->od->l3dgw_port && op->od->l3redirect_port && op->peer
                && op->peer->od->localnet_port) {
                bool add_chassis_resident_check = false;
                if (op == op->od->l3dgw_port) {
                    /* Traffic with eth.src = l3dgw_port->lrp_networks.ea_s
                     * should only be sent from the "redirect-chassis", so that
                     * upstream MAC learning points to the "redirect-chassis".
                     * Also need to avoid generation of multiple ARP responses
                     * from different chassis. */
                    add_chassis_resident_check = true;
                } else {
                    /* Check if the option 'reside-on-redirect-chassis'
                     * is set to true on the router port. If set to true
                     * and if peer's logical switch has a localnet port, it
                     * means the router pipeline for the packets from
                     * peer's logical switch is be run on the chassis
                     * hosting the gateway port and it should reply to the
                     * ARP requests for the router port IPs.
                     */
                    add_chassis_resident_check = smap_get_bool(
                        &op->nbrp->options,
                        "reside-on-redirect-chassis", false);
                }

                if (add_chassis_resident_check) {
                    ds_put_format(&match, " && is_chassis_resident(%s)",
                                  op->od->l3redirect_port->json_key);
                }
            }

            ds_clear(&actions);
            ds_put_format(&actions,
                "eth.dst = eth.src; "
                "eth.src = %s; "
                "arp.op = 2; /* ARP reply */ "
                "arp.tha = arp.sha; "
                "arp.sha = %s; "
                "arp.tpa = arp.spa; "
                "arp.spa = %s; "
                "outport = %s; "
                "flags.loopback = 1; "
                "output;",
                op->lrp_networks.ea_s,
                op->lrp_networks.ea_s,
                op->lrp_networks.ipv4_addrs[i].addr_s,
                op->json_key);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* A set to hold all load-balancer vips that need ARP responses. */
        struct sset all_ips = SSET_INITIALIZER(&all_ips);
        int addr_family;
        get_router_load_balancer_ips(op->od, &all_ips, &addr_family);

        const char *ip_address;
        SSET_FOR_EACH(ip_address, &all_ips) {
            ds_clear(&match);
            if (addr_family == AF_INET) {
                ds_put_format(&match,
                              "inport == %s && arp.tpa == %s && arp.op == 1",
                              op->json_key, ip_address);
            } else {
                ds_put_format(&match,
                              "inport == %s && nd_ns && nd.target == %s",
                              op->json_key, ip_address);
            }

            ds_clear(&actions);
            if (addr_family == AF_INET) {
                ds_put_format(&actions,
                "eth.dst = eth.src; "
                "eth.src = %s; "
                "arp.op = 2; /* ARP reply */ "
                "arp.tha = arp.sha; "
                "arp.sha = %s; "
                "arp.tpa = arp.spa; "
                "arp.spa = %s; "
                "outport = %s; "
                "flags.loopback = 1; "
                "output;",
                op->lrp_networks.ea_s,
                op->lrp_networks.ea_s,
                ip_address,
                op->json_key);
            } else {
                ds_put_format(&actions,
                "nd_na { "
                "eth.src = %s; "
                "ip6.src = %s; "
                "nd.target = %s; "
                "nd.tll = %s; "
                "outport = inport; "
                "flags.loopback = 1; "
                "output; "
                "};",
                op->lrp_networks.ea_s,
                ip_address,
                ip_address,
                op->lrp_networks.ea_s);
            }
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        sset_destroy(&all_ips);

        /* A gateway router can have 2 SNAT IP addresses to force DNATed and
         * LBed traffic respectively to be SNATed.  In addition, there can be
         * a number of SNAT rules in the NAT table. */
        struct v46_ip *snat_ips = xmalloc(sizeof *snat_ips
                                          * (op->od->nbr->n_nat + 2));
        size_t n_snat_ips = 0;

        struct v46_ip snat_ip;
        const char *dnat_force_snat_ip = get_force_snat_ip(op->od, "dnat",
                                                           &snat_ip);
        if (dnat_force_snat_ip) {
            snat_ips[n_snat_ips++] = snat_ip;
        }

        const char *lb_force_snat_ip = get_force_snat_ip(op->od, "lb",
                                                         &snat_ip);
        if (lb_force_snat_ip) {
            snat_ips[n_snat_ips++] = snat_ip;
        }

        for (int i = 0; i < op->od->nbr->n_nat; i++) {
            const struct nbrec_nat *nat;

            nat = op->od->nbr->nat[i];

            ovs_be32 ip;
            struct in6_addr ipv6;
            bool is_v6 = false;
            if (!ip_parse(nat->external_ip, &ip) || !ip) {
                if (!ipv6_parse(nat->external_ip, &ipv6)) {
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad ip address %s in nat configuration "
                                 "for router %s", nat->external_ip, op->key);
                    continue;
                }
                is_v6 = true;
            }

            if (!strcmp(nat->type, "snat")) {
                if (is_v6) {
                    snat_ips[n_snat_ips].family = AF_INET6;
                    snat_ips[n_snat_ips++].ipv6 = ipv6;
                } else {
                    snat_ips[n_snat_ips].family = AF_INET;
                    snat_ips[n_snat_ips++].ipv4 = ip;
                }
                continue;
            }

            /* ARP / ND handling for external IP addresses.
             *
             * DNAT IP addresses are external IP addresses that need ARP
             * handling. */
            char addr_s[INET6_ADDRSTRLEN + 1];
            ds_clear(&match);
            ds_clear(&actions);
            if (is_v6) {
                /* For ND solicitations, we need to listen for both the
                 * unicast IPv6 address and its all-nodes multicast address,
                 * but always respond with the unicast IPv6 address. */
                char sn_addr_s[INET6_ADDRSTRLEN + 1];
                struct in6_addr sn_addr;
                in6_addr_solicited_node(&sn_addr, &ipv6);
                ipv6_string_mapped(sn_addr_s, &sn_addr);
                ipv6_string_mapped(addr_s, &ipv6);

                ds_put_format(&match, "inport == %s && "
                        "nd_ns && ip6.dst == {%s, %s} && nd.target == %s",
                        op->json_key, addr_s, sn_addr_s, addr_s);

                ds_put_format(&actions,
                    "eth.dst = eth.src; "
                    "nd_na { ");
            } else {
                ds_put_format(&match,
                              "inport == %s "
                              "&& arp.tpa == "IP_FMT" && arp.op == 1",
                              op->json_key, IP_ARGS(ip));

                ds_put_format(&actions,
                    "eth.dst = eth.src; "
                    "arp.op = 2; /* ARP reply */ "
                    "arp.tha = arp.sha; ");
            }
            if (op->od->l3dgw_port && op == op->od->l3dgw_port) {
                struct eth_addr mac;
                if (nat->external_mac &&
                    eth_addr_from_string(nat->external_mac, &mac)
                    && nat->logical_port) {
                    /* distributed NAT case, use nat->external_mac */
                    if (is_v6) {
                        ds_put_format(&actions,
                            "eth.src = "ETH_ADDR_FMT"; "
                            "nd.tll = "ETH_ADDR_FMT"; ",
                            ETH_ADDR_ARGS(mac),
                            ETH_ADDR_ARGS(mac));

                    } else {
                        ds_put_format(&actions,
                            "eth.src = "ETH_ADDR_FMT"; "
                            "arp.sha = "ETH_ADDR_FMT"; ",
                            ETH_ADDR_ARGS(mac),
                            ETH_ADDR_ARGS(mac));
                    }
                    /* Traffic with eth.src = nat->external_mac should only be
                     * sent from the chassis where nat->logical_port is
                     * resident, so that upstream MAC learning points to the
                     * correct chassis.  Also need to avoid generation of
                     * multiple ARP responses from different chassis. */
                    ds_put_format(&match, " && is_chassis_resident(\"%s\")",
                                  nat->logical_port);
                } else {
                    if (is_v6) {
                        ds_put_format(&actions,
                            "eth.src = %s; "
                            "nd.tll = %s; ",
                            op->lrp_networks.ea_s,
                            op->lrp_networks.ea_s);

                    } else {
                        ds_put_format(&actions,
                            "eth.src = %s; "
                            "arp.sha = %s; ",
                            op->lrp_networks.ea_s,
                            op->lrp_networks.ea_s);
                    }
                    /* Traffic with eth.src = l3dgw_port->lrp_networks.ea_s
                     * should only be sent from the "redirect-chassis", so that
                     * upstream MAC learning points to the "redirect-chassis".
                     * Also need to avoid generation of multiple ARP responses
                     * from different chassis. */
                    if (op->od->l3redirect_port) {
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      op->od->l3redirect_port->json_key);
                    }
                }
            } else {
                if (is_v6) {
                    ds_put_format(&actions,
                        "eth.src = %s; "
                        "nd.tll = %s; ",
                        op->lrp_networks.ea_s,
                        op->lrp_networks.ea_s);
                } else {
                    ds_put_format(&actions,
                        "eth.src = %s; "
                        "arp.sha = %s; ",
                        op->lrp_networks.ea_s,
                        op->lrp_networks.ea_s);
                }
            }
            if (is_v6) {
                ds_put_format(&actions,
                    "ip6.src = %s; "
                    "nd.target = %s; "
                    "outport = %s; "
                    "flags.loopback = 1; "
                    "output; "
                    "};",
                    addr_s, addr_s, op->json_key);
            } else {
                ds_put_format(&actions,
                    "arp.tpa = arp.spa; "
                    "arp.spa = "IP_FMT"; "
                    "outport = %s; "
                    "flags.loopback = 1; "
                    "output;",
                    IP_ARGS(ip),
                    op->json_key);
            }
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        if (!smap_get(&op->od->nbr->options, "chassis")
            && !op->od->l3dgw_port) {
            /* UDP/TCP port unreachable. */
            for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
                ds_clear(&match);
                ds_put_format(&match,
                              "ip4 && ip4.dst == %s && !ip.later_frag && udp",
                              op->lrp_networks.ipv4_addrs[i].addr_s);
                const char *action = "icmp4 {"
                                     "eth.dst <-> eth.src; "
                                     "ip4.dst <-> ip4.src; "
                                     "ip.ttl = 255; "
                                     "icmp4.type = 3; "
                                     "icmp4.code = 3; "
                                     "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 80,
                              ds_cstr(&match), action);

                ds_clear(&match);
                ds_put_format(&match,
                              "ip4 && ip4.dst == %s && !ip.later_frag && tcp",
                              op->lrp_networks.ipv4_addrs[i].addr_s);
                action = "tcp_reset {"
                         "eth.dst <-> eth.src; "
                         "ip4.dst <-> ip4.src; "
                         "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 80,
                              ds_cstr(&match), action);

                ds_clear(&match);
                ds_put_format(&match,
                              "ip4 && ip4.dst == %s && !ip.later_frag",
                              op->lrp_networks.ipv4_addrs[i].addr_s);
                action = "icmp4 {"
                         "eth.dst <-> eth.src; "
                         "ip4.dst <-> ip4.src; "
                         "ip.ttl = 255; "
                         "icmp4.type = 3; "
                         "icmp4.code = 2; "
                         "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 70,
                              ds_cstr(&match), action);
            }
        }

        ds_clear(&match);
        ds_put_cstr(&match, "ip4.dst == {");
        bool has_drop_ips = false;
        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            bool snat_ip_is_router_ip = false;
            for (int j = 0; j < n_snat_ips; j++) {
                /* Packets to SNAT IPs should not be dropped. */
                if (snat_ips[j].family == AF_INET
                    && op->lrp_networks.ipv4_addrs[i].addr
                       == snat_ips[j].ipv4) {
                        snat_ip_is_router_ip = true;
                        break;
                }
            }
            if (snat_ip_is_router_ip) {
                continue;
            }
            ds_put_format(&match, "%s, ",
                          op->lrp_networks.ipv4_addrs[i].addr_s);
            has_drop_ips = true;
        }
        if (has_drop_ips) {
            ds_chomp(&match, ' ');
            ds_chomp(&match, ',');
            ds_put_cstr(&match, "} || ip6.dst == {");
        } else {
            ds_clear(&match);
            ds_put_cstr(&match, "ip6.dst == {");
        }

        for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            bool snat_ip_is_router_ip = false;
            for (int j = 0; j < n_snat_ips; j++) {
                /* Packets to SNAT IPs should not be dropped. */
                if (snat_ips[j].family == AF_INET6
                    && !memcmp(&op->lrp_networks.ipv6_addrs[i].addr,
                               &snat_ips[j].ipv6, sizeof snat_ips[j].ipv6)) {
                    snat_ip_is_router_ip = true;
                    break;
                }
            }
            if (snat_ip_is_router_ip) {
                continue;
            }
            ds_put_format(&match, "%s, ",
                          op->lrp_networks.ipv6_addrs[i].addr_s);
            has_drop_ips = true;
        }

        ds_chomp(&match, ' ');
        ds_chomp(&match, ',');
        ds_put_cstr(&match, "}");

        if (has_drop_ips) {
            /* Drop IP traffic to this router. */
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 60,
                          ds_cstr(&match), "drop;");
        }

        free(snat_ips);
    }

    /* Logical router ingress table 3: IP Input for IPv6. */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        if (op->derived) {
            /* No ingress packets are accepted on a chassisredirect
             * port, so no need to program flows for that port. */
            continue;
        }

        if (op->lrp_networks.n_ipv6_addrs) {
            /* ICMPv6 echo reply.  These flows reply to echo requests
             * received for the router's IP address. */
            ds_clear(&match);
            ds_put_cstr(&match, "ip6.dst == ");
            op_put_v6_networks(&match, op);
            ds_put_cstr(&match, " && icmp6.type == 128 && icmp6.code == 0");

            ds_clear(&actions);
            ds_put_cstr(&actions,
                        "ip6.dst <-> ip6.src; "
                        "ip.ttl = 255; "
                        "icmp6.type = 129; "
                        "flags.loopback = 1; "
                        "next; ");
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* ND reply.  These flows reply to ND solicitations for the
         * router's own IP address. */
        for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            ds_clear(&match);
            ds_put_format(&match,
                    "inport == %s && nd_ns && ip6.dst == {%s, %s} "
                    "&& nd.target == %s",
                    op->json_key,
                    op->lrp_networks.ipv6_addrs[i].addr_s,
                    op->lrp_networks.ipv6_addrs[i].sn_addr_s,
                    op->lrp_networks.ipv6_addrs[i].addr_s);
            if (op->od->l3dgw_port && op == op->od->l3dgw_port
                && op->od->l3redirect_port) {
                /* Traffic with eth.src = l3dgw_port->lrp_networks.ea_s
                 * should only be sent from the "redirect-chassis", so that
                 * upstream MAC learning points to the "redirect-chassis".
                 * Also need to avoid generation of multiple ND replies
                 * from different chassis. */
                ds_put_format(&match, " && is_chassis_resident(%s)",
                              op->od->l3redirect_port->json_key);
            }

            ds_clear(&actions);
            ds_put_format(&actions,
                          "nd_na_router { "
                          "eth.src = %s; "
                          "ip6.src = %s; "
                          "nd.target = %s; "
                          "nd.tll = %s; "
                          "outport = inport; "
                          "flags.loopback = 1; "
                          "output; "
                          "};",
                          op->lrp_networks.ea_s,
                          op->lrp_networks.ipv6_addrs[i].addr_s,
                          op->lrp_networks.ipv6_addrs[i].addr_s,
                          op->lrp_networks.ea_s);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 90,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* UDP/TCP port unreachable */
        if (!smap_get(&op->od->nbr->options, "chassis")
            && !op->od->l3dgw_port) {
            for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
                ds_clear(&match);
                ds_put_format(&match,
                              "ip6 && ip6.dst == %s && !ip.later_frag && tcp",
                              op->lrp_networks.ipv6_addrs[i].addr_s);
                const char *action = "tcp_reset {"
                                     "eth.dst <-> eth.src; "
                                     "ip6.dst <-> ip6.src; "
                                     "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 80,
                          ds_cstr(&match), action);

                ds_clear(&match);
                ds_put_format(&match,
                              "ip6 && ip6.dst == %s && !ip.later_frag && udp",
                              op->lrp_networks.ipv6_addrs[i].addr_s);
                action = "icmp6 {"
                         "eth.dst <-> eth.src; "
                         "ip6.dst <-> ip6.src; "
                         "ip.ttl = 255; "
                         "icmp6.type = 1; "
                         "icmp6.code = 4; "
                         "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 80,
                              ds_cstr(&match), action);

                ds_clear(&match);
                ds_put_format(&match,
                              "ip6 && ip6.dst == %s && !ip.later_frag",
                              op->lrp_networks.ipv6_addrs[i].addr_s);
                action = "icmp6 {"
                         "eth.dst <-> eth.src; "
                         "ip6.dst <-> ip6.src; "
                         "ip.ttl = 255; "
                         "icmp6.type = 1; "
                         "icmp6.code = 3; "
                         "next; };";
                ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 70,
                              ds_cstr(&match), action);
            }
        }

        /* ICMPv6 time exceeded */
        for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            /* skip link-local address */
            if (in6_is_lla(&op->lrp_networks.ipv6_addrs[i].network)) {
                continue;
            }

            ds_clear(&match);
            ds_clear(&actions);

            ds_put_format(&match,
                          "inport == %s && ip6 && "
                          "ip6.src == %s/%d && "
                          "ip.ttl == {0, 1} && !ip.later_frag",
                          op->json_key,
                          op->lrp_networks.ipv6_addrs[i].network_s,
                          op->lrp_networks.ipv6_addrs[i].plen);
            ds_put_format(&actions,
                          "icmp6 {"
                          "eth.dst <-> eth.src; "
                          "ip6.dst = ip6.src; "
                          "ip6.src = %s; "
                          "ip.ttl = 255; "
                          "icmp6.type = 3; /* Time exceeded */ "
                          "icmp6.code = 0; /* TTL exceeded in transit */ "
                          "next; };",
                          op->lrp_networks.ipv6_addrs[i].addr_s);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_IP_INPUT, 40,
                          ds_cstr(&match), ds_cstr(&actions));
        }
    }

    /* NAT, Defrag and load balancing. */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        /* Packets are allowed by default. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DEFRAG, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_OUT_SNAT, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_OUT_UNDNAT, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_OUT_EGR_LOOP, 0, "1", "next;");

        /* NAT rules are only valid on Gateway routers and routers with
         * l3dgw_port (router has a port with "redirect-chassis"
         * specified). */
        if (!smap_get(&od->nbr->options, "chassis") && !od->l3dgw_port) {
            continue;
        }

        struct v46_ip snat_ip, lb_snat_ip;
        const char *dnat_force_snat_ip = get_force_snat_ip(od, "dnat",
                                                           &snat_ip);
        const char *lb_force_snat_ip = get_force_snat_ip(od, "lb",
                                                         &lb_snat_ip);

        for (int i = 0; i < od->nbr->n_nat; i++) {
            const struct nbrec_nat *nat;

            nat = od->nbr->nat[i];

            ovs_be32 ip, mask;
            struct in6_addr ipv6, mask_v6, v6_exact = IN6ADDR_EXACT_INIT;
            bool is_v6 = false;
            bool stateless = lrouter_nat_is_stateless(nat);

            char *error = ip_parse_masked(nat->external_ip, &ip, &mask);
            if (error || mask != OVS_BE32_MAX) {
                free(error);
                error = ipv6_parse_masked(nat->external_ip, &ipv6, &mask_v6);
                if (error || memcmp(&mask_v6, &v6_exact, sizeof(mask_v6))) {
                    /* Invalid for both IPv4 and IPv6 */
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad external ip %s for nat",
                                 nat->external_ip);
                    free(error);
                    continue;
                }
                /* It was an invalid IPv4 address, but valid IPv6.
                 * Treat the rest of the handling of this NAT rule
                 * as IPv6. */
                is_v6 = true;
            }

            /* Check the validity of nat->logical_ip. 'logical_ip' can
             * be a subnet when the type is "snat". */
            if (is_v6) {
                error = ipv6_parse_masked(nat->external_ip, &ipv6, &mask_v6);
            } else {
                error = ip_parse_masked(nat->logical_ip, &ip, &mask);
            }
            if (!strcmp(nat->type, "snat")) {
                if (error) {
                    /* Invalid for both IPv4 and IPv6 */
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad ip network or ip %s for snat "
                                 "in router "UUID_FMT"",
                                 nat->logical_ip, UUID_ARGS(&od->key));
                    free(error);
                    continue;
                }
            } else {
                if (error || (!is_v6 && mask != OVS_BE32_MAX)
                    || (is_v6 && memcmp(&mask_v6, &v6_exact,
                                        sizeof mask_v6))) {
                    /* Invalid for both IPv4 and IPv6 */
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad ip %s for dnat in router "
                        ""UUID_FMT"", nat->logical_ip, UUID_ARGS(&od->key));
                    free(error);
                    continue;
                }
            }

            /* For distributed router NAT, determine whether this NAT rule
             * satisfies the conditions for distributed NAT processing. */
            bool distributed = false;
            struct eth_addr mac;
            if (od->l3dgw_port && !strcmp(nat->type, "dnat_and_snat") &&
                nat->logical_port && nat->external_mac) {
                if (eth_addr_from_string(nat->external_mac, &mac)) {
                    distributed = true;
                } else {
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(5, 1);
                    VLOG_WARN_RL(&rl, "bad mac %s for dnat in router "
                        ""UUID_FMT"", nat->external_mac, UUID_ARGS(&od->key));
                    continue;
                }
            }

            /* Ingress UNSNAT table: It is for already established connections'
             * reverse traffic. i.e., SNAT has already been done in egress
             * pipeline and now the packet has entered the ingress pipeline as
             * part of a reply. We undo the SNAT here.
             *
             * Undoing SNAT has to happen before DNAT processing.  This is
             * because when the packet was DNATed in ingress pipeline, it did
             * not know about the possibility of eventual additional SNAT in
             * egress pipeline. */
            if (!strcmp(nat->type, "snat")
                || !strcmp(nat->type, "dnat_and_snat")) {
                if (!od->l3dgw_port) {
                    /* Gateway router. */
                    ds_clear(&match);
                    ds_clear(&actions);
                    ds_put_format(&match, "ip && ip%s.dst == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip);
                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                       ds_put_format(&actions, "ip%s.dst=%s; next;",
                                     is_v6 ? "6" : "4", nat->logical_ip);
                    } else {
                       ds_put_cstr(&actions, "ct_snat;");
                    }

                    ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 90,
                                  ds_cstr(&match), ds_cstr(&actions));
                } else {
                    /* Distributed router. */

                    /* Traffic received on l3dgw_port is subject to NAT. */
                    ds_clear(&match);
                    ds_clear(&actions);
                    ds_put_format(&match, "ip && ip%s.dst == %s"
                                          " && inport == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip,
                                  od->l3dgw_port->json_key);
                    if (!distributed && od->l3redirect_port) {
                        /* Flows for NAT rules that are centralized are only
                         * programmed on the "redirect-chassis". */
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      od->l3redirect_port->json_key);
                    }

                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                        ds_put_format(&actions, "ip%s.dst=%s; next;",
                                      is_v6 ? "6" : "4", nat->logical_ip);
                    } else {
                        ds_put_cstr(&actions, "ct_snat;");
                    }

                    ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 100,
                                  ds_cstr(&match), ds_cstr(&actions));

                    /* Traffic received on other router ports must be
                     * redirected to the central instance of the l3dgw_port
                     * for NAT processing. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.dst == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip);
                    ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 50,
                                  ds_cstr(&match),
                                  REGBIT_NAT_REDIRECT" = 1; next;");
                }
            }

            /* Ingress DNAT table: Packets enter the pipeline with destination
             * IP address that needs to be DNATted from a external IP address
             * to a logical IP address. */
            if (!strcmp(nat->type, "dnat")
                || !strcmp(nat->type, "dnat_and_snat")) {
                if (!od->l3dgw_port) {
                    /* Gateway router. */
                    /* Packet when it goes from the initiator to destination.
                     * We need to set flags.loopback because the router can
                     * send the packet back through the same interface. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.dst == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip);
                    ds_clear(&actions);
                    if (dnat_force_snat_ip) {
                        /* Indicate to the future tables that a DNAT has taken
                         * place and a force SNAT needs to be done in the
                         * Egress SNAT table. */
                        ds_put_format(&actions,
                                      "flags.force_snat_for_dnat = 1; ");
                    }

                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                        ds_put_format(&actions, "flags.loopback = 1; "
                                      "ip%s.dst=%s; next;",
                                      is_v6 ? "6" : "4", nat->logical_ip);
                    } else {
                        ds_put_format(&actions, "flags.loopback = 1; "
                                      "ct_dnat(%s);", nat->logical_ip);
                    }

                    ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, 100,
                                  ds_cstr(&match), ds_cstr(&actions));
                } else {
                    /* Distributed router. */

                    /* Traffic received on l3dgw_port is subject to NAT. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.dst == %s"
                                          " && inport == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip,
                                  od->l3dgw_port->json_key);
                    if (!distributed && od->l3redirect_port) {
                        /* Flows for NAT rules that are centralized are only
                         * programmed on the "redirect-chassis". */
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      od->l3redirect_port->json_key);
                    }
                    ds_clear(&actions);

                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                        ds_put_format(&actions, "ip%s.dst=%s; next;",
                                      is_v6 ? "6" : "4", nat->logical_ip);
                    } else {
                        ds_put_format(&actions, "ct_dnat(%s);",
                                      nat->logical_ip);
                    }

                    ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, 100,
                                  ds_cstr(&match), ds_cstr(&actions));

                    /* Traffic received on other router ports must be
                     * redirected to the central instance of the l3dgw_port
                     * for NAT processing. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.dst == %s",
                                  is_v6 ? "6" : "4",
                                  nat->external_ip);
                    ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, 50,
                                  ds_cstr(&match),
                                  REGBIT_NAT_REDIRECT" = 1; next;");
                }
            }

            /* Egress UNDNAT table: It is for already established connections'
             * reverse traffic. i.e., DNAT has already been done in ingress
             * pipeline and now the packet has entered the egress pipeline as
             * part of a reply. We undo the DNAT here.
             *
             * Note that this only applies for NAT on a distributed router.
             * Undo DNAT on a gateway router is done in the ingress DNAT
             * pipeline stage. */
            if (od->l3dgw_port && (!strcmp(nat->type, "dnat")
                || !strcmp(nat->type, "dnat_and_snat"))) {
                ds_clear(&match);
                ds_put_format(&match, "ip && ip%s.src == %s"
                                      " && outport == %s",
                              is_v6 ? "6" : "4",
                              nat->logical_ip,
                              od->l3dgw_port->json_key);
                if (!distributed && od->l3redirect_port) {
                    /* Flows for NAT rules that are centralized are only
                     * programmed on the "redirect-chassis". */
                    ds_put_format(&match, " && is_chassis_resident(%s)",
                                  od->l3redirect_port->json_key);
                }
                ds_clear(&actions);
                if (distributed) {
                    ds_put_format(&actions, "eth.src = "ETH_ADDR_FMT"; ",
                                  ETH_ADDR_ARGS(mac));
                }

                if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                    ds_put_format(&actions, "ip%s.src=%s; next;",
                                  is_v6 ? "6" : "4", nat->external_ip);
                } else {
                    ds_put_format(&actions, "ct_dnat;");
                }

                ovn_lflow_add(lflows, od, S_ROUTER_OUT_UNDNAT, 100,
                              ds_cstr(&match), ds_cstr(&actions));
            }

            /* Egress SNAT table: Packets enter the egress pipeline with
             * source ip address that needs to be SNATted to a external ip
             * address. */
            if (!strcmp(nat->type, "snat")
                || !strcmp(nat->type, "dnat_and_snat")) {
                if (!od->l3dgw_port) {
                    /* Gateway router. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.src == %s",
                                  is_v6 ? "6" : "4",
                                  nat->logical_ip);
                    ds_clear(&actions);

                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                        ds_put_format(&actions, "ip%s.src=%s; next;",
                                      is_v6 ? "6" : "4", nat->external_ip);
                    } else {
                        ds_put_format(&actions, "ct_snat(%s);",
                                      nat->external_ip);
                    }

                    /* The priority here is calculated such that the
                     * nat->logical_ip with the longest mask gets a higher
                     * priority. */
                    ovn_lflow_add(lflows, od, S_ROUTER_OUT_SNAT,
                                  count_1bits(ntohl(mask)) + 1,
                                  ds_cstr(&match), ds_cstr(&actions));
                } else {
                    uint16_t priority = count_1bits(ntohl(mask)) + 1;

                    /* Distributed router. */
                    ds_clear(&match);
                    ds_put_format(&match, "ip && ip%s.src == %s"
                                          " && outport == %s",
                                  is_v6 ? "6" : "4",
                                  nat->logical_ip,
                                  od->l3dgw_port->json_key);
                    if (!distributed && od->l3redirect_port) {
                        /* Flows for NAT rules that are centralized are only
                         * programmed on the "redirect-chassis". */
                        priority += 128;
                        ds_put_format(&match, " && is_chassis_resident(%s)",
                                      od->l3redirect_port->json_key);
                    }
                    ds_clear(&actions);
                    if (distributed) {
                        ds_put_format(&actions, "eth.src = "ETH_ADDR_FMT"; ",
                                      ETH_ADDR_ARGS(mac));
                    }

                    if (!strcmp(nat->type, "dnat_and_snat") && stateless) {
                        ds_put_format(&actions, "ip%s.src=%s; next;",
                                      is_v6 ? "6" : "4", nat->external_ip);
                    } else {
                        ds_put_format(&actions, "ct_snat(%s);",
                                      nat->external_ip);
                    }

                    /* The priority here is calculated such that the
                     * nat->logical_ip with the longest mask gets a higher
                     * priority. */
                    ovn_lflow_add(lflows, od, S_ROUTER_OUT_SNAT,
                                  priority, ds_cstr(&match),
                                  ds_cstr(&actions));
                }
            }

            /* Logical router ingress table 0:
             * For NAT on a distributed router, add rules allowing
             * ingress traffic with eth.dst matching nat->external_mac
             * on the l3dgw_port instance where nat->logical_port is
             * resident. */
            if (distributed) {
                ds_clear(&match);
                ds_put_format(&match,
                              "eth.dst == "ETH_ADDR_FMT" && inport == %s"
                              " && is_chassis_resident(\"%s\")",
                              ETH_ADDR_ARGS(mac),
                              od->l3dgw_port->json_key,
                              nat->logical_port);
                ovn_lflow_add(lflows, od, S_ROUTER_IN_ADMISSION, 50,
                              ds_cstr(&match), "next;");
            }

            /* Ingress Gateway Redirect Table: For NAT on a distributed
             * router, add flows that are specific to a NAT rule.  These
             * flows indicate the presence of an applicable NAT rule that
             * can be applied in a distributed manner. */
            if (distributed) {
                ds_clear(&match);
                ds_put_format(&match, "ip%s.src == %s && outport == %s",
                              is_v6 ? "6" : "4",
                              nat->logical_ip,
                              od->l3dgw_port->json_key);
                ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 100,
                              ds_cstr(&match), "next;");
            }

            /* Egress Loopback table: For NAT on a distributed router.
             * If packets in the egress pipeline on the distributed
             * gateway port have ip.dst matching a NAT external IP, then
             * loop a clone of the packet back to the beginning of the
             * ingress pipeline with inport = outport. */
            if (od->l3dgw_port) {
                /* Distributed router. */
                if (!strcmp(nat->type, "dnat_and_snat") &&
                    nat->external_mac && nat->external_ip) {
                    for (int j = 0; j < od->nbr->n_nat; j++) {
                        const struct nbrec_nat *nat2 = od->nbr->nat[j];

                        if (nat2 == nat ||
                            strcmp(nat2->type, "dnat_and_snat") ||
                            !nat2->external_mac || !nat2->external_ip) {
                            continue;
                        }

                        ds_clear(&match);
                        ds_put_format(&match, "is_chassis_resident(\"%s\") && "
                                      "ip%s.src == %s && ip%s.dst == %s",
                                      nat->logical_port,
                                      is_v6 ? "6" : "4", nat2->external_ip,
                                      is_v6 ? "6" : "4", nat->external_ip);
                        ds_clear(&actions);
                        ds_put_format(&actions,
                                      "inport = outport; outport = \"\"; "
                                      "flags = 0; flags.loopback = 1; "
                                      REGBIT_EGRESS_LOOPBACK" = 1; "
                                      "next(pipeline=ingress, table=0); ");
                        ovn_lflow_add(lflows, od, S_ROUTER_OUT_EGR_LOOP, 300,
                                      ds_cstr(&match),  ds_cstr(&actions));

                        ds_clear(&match);
                        ds_put_format(&match,
                                      "ip%s.src == %s && ip%s.dst == %s",
                                      is_v6 ? "6" : "4", nat2->external_ip,
                                      is_v6 ? "6" : "4", nat->external_ip);
                        ovn_lflow_add(lflows, od, S_ROUTER_OUT_EGR_LOOP, 200,
                                      ds_cstr(&match), "next;");
                        ds_clear(&match);
                    }
                }

                ds_clear(&match);
                ds_put_format(&match, "ip%s.dst == %s && outport == %s",
                              is_v6 ? "6" : "4",
                              nat->external_ip,
                              od->l3dgw_port->json_key);
                ds_clear(&actions);
                ds_put_format(&actions,
                              "clone { ct_clear; "
                              "inport = outport; outport = \"\"; "
                              "flags = 0; flags.loopback = 1; ");
                for (int j = 0; j < MFF_N_LOG_REGS; j++) {
                    ds_put_format(&actions, "reg%d = 0; ", j);
                }
                ds_put_format(&actions, REGBIT_EGRESS_LOOPBACK" = 1; "
                              "next(pipeline=ingress, table=0); };");
                ovn_lflow_add(lflows, od, S_ROUTER_OUT_EGR_LOOP, 100,
                              ds_cstr(&match), ds_cstr(&actions));
            }
        }

        /* Handle force SNAT options set in the gateway router. */
        if (dnat_force_snat_ip && !od->l3dgw_port) {
            /* If a packet with destination IP address as that of the
             * gateway router (as set in options:dnat_force_snat_ip) is seen,
             * UNSNAT it. */
            ds_clear(&match);
            ds_put_format(&match, "ip && ip%s.dst == %s",
                          snat_ip.family == AF_INET ? "4" : "6",
                          dnat_force_snat_ip);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 110,
                          ds_cstr(&match), "ct_snat;");

            /* Higher priority rules to force SNAT with the IP addresses
             * configured in the Gateway router.  This only takes effect
             * when the packet has already been DNATed once. */
            ds_clear(&match);
            ds_put_format(&match, "flags.force_snat_for_dnat == 1 && ip");
            ds_clear(&actions);
            ds_put_format(&actions, "ct_snat(%s);", dnat_force_snat_ip);
            ovn_lflow_add(lflows, od, S_ROUTER_OUT_SNAT, 100,
                          ds_cstr(&match), ds_cstr(&actions));
        }
        if (lb_force_snat_ip && !od->l3dgw_port) {
            /* If a packet with destination IP address as that of the
             * gateway router (as set in options:lb_force_snat_ip) is seen,
             * UNSNAT it. */
            ds_clear(&match);
            ds_put_format(&match, "ip && ip%s.dst == %s",
                          lb_snat_ip.family == AF_INET ? "4" : "6",
                          lb_force_snat_ip);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_UNSNAT, 100,
                          ds_cstr(&match), "ct_snat;");

            /* Load balanced traffic will have flags.force_snat_for_lb set.
             * Force SNAT it. */
            ds_clear(&match);
            ds_put_format(&match, "flags.force_snat_for_lb == 1 && ip");
            ds_clear(&actions);
            ds_put_format(&actions, "ct_snat(%s);", lb_force_snat_ip);
            ovn_lflow_add(lflows, od, S_ROUTER_OUT_SNAT, 100,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        if (!od->l3dgw_port) {
            /* For gateway router, re-circulate every packet through
            * the DNAT zone.  This helps with the following.
            *
            * Any packet that needs to be unDNATed in the reverse
            * direction gets unDNATed. Ideally this could be done in
            * the egress pipeline. But since the gateway router
            * does not have any feature that depends on the source
            * ip address being external IP address for IP routing,
            * we can do it here, saving a future re-circulation. */
            ovn_lflow_add(lflows, od, S_ROUTER_IN_DNAT, 50,
                          "ip", "flags.loopback = 1; ct_dnat;");
        } else {
            ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_RESOLVE, 400,
                          REGBIT_DISTRIBUTED_NAT" == 1", "next;");

            /* For NAT on a distributed router, add flows to Ingress
             * IP Routing table, Ingress ARP Resolution table, and
             * Ingress Gateway Redirect Table that are not specific to a
             * NAT rule. */

            /* The highest priority IN_IP_ROUTING rule matches packets
             * with REGBIT_NAT_REDIRECT (set in DNAT or UNSNAT stages),
             * with action "ip.ttl--; next;".  The IN_GW_REDIRECT table
             * will take care of setting the outport. */
            ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_ROUTING, 300,
                          REGBIT_NAT_REDIRECT" == 1", "ip.ttl--; next;");

            /* The highest priority IN_ARP_RESOLVE rule matches packets
             * with REGBIT_NAT_REDIRECT (set in DNAT or UNSNAT stages),
             * then sets eth.dst to the distributed gateway port's
             * ethernet address. */
            ds_clear(&actions);
            ds_put_format(&actions, "eth.dst = %s; next;",
                          od->l3dgw_port->lrp_networks.ea_s);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_RESOLVE, 200,
                          REGBIT_NAT_REDIRECT" == 1", ds_cstr(&actions));

            /* The highest priority IN_GW_REDIRECT rule redirects packets
             * with REGBIT_NAT_REDIRECT (set in DNAT or UNSNAT stages) to
             * the central instance of the l3dgw_port for NAT processing. */
            ds_clear(&actions);
            ds_put_format(&actions, "outport = %s; next;",
                          od->l3redirect_port->json_key);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 200,
                          REGBIT_NAT_REDIRECT" == 1", ds_cstr(&actions));
        }

        /* Load balancing and packet defrag are only valid on
         * Gateway routers or router with gateway port. */
        if (!smap_get(&od->nbr->options, "chassis") && !od->l3dgw_port) {
            continue;
        }

        /* A set to hold all ips that need defragmentation and tracking. */
        struct sset all_ips = SSET_INITIALIZER(&all_ips);

        for (int i = 0; i < od->nbr->n_load_balancer; i++) {
            struct nbrec_load_balancer *lb = od->nbr->load_balancer[i];
            struct smap *vips = &lb->vips;
            struct smap_node *node;

            SMAP_FOR_EACH (node, vips) {
                uint16_t port = 0;
                int addr_family;

                /* node->key contains IP:port or just IP. */
                char *ip_address = NULL;
                ip_address_and_port_from_lb_key(node->key, &ip_address, &port,
                        &addr_family);
                if (!ip_address) {
                    continue;
                }

                if (!sset_contains(&all_ips, ip_address)) {
                    sset_add(&all_ips, ip_address);
                    /* If there are any load balancing rules, we should send
                     * the packet to conntrack for defragmentation and
                     * tracking.  This helps with two things.
                     *
                     * 1. With tracking, we can send only new connections to
                     *    pick a DNAT ip address from a group.
                     * 2. If there are L4 ports in load balancing rules, we
                     *    need the defragmentation to match on L4 ports. */
                    ds_clear(&match);
                    if (addr_family == AF_INET) {
                        ds_put_format(&match, "ip && ip4.dst == %s",
                                      ip_address);
                    } else if (addr_family == AF_INET6) {
                        ds_put_format(&match, "ip && ip6.dst == %s",
                                      ip_address);
                    }
                    ovn_lflow_add(lflows, od, S_ROUTER_IN_DEFRAG,
                                  100, ds_cstr(&match), "ct_next;");
                }

                /* Higher priority rules are added for load-balancing in DNAT
                 * table.  For every match (on a VIP[:port]), we add two flows
                 * via add_router_lb_flow().  One flow is for specific matching
                 * on ct.new with an action of "ct_lb($targets);".  The other
                 * flow is for ct.est with an action of "ct_dnat;". */
                ds_clear(&actions);
                ds_put_format(&actions, "ct_lb(%s);", node->value);

                ds_clear(&match);
                if (addr_family == AF_INET) {
                    ds_put_format(&match, "ip && ip4.dst == %s",
                                ip_address);
                } else if (addr_family == AF_INET6) {
                    ds_put_format(&match, "ip && ip6.dst == %s",
                                ip_address);
                }

                int prio = 110;
                bool is_udp = lb->protocol && !strcmp(lb->protocol, "udp") ?
                    true : false;
                if (port) {
                    if (is_udp) {
                        ds_put_format(&match, " && udp && udp.dst == %d",
                                      port);
                    } else {
                        ds_put_format(&match, " && tcp && tcp.dst == %d",
                                      port);
                    }
                    prio = 120;
                }

                if (od->l3redirect_port) {
                    ds_put_format(&match, " && is_chassis_resident(%s)",
                                  od->l3redirect_port->json_key);
                }
                add_router_lb_flow(lflows, od, &match, &actions, prio,
                                   lb_force_snat_ip, node, is_udp,
                                   addr_family, ip_address, port, lb,
                                   meter_groups);

                free(ip_address);
            }
        }
        sset_destroy(&all_ips);
    }

    /* Logical router ingress table 5 and 6: IPv6 Router Adv (RA) options and
     * response. */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp || op->nbrp->peer || !op->peer) {
            continue;
        }

        if (!op->lrp_networks.n_ipv6_addrs) {
            continue;
        }

        const char *address_mode = smap_get(
            &op->nbrp->ipv6_ra_configs, "address_mode");

        if (!address_mode) {
            continue;
        }
        if (strcmp(address_mode, "slaac") &&
            strcmp(address_mode, "dhcpv6_stateful") &&
            strcmp(address_mode, "dhcpv6_stateless")) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_WARN_RL(&rl, "Invalid address mode [%s] defined",
                         address_mode);
            continue;
        }

        if (smap_get_bool(&op->nbrp->ipv6_ra_configs, "send_periodic",
                          false)) {
            copy_ra_to_sb(op, address_mode);
        }

        ds_clear(&match);
        ds_put_format(&match, "inport == %s && ip6.dst == ff02::2 && nd_rs",
                              op->json_key);
        ds_clear(&actions);

        const char *mtu_s = smap_get(
            &op->nbrp->ipv6_ra_configs, "mtu");

        /* As per RFC 2460, 1280 is minimum IPv6 MTU. */
        uint32_t mtu = (mtu_s && atoi(mtu_s) >= 1280) ? atoi(mtu_s) : 0;

        ds_put_format(&actions, REGBIT_ND_RA_OPTS_RESULT" = put_nd_ra_opts("
                      "addr_mode = \"%s\", slla = %s",
                      address_mode, op->lrp_networks.ea_s);
        if (mtu > 0) {
            ds_put_format(&actions, ", mtu = %u", mtu);
        }

        bool add_rs_response_flow = false;

        for (size_t i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            if (in6_is_lla(&op->lrp_networks.ipv6_addrs[i].network)) {
                continue;
            }

            ds_put_format(&actions, ", prefix = %s/%u",
                          op->lrp_networks.ipv6_addrs[i].network_s,
                          op->lrp_networks.ipv6_addrs[i].plen);

            add_rs_response_flow = true;
        }

        if (add_rs_response_flow) {
            ds_put_cstr(&actions, "); next;");
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_ND_RA_OPTIONS, 50,
                          ds_cstr(&match), ds_cstr(&actions));
            ds_clear(&actions);
            ds_clear(&match);
            ds_put_format(&match, "inport == %s && ip6.dst == ff02::2 && "
                          "nd_ra && "REGBIT_ND_RA_OPTS_RESULT, op->json_key);

            char ip6_str[INET6_ADDRSTRLEN + 1];
            struct in6_addr lla;
            in6_generate_lla(op->lrp_networks.ea, &lla);
            memset(ip6_str, 0, sizeof(ip6_str));
            ipv6_string_mapped(ip6_str, &lla);
            ds_put_format(&actions, "eth.dst = eth.src; eth.src = %s; "
                          "ip6.dst = ip6.src; ip6.src = %s; "
                          "outport = inport; flags.loopback = 1; "
                          "output;",
                          op->lrp_networks.ea_s, ip6_str);
            ovn_lflow_add(lflows, op->od, S_ROUTER_IN_ND_RA_RESPONSE, 50,
                          ds_cstr(&match), ds_cstr(&actions));
        }
    }

    /* Logical router ingress table 5, 6: RS responder, by default goto next.
     * (priority 0)*/
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_ROUTER_IN_ND_RA_OPTIONS, 0, "1", "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_ND_RA_RESPONSE, 0, "1", "next;");
    }

    /* Logical router ingress table 7: IP Routing.
     *
     * A packet that arrives at this table is an IP packet that should be
     * routed to the address in 'ip[46].dst'. This table sets outport to
     * the correct output port, eth.src to the output port's MAC
     * address, and '[xx]reg0' to the next-hop IP address (leaving
     * 'ip[46].dst', the packet’s final destination, unchanged), and
     * advances to the next table for ARP/ND resolution. */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        /* create logical flows for DVR floating IPs */
        add_distributed_nat_routes(lflows, op);

        for (int i = 0; i < op->lrp_networks.n_ipv4_addrs; i++) {
            add_route(lflows, op, op->lrp_networks.ipv4_addrs[i].addr_s,
                      op->lrp_networks.ipv4_addrs[i].network_s,
                      op->lrp_networks.ipv4_addrs[i].plen, NULL, NULL);
        }

        for (int i = 0; i < op->lrp_networks.n_ipv6_addrs; i++) {
            add_route(lflows, op, op->lrp_networks.ipv6_addrs[i].addr_s,
                      op->lrp_networks.ipv6_addrs[i].network_s,
                      op->lrp_networks.ipv6_addrs[i].plen, NULL, NULL);
        }
    }

    /* Convert the static routes to flows. */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        for (int i = 0; i < od->nbr->n_static_routes; i++) {
            const struct nbrec_logical_router_static_route *route;

            route = od->nbr->static_routes[i];
            build_static_route_flow(lflows, od, ports, route);
        }
    }

    /* IP Multicast lookup. Here we set the output port, adjust TTL and
     * advance to next table (priority 500).
     */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr || !od->mcast_info.rtr.relay) {
            continue;
        }

        struct ovn_igmp_group *igmp_group;

        LIST_FOR_EACH (igmp_group, list_node, &od->mcast_info.groups) {
            ds_clear(&match);
            ds_clear(&actions);
            ds_put_format(&match, "ip4 && ip4.dst == %s ",
                          igmp_group->mcgroup.name);
            if (od->mcast_info.rtr.flood_static) {
                ds_put_cstr(&actions,
                            "clone { "
                                "outport = \""MC_STATIC"\"; "
                                "ip.ttl--; "
                                "next; "
                            "};");
            }
            ds_put_format(&actions, "outport = \"%s\"; ip.ttl--; next;",
                          igmp_group->mcgroup.name);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_ROUTING, 500,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* If needed, flood unregistered multicast on statically configured
         * ports.
         */
        if (od->mcast_info.rtr.flood_static) {
            ds_clear(&match);
            ds_clear(&actions);
            ds_put_format(&match, "ip4.mcast");
            ovn_lflow_add(lflows, od, S_ROUTER_IN_IP_ROUTING, 450,
                          "ip4.mcast",
                          "clone { "
                                "outport = \""MC_STATIC"\"; "
                                "ip.ttl--; "
                                "next; "
                          "};");
        }
    }

    /* Logical router ingress table 8: Policy.
     *
     * A packet that arrives at this table is an IP packet that should be
     * permitted/denied/rerouted to the address in the rule's nexthop.
     * This table sets outport to the correct out_port,
     * eth.src to the output port's MAC address,
     * and '[xx]reg0' to the next-hop IP address (leaving
     * 'ip[46].dst', the packet’s final destination, unchanged), and
     * advances to the next table for ARP/ND resolution. */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }
        /* This is a catch-all rule. It has the lowest priority (0)
         * does a match-all("1") and pass-through (next) */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_POLICY, 0, "1", "next;");

        /* Convert routing policies to flows. */
        for (int i = 0; i < od->nbr->n_policies; i++) {
            const struct nbrec_logical_router_policy *rule
                = od->nbr->policies[i];
            build_routing_policy_flow(lflows, od, ports, rule);
        }
    }


    /* XXX destination unreachable */

    /* Local router ingress table 9: ARP Resolution.
     *
     * Multicast packets already have the outport set so just advance to next
     * table (priority 500). */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_RESOLVE, 500,
                      "ip4.mcast", "next;");
    }

    /* Local router ingress table 9: ARP Resolution.
     *
     * Any unicast packet that reaches this table is an IP packet whose
     * next-hop IP address is in reg0. (ip4.dst is the final destination.)
     * This table resolves the IP address in reg0 into an output port in
     * outport and an Ethernet address in eth.dst.
     */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (op->nbsp && !lsp_is_enabled(op->nbsp)) {
            continue;
        }

        if (op->nbrp) {
            /* This is a logical router port. If next-hop IP address in
             * '[xx]reg0' matches IP address of this router port, then
             * the packet is intended to eventually be sent to this
             * logical port. Set the destination mac address using this
             * port's mac address.
             *
             * The packet is still in peer's logical pipeline. So the match
             * should be on peer's outport. */
            if (op->peer && op->nbrp->peer) {
                if (op->lrp_networks.n_ipv4_addrs) {
                    ds_clear(&match);
                    ds_put_format(&match, "outport == %s && reg0 == ",
                                  op->peer->json_key);
                    op_put_v4_networks(&match, op, false);

                    ds_clear(&actions);
                    ds_put_format(&actions, "eth.dst = %s; next;",
                                  op->lrp_networks.ea_s);
                    ovn_lflow_add(lflows, op->peer->od, S_ROUTER_IN_ARP_RESOLVE,
                                  100, ds_cstr(&match), ds_cstr(&actions));
                }

                if (op->lrp_networks.n_ipv6_addrs) {
                    ds_clear(&match);
                    ds_put_format(&match, "outport == %s && xxreg0 == ",
                                  op->peer->json_key);
                    op_put_v6_networks(&match, op);

                    ds_clear(&actions);
                    ds_put_format(&actions, "eth.dst = %s; next;",
                                  op->lrp_networks.ea_s);
                    ovn_lflow_add(lflows, op->peer->od, S_ROUTER_IN_ARP_RESOLVE,
                                  100, ds_cstr(&match), ds_cstr(&actions));
                }
            }

            if (!op->derived && op->od->l3redirect_port) {
                const char *redirect_type = smap_get(&op->nbrp->options,
                                                     "redirect-type");
                if (redirect_type && !strcasecmp(redirect_type, "bridged")) {
                    /* Packet is on a non gateway chassis and
                     * has an unresolved ARP on a network behind gateway
                     * chassis attached router port. Since, redirect type
                     * is set to vlan, hence instead of calling "get_arp"
                     * on this node, we will redirect the packet to gateway
                     * chassis, by setting destination mac router port mac.*/
                    ds_clear(&match);
                    ds_put_format(&match, "outport == %s && "
                                  "!is_chassis_resident(%s)", op->json_key,
                                  op->od->l3redirect_port->json_key);
                    ds_clear(&actions);
                    ds_put_format(&actions, "eth.dst = %s; next;",
                                  op->lrp_networks.ea_s);
                    ovn_lflow_add(lflows, op->od, S_ROUTER_IN_ARP_RESOLVE,
                                  50, ds_cstr(&match), ds_cstr(&actions));
                }
            }
        } else if (op->od->n_router_ports && strcmp(op->nbsp->type, "router")
                   && strcmp(op->nbsp->type, "virtual")) {
            /* This is a logical switch port that backs a VM or a container.
             * Extract its addresses. For each of the address, go through all
             * the router ports attached to the switch (to which this port
             * connects) and if the address in question is reachable from the
             * router port, add an ARP/ND entry in that router's pipeline. */

            for (size_t i = 0; i < op->n_lsp_addrs; i++) {
                const char *ea_s = op->lsp_addrs[i].ea_s;
                for (size_t j = 0; j < op->lsp_addrs[i].n_ipv4_addrs; j++) {
                    const char *ip_s = op->lsp_addrs[i].ipv4_addrs[j].addr_s;
                    for (size_t k = 0; k < op->od->n_router_ports; k++) {
                        /* Get the Logical_Router_Port that the
                         * Logical_Switch_Port is connected to, as
                         * 'peer'. */
                        const char *peer_name = smap_get(
                            &op->od->router_ports[k]->nbsp->options,
                            "router-port");
                        if (!peer_name) {
                            continue;
                        }

                        struct ovn_port *peer = ovn_port_find(ports, peer_name);
                        if (!peer || !peer->nbrp) {
                            continue;
                        }

                        if (!find_lrp_member_ip(peer, ip_s)) {
                            continue;
                        }

                        ds_clear(&match);
                        ds_put_format(&match, "outport == %s && reg0 == %s",
                                      peer->json_key, ip_s);

                        ds_clear(&actions);
                        ds_put_format(&actions, "eth.dst = %s; next;", ea_s);
                        ovn_lflow_add(lflows, peer->od,
                                      S_ROUTER_IN_ARP_RESOLVE, 100,
                                      ds_cstr(&match), ds_cstr(&actions));
                    }
                }

                for (size_t j = 0; j < op->lsp_addrs[i].n_ipv6_addrs; j++) {
                    const char *ip_s = op->lsp_addrs[i].ipv6_addrs[j].addr_s;
                    for (size_t k = 0; k < op->od->n_router_ports; k++) {
                        /* Get the Logical_Router_Port that the
                         * Logical_Switch_Port is connected to, as
                         * 'peer'. */
                        const char *peer_name = smap_get(
                            &op->od->router_ports[k]->nbsp->options,
                            "router-port");
                        if (!peer_name) {
                            continue;
                        }

                        struct ovn_port *peer = ovn_port_find(ports, peer_name);
                        if (!peer || !peer->nbrp) {
                            continue;
                        }

                        if (!find_lrp_member_ip(peer, ip_s)) {
                            continue;
                        }

                        ds_clear(&match);
                        ds_put_format(&match, "outport == %s && xxreg0 == %s",
                                      peer->json_key, ip_s);

                        ds_clear(&actions);
                        ds_put_format(&actions, "eth.dst = %s; next;", ea_s);
                        ovn_lflow_add(lflows, peer->od,
                                      S_ROUTER_IN_ARP_RESOLVE, 100,
                                      ds_cstr(&match), ds_cstr(&actions));
                    }
                }
            }
        } else if (op->od->n_router_ports && strcmp(op->nbsp->type, "router")
                   && !strcmp(op->nbsp->type, "virtual")) {
            /* This is a virtual port. Add ARP replies for the virtual ip with
             * the mac of the present active virtual parent.
             * If the logical port doesn't have virtual parent set in
             * Port_Binding table, then add the flow to set eth.dst to
             * 00:00:00:00:00:00 and advance to next table so that ARP is
             * resolved by router pipeline using the arp{} action.
             * The MAC_Binding entry for the virtual ip might be invalid. */
            ovs_be32 ip;

            const char *vip = smap_get(&op->nbsp->options,
                                       "virtual-ip");
            const char *virtual_parents = smap_get(&op->nbsp->options,
                                                   "virtual-parents");
            if (!vip || !virtual_parents ||
                !ip_parse(vip, &ip) || !op->sb) {
                continue;
            }

            if (!op->sb->virtual_parent || !op->sb->virtual_parent[0] ||
                !op->sb->chassis) {
                /* The virtual port is not claimed yet. */
                for (size_t i = 0; i < op->od->n_router_ports; i++) {
                    const char *peer_name = smap_get(
                        &op->od->router_ports[i]->nbsp->options,
                        "router-port");
                    if (!peer_name) {
                        continue;
                    }

                    struct ovn_port *peer = ovn_port_find(ports, peer_name);
                    if (!peer || !peer->nbrp) {
                        continue;
                    }

                    if (find_lrp_member_ip(peer, vip)) {
                        ds_clear(&match);
                        ds_put_format(&match, "outport == %s && reg0 == %s",
                                        peer->json_key, vip);

                        ds_clear(&actions);
                        ds_put_format(&actions,
                                      "eth.dst = 00:00:00:00:00:00; next;");
                        ovn_lflow_add(lflows, peer->od,
                                        S_ROUTER_IN_ARP_RESOLVE, 100,
                                        ds_cstr(&match), ds_cstr(&actions));
                        break;
                    }
                }
            } else {
                struct ovn_port *vp =
                    ovn_port_find(ports, op->sb->virtual_parent);
                if (!vp || !vp->nbsp) {
                    continue;
                }

                for (size_t i = 0; i < vp->n_lsp_addrs; i++) {
                    bool found_vip_network = false;
                    const char *ea_s = vp->lsp_addrs[i].ea_s;
                    for (size_t j = 0; j < vp->od->n_router_ports; j++) {
                        /* Get the Logical_Router_Port that the
                        * Logical_Switch_Port is connected to, as
                        * 'peer'. */
                        const char *peer_name = smap_get(
                            &vp->od->router_ports[j]->nbsp->options,
                            "router-port");
                        if (!peer_name) {
                            continue;
                        }

                        struct ovn_port *peer =
                            ovn_port_find(ports, peer_name);
                        if (!peer || !peer->nbrp) {
                            continue;
                        }

                        if (!find_lrp_member_ip(peer, vip)) {
                            continue;
                        }

                        ds_clear(&match);
                        ds_put_format(&match, "outport == %s && reg0 == %s",
                                        peer->json_key, vip);

                        ds_clear(&actions);
                        ds_put_format(&actions, "eth.dst = %s; next;", ea_s);
                        ovn_lflow_add(lflows, peer->od,
                                        S_ROUTER_IN_ARP_RESOLVE, 100,
                                        ds_cstr(&match), ds_cstr(&actions));
                        found_vip_network = true;
                        break;
                    }

                    if (found_vip_network) {
                        break;
                    }
                }
            }
        } else if (!strcmp(op->nbsp->type, "router")) {
            /* This is a logical switch port that connects to a router. */

            /* The peer of this switch port is the router port for which
             * we need to add logical flows such that it can resolve
             * ARP entries for all the other router ports connected to
             * the switch in question. */

            const char *peer_name = smap_get(&op->nbsp->options,
                                             "router-port");
            if (!peer_name) {
                continue;
            }

            struct ovn_port *peer = ovn_port_find(ports, peer_name);
            if (!peer || !peer->nbrp) {
                continue;
            }

            for (size_t i = 0; i < op->od->n_router_ports; i++) {
                const char *router_port_name = smap_get(
                                    &op->od->router_ports[i]->nbsp->options,
                                    "router-port");
                struct ovn_port *router_port = ovn_port_find(ports,
                                                             router_port_name);
                if (!router_port || !router_port->nbrp) {
                    continue;
                }

                /* Skip the router port under consideration. */
                if (router_port == peer) {
                   continue;
                }

                if (router_port->lrp_networks.n_ipv4_addrs) {
                    ds_clear(&match);
                    ds_put_format(&match, "outport == %s && reg0 == ",
                                  peer->json_key);
                    op_put_v4_networks(&match, router_port, false);

                    ds_clear(&actions);
                    ds_put_format(&actions, "eth.dst = %s; next;",
                                              router_port->lrp_networks.ea_s);
                    ovn_lflow_add(lflows, peer->od, S_ROUTER_IN_ARP_RESOLVE,
                                  100, ds_cstr(&match), ds_cstr(&actions));
                }

                if (router_port->lrp_networks.n_ipv6_addrs) {
                    ds_clear(&match);
                    ds_put_format(&match, "outport == %s && xxreg0 == ",
                                  peer->json_key);
                    op_put_v6_networks(&match, router_port);

                    ds_clear(&actions);
                    ds_put_format(&actions, "eth.dst = %s; next;",
                                  router_port->lrp_networks.ea_s);
                    ovn_lflow_add(lflows, peer->od, S_ROUTER_IN_ARP_RESOLVE,
                                  100, ds_cstr(&match), ds_cstr(&actions));
                }
            }
        }
    }

    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_RESOLVE, 0, "ip4",
                      "get_arp(outport, reg0); next;");

        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_RESOLVE, 0, "ip6",
                      "get_nd(outport, xxreg0); next;");
    }

    /* Local router ingress table 10: Check packet length.
     *
     * Any IPv4 packet with outport set to the distributed gateway
     * router port, check the packet length and store the result in the
     * 'REGBIT_PKT_LARGER' register bit.
     *
     * Local router ingress table 11: Handle larger packets.
     *
     * Any IPv4 packet with outport set to the distributed gateway
     * router port and the 'REGBIT_PKT_LARGER' register bit is set,
     * generate ICMPv4 packet with type 3 (Destination Unreachable) and
     * code 4 (Fragmentation needed).
     * */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        /* Packets are allowed by default. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_CHK_PKT_LEN, 0, "1",
                      "next;");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_LARGER_PKTS, 0, "1",
                      "next;");

        if (od->l3dgw_port && od->l3redirect_port) {
            int gw_mtu = 0;
            if (od->l3dgw_port->nbrp) {
                 gw_mtu = smap_get_int(&od->l3dgw_port->nbrp->options,
                                       "gateway_mtu", 0);
            }
            /* Add the flows only if gateway_mtu is configured. */
            if (gw_mtu <= 0) {
                continue;
            }

            ds_clear(&match);
            ds_put_format(&match, "outport == %s && ip4",
                          od->l3dgw_port->json_key);

            ds_clear(&actions);
            ds_put_format(&actions,
                          REGBIT_PKT_LARGER" = check_pkt_larger(%d);"
                          " next;", gw_mtu);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_CHK_PKT_LEN, 50,
                          ds_cstr(&match), ds_cstr(&actions));

            for (size_t i = 0; i < od->nbr->n_ports; i++) {
                struct ovn_port *rp = ovn_port_find(ports,
                                                    od->nbr->ports[i]->name);
                if (!rp || rp == od->l3dgw_port ||
                    !rp->lrp_networks.ipv4_addrs) {
                    continue;
                }
                ds_clear(&match);
                ds_put_format(&match, "inport == %s && outport == %s && ip4 "
                              "&& "REGBIT_PKT_LARGER,
                              rp->json_key, od->l3dgw_port->json_key);

                ds_clear(&actions);
                /* Set icmp4.frag_mtu to gw_mtu - 58. 58 is the Geneve tunnel
                 * overhead. */
                ds_put_format(&actions,
                    "icmp4_error {"
                    REGBIT_EGRESS_LOOPBACK" = 1; "
                    "eth.dst = %s; "
                    "ip4.dst = ip4.src; "
                    "ip4.src = %s; "
                    "ip.ttl = 255; "
                    "icmp4.type = 3; /* Destination Unreachable. */ "
                    "icmp4.code = 4; /* Frag Needed and DF was Set. */ "
                    "icmp4.frag_mtu = %d; "
                    "next(pipeline=ingress, table=0); };",
                    rp->lrp_networks.ea_s,
                    rp->lrp_networks.ipv4_addrs[0].addr_s,
                    gw_mtu - 18);
                ovn_lflow_add(lflows, od, S_ROUTER_IN_LARGER_PKTS, 50,
                              ds_cstr(&match), ds_cstr(&actions));
            }
        }
    }

    /* Logical router ingress table 12: Gateway redirect.
     *
     * For traffic with outport equal to the l3dgw_port
     * on a distributed router, this table redirects a subset
     * of the traffic to the l3redirect_port which represents
     * the central instance of the l3dgw_port.
     */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }
        if (od->l3dgw_port && od->l3redirect_port) {
            ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 300,
                          REGBIT_DISTRIBUTED_NAT" == 1", "next;");

            /* For traffic with outport == l3dgw_port, if the
             * packet did not match any higher priority redirect
             * rule, then the traffic is redirected to the central
             * instance of the l3dgw_port. */
            ds_clear(&match);
            ds_put_format(&match, "outport == %s",
                          od->l3dgw_port->json_key);
            ds_clear(&actions);
            ds_put_format(&actions, "outport = %s; next;",
                          od->l3redirect_port->json_key);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 50,
                          ds_cstr(&match), ds_cstr(&actions));

            /* If the Ethernet destination has not been resolved,
             * redirect to the central instance of the l3dgw_port.
             * Such traffic will be replaced by an ARP request or ND
             * Neighbor Solicitation in the ARP request ingress
             * table, before being redirected to the central instance.
             */
            ds_put_format(&match, " && eth.dst == 00:00:00:00:00:00");
            ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 150,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        /* Packets are allowed by default. */
        ovn_lflow_add(lflows, od, S_ROUTER_IN_GW_REDIRECT, 0, "1", "next;");
    }

    /* Local router ingress table 13: ARP request.
     *
     * In the common case where the Ethernet destination has been resolved,
     * this table outputs the packet (priority 0).  Otherwise, it composes
     * and sends an ARP/IPv6 NA request (priority 100). */
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbr) {
            continue;
        }

        for (int i = 0; i < od->nbr->n_static_routes; i++) {
            const struct nbrec_logical_router_static_route *route;

            route = od->nbr->static_routes[i];
            struct in6_addr gw_ip6;
            unsigned int plen;
            char *error = ipv6_parse_cidr(route->nexthop, &gw_ip6, &plen);
            if (error || plen != 128) {
                free(error);
                continue;
            }

            ds_clear(&match);
            ds_put_format(&match, "eth.dst == 00:00:00:00:00:00 && "
                          "ip6 && xxreg0 == %s", route->nexthop);
            struct in6_addr sn_addr;
            struct eth_addr eth_dst;
            in6_addr_solicited_node(&sn_addr, &gw_ip6);
            ipv6_multicast_to_ethernet(&eth_dst, &sn_addr);

            char sn_addr_s[INET6_ADDRSTRLEN + 1];
            ipv6_string_mapped(sn_addr_s, &sn_addr);

            ds_clear(&actions);
            ds_put_format(&actions,
                          "nd_ns { "
                          "eth.dst = "ETH_ADDR_FMT"; "
                          "ip6.dst = %s; "
                          "nd.target = %s; "
                          "output; "
                          "};", ETH_ADDR_ARGS(eth_dst), sn_addr_s,
                          route->nexthop);
            ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_REQUEST, 200,
                          ds_cstr(&match), ds_cstr(&actions));
        }

        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_REQUEST, 100,
                      "eth.dst == 00:00:00:00:00:00",
                      "arp { "
                      "eth.dst = ff:ff:ff:ff:ff:ff; "
                      "arp.spa = reg1; "
                      "arp.tpa = reg0; "
                      "arp.op = 1; " /* ARP request */
                      "output; "
                      "};");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_REQUEST, 100,
                      "eth.dst == 00:00:00:00:00:00",
                      "nd_ns { "
                      "nd.target = xxreg0; "
                      "output; "
                      "};");
        ovn_lflow_add(lflows, od, S_ROUTER_IN_ARP_REQUEST, 0, "1", "output;");
    }

    /* Logical router egress table 1: Delivery (priority 100-110).
     *
     * Priority 100 rules deliver packets to enabled logical ports.
     * Priority 110 rules match multicast packets and update the source
     * mac before delivering to enabled logical ports. IP multicast traffic
     * bypasses S_ROUTER_IN_IP_ROUTING route lookups.
     */
    HMAP_FOR_EACH (op, key_node, ports) {
        if (!op->nbrp) {
            continue;
        }

        if (!lrport_is_enabled(op->nbrp)) {
            /* Drop packets to disabled logical ports (since logical flow
             * tables are default-drop). */
            continue;
        }

        if (op->derived) {
            /* No egress packets should be processed in the context of
             * a chassisredirect port.  The chassisredirect port should
             * be replaced by the l3dgw port in the local output
             * pipeline stage before egress processing. */
            continue;
        }

        /* If multicast relay is enabled then also adjust source mac for IP
         * multicast traffic.
         */
        if (op->od->mcast_info.rtr.relay) {
            ds_clear(&match);
            ds_clear(&actions);
            ds_put_format(&match, "ip4.mcast && outport == %s",
                          op->json_key);
            ds_put_format(&actions, "eth.src = %s; output;",
                          op->lrp_networks.ea_s);
            ovn_lflow_add(lflows, op->od, S_ROUTER_OUT_DELIVERY, 110,
                        ds_cstr(&match), ds_cstr(&actions));
        }

        ds_clear(&match);
        ds_put_format(&match, "outport == %s", op->json_key);
        ovn_lflow_add(lflows, op->od, S_ROUTER_OUT_DELIVERY, 100,
                      ds_cstr(&match), "output;");
    }

    ds_destroy(&match);
    ds_destroy(&actions);
}

/* Updates the Logical_Flow and Multicast_Group tables in the OVN_SB database,
 * constructing their contents based on the OVN_NB database. */
static void
build_lflows(struct northd_context *ctx, struct hmap *datapaths,
             struct hmap *ports, struct hmap *port_groups,
             struct hmap *mcgroups, struct hmap *igmp_groups,
             struct shash *meter_groups)
{
    struct hmap lflows = HMAP_INITIALIZER(&lflows);

    build_lswitch_flows(datapaths, ports, port_groups, &lflows, mcgroups,
                        igmp_groups, meter_groups);
    build_lrouter_flows(datapaths, ports, &lflows, meter_groups);

    /* Push changes to the Logical_Flow table to database. */
    const struct sbrec_logical_flow *sbflow, *next_sbflow;
    SBREC_LOGICAL_FLOW_FOR_EACH_SAFE (sbflow, next_sbflow, ctx->ovnsb_idl) {
        struct ovn_datapath *od
            = ovn_datapath_from_sbrec(datapaths, sbflow->logical_datapath);
        if (!od) {
            sbrec_logical_flow_delete(sbflow);
            continue;
        }

        enum ovn_datapath_type dp_type = od->nbs ? DP_SWITCH : DP_ROUTER;
        enum ovn_pipeline pipeline
            = !strcmp(sbflow->pipeline, "ingress") ? P_IN : P_OUT;
        struct ovn_lflow *lflow = ovn_lflow_find(
            &lflows, od, ovn_stage_build(dp_type, pipeline, sbflow->table_id),
            sbflow->priority, sbflow->match, sbflow->actions, sbflow->hash);
        if (lflow) {
            ovn_lflow_destroy(&lflows, lflow);
        } else {
            sbrec_logical_flow_delete(sbflow);
        }
    }
    struct ovn_lflow *lflow, *next_lflow;
    HMAP_FOR_EACH_SAFE (lflow, next_lflow, hmap_node, &lflows) {
        const char *pipeline = ovn_stage_get_pipeline_name(lflow->stage);
        uint8_t table = ovn_stage_get_table(lflow->stage);

        sbflow = sbrec_logical_flow_insert(ctx->ovnsb_txn);
        sbrec_logical_flow_set_logical_datapath(sbflow, lflow->od->sb);
        sbrec_logical_flow_set_pipeline(sbflow, pipeline);
        sbrec_logical_flow_set_table_id(sbflow, table);
        sbrec_logical_flow_set_priority(sbflow, lflow->priority);
        sbrec_logical_flow_set_match(sbflow, lflow->match);
        sbrec_logical_flow_set_actions(sbflow, lflow->actions);

        /* Trim the source locator lflow->where, which looks something like
         * "ovn/northd/ovn-northd.c:1234", down to just the part following the
         * last slash, e.g. "ovn-northd.c:1234". */
        const char *slash = strrchr(lflow->where, '/');
#if _WIN32
        const char *backslash = strrchr(lflow->where, '\\');
        if (!slash || backslash > slash) {
            slash = backslash;
        }
#endif
        const char *where = slash ? slash + 1 : lflow->where;

        struct smap ids = SMAP_INITIALIZER(&ids);
        smap_add(&ids, "stage-name", ovn_stage_to_str(lflow->stage));
        smap_add(&ids, "source", where);
        if (lflow->stage_hint) {
            smap_add(&ids, "stage-hint", lflow->stage_hint);
        }
        sbrec_logical_flow_set_external_ids(sbflow, &ids);
        smap_destroy(&ids);

        ovn_lflow_destroy(&lflows, lflow);
    }
    hmap_destroy(&lflows);

    /* Push changes to the Multicast_Group table to database. */
    const struct sbrec_multicast_group *sbmc, *next_sbmc;
    SBREC_MULTICAST_GROUP_FOR_EACH_SAFE (sbmc, next_sbmc, ctx->ovnsb_idl) {
        struct ovn_datapath *od = ovn_datapath_from_sbrec(datapaths,
                                                          sbmc->datapath);
        if (!od) {
            sbrec_multicast_group_delete(sbmc);
            continue;
        }

        struct multicast_group group = { .name = sbmc->name,
                                         .key = sbmc->tunnel_key };
        struct ovn_multicast *mc = ovn_multicast_find(mcgroups, od, &group);
        if (mc) {
            ovn_multicast_update_sbrec(mc, sbmc);
            ovn_multicast_destroy(mcgroups, mc);
        } else {
            sbrec_multicast_group_delete(sbmc);
        }
    }
    struct ovn_multicast *mc, *next_mc;
    HMAP_FOR_EACH_SAFE (mc, next_mc, hmap_node, mcgroups) {
        if (!mc->datapath) {
            ovn_multicast_destroy(mcgroups, mc);
            continue;
        }
        sbmc = sbrec_multicast_group_insert(ctx->ovnsb_txn);
        sbrec_multicast_group_set_datapath(sbmc, mc->datapath->sb);
        sbrec_multicast_group_set_name(sbmc, mc->group->name);
        sbrec_multicast_group_set_tunnel_key(sbmc, mc->group->key);
        ovn_multicast_update_sbrec(mc, sbmc);
        ovn_multicast_destroy(mcgroups, mc);
    }
}

static void
sync_address_set(struct northd_context *ctx, const char *name,
                 const char **addrs, size_t n_addrs,
                 struct shash *sb_address_sets)
{
    const struct sbrec_address_set *sb_address_set;
    sb_address_set = shash_find_and_delete(sb_address_sets,
                                           name);
    if (!sb_address_set) {
        sb_address_set = sbrec_address_set_insert(ctx->ovnsb_txn);
        sbrec_address_set_set_name(sb_address_set, name);
    }

    sbrec_address_set_set_addresses(sb_address_set,
                                    addrs, n_addrs);
}

/* Go through 'addresses' and add found IPv4 addresses to 'ipv4_addrs' and IPv6
 * addresses to 'ipv6_addrs'.
 */
static void
split_addresses(const char *addresses, struct svec *ipv4_addrs,
                struct svec *ipv6_addrs)
{
    struct lport_addresses laddrs;
    extract_lsp_addresses(addresses, &laddrs);
    for (size_t k = 0; k < laddrs.n_ipv4_addrs; k++) {
        svec_add(ipv4_addrs, laddrs.ipv4_addrs[k].addr_s);
    }
    for (size_t k = 0; k < laddrs.n_ipv6_addrs; k++) {
        svec_add(ipv6_addrs, laddrs.ipv6_addrs[k].addr_s);
    }
    destroy_lport_addresses(&laddrs);
}

/* OVN_Southbound Address_Set table contains same records as in north
 * bound, plus the records generated from Port_Group table in north bound.
 *
 * There are 2 records generated from each port group, one for IPv4, and
 * one for IPv6, named in the format: <port group name>_ip4 and
 * <port group name>_ip6 respectively. MAC addresses are ignored.
 *
 * We always update OVN_Southbound to match the Address_Set and Port_Group
 * in OVN_Northbound, so that the address sets used in Logical_Flows in
 * OVN_Southbound is checked against the proper set.*/
static void
sync_address_sets(struct northd_context *ctx)
{
    struct shash sb_address_sets = SHASH_INITIALIZER(&sb_address_sets);

    const struct sbrec_address_set *sb_address_set;
    SBREC_ADDRESS_SET_FOR_EACH (sb_address_set, ctx->ovnsb_idl) {
        shash_add(&sb_address_sets, sb_address_set->name, sb_address_set);
    }

    /* sync port group generated address sets first */
    const struct nbrec_port_group *nb_port_group;
    NBREC_PORT_GROUP_FOR_EACH (nb_port_group, ctx->ovnnb_idl) {
        struct svec ipv4_addrs = SVEC_EMPTY_INITIALIZER;
        struct svec ipv6_addrs = SVEC_EMPTY_INITIALIZER;
        for (size_t i = 0; i < nb_port_group->n_ports; i++) {
            for (size_t j = 0; j < nb_port_group->ports[i]->n_addresses; j++) {
                const char *addrs = nb_port_group->ports[i]->addresses[j];
                if (!is_dynamic_lsp_address(addrs)) {
                    split_addresses(addrs, &ipv4_addrs, &ipv6_addrs);
                }
            }
            if (nb_port_group->ports[i]->dynamic_addresses) {
                split_addresses(nb_port_group->ports[i]->dynamic_addresses,
                                &ipv4_addrs, &ipv6_addrs);
            }
        }
        char *ipv4_addrs_name = xasprintf("%s_ip4", nb_port_group->name);
        char *ipv6_addrs_name = xasprintf("%s_ip6", nb_port_group->name);
        sync_address_set(ctx, ipv4_addrs_name,
                         /* "char **" is not compatible with "const char **" */
                         (const char **)ipv4_addrs.names,
                         ipv4_addrs.n, &sb_address_sets);
        sync_address_set(ctx, ipv6_addrs_name,
                         /* "char **" is not compatible with "const char **" */
                         (const char **)ipv6_addrs.names,
                         ipv6_addrs.n, &sb_address_sets);
        free(ipv4_addrs_name);
        free(ipv6_addrs_name);
        svec_destroy(&ipv4_addrs);
        svec_destroy(&ipv6_addrs);
    }

    /* sync user defined address sets, which may overwrite port group
     * generated address sets if same name is used */
    const struct nbrec_address_set *nb_address_set;
    NBREC_ADDRESS_SET_FOR_EACH (nb_address_set, ctx->ovnnb_idl) {
        sync_address_set(ctx, nb_address_set->name,
            /* "char **" is not compatible with "const char **" */
            (const char **)nb_address_set->addresses,
            nb_address_set->n_addresses, &sb_address_sets);
    }

    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, &sb_address_sets) {
        sbrec_address_set_delete(node->data);
        shash_delete(&sb_address_sets, node);
    }
    shash_destroy(&sb_address_sets);
}

/* Each port group in Port_Group table in OVN_Northbound has a corresponding
 * entry in Port_Group table in OVN_Southbound. In OVN_Northbound the entries
 * contains lport uuids, while in OVN_Southbound we store the lport names.
 */
static void
sync_port_groups(struct northd_context *ctx)
{
    struct shash sb_port_groups = SHASH_INITIALIZER(&sb_port_groups);

    const struct sbrec_port_group *sb_port_group;
    SBREC_PORT_GROUP_FOR_EACH (sb_port_group, ctx->ovnsb_idl) {
        shash_add(&sb_port_groups, sb_port_group->name, sb_port_group);
    }

    const struct nbrec_port_group *nb_port_group;
    NBREC_PORT_GROUP_FOR_EACH (nb_port_group, ctx->ovnnb_idl) {
        sb_port_group = shash_find_and_delete(&sb_port_groups,
                                               nb_port_group->name);
        if (!sb_port_group) {
            sb_port_group = sbrec_port_group_insert(ctx->ovnsb_txn);
            sbrec_port_group_set_name(sb_port_group, nb_port_group->name);
        }

        const char **nb_port_names = xcalloc(nb_port_group->n_ports,
                                             sizeof *nb_port_names);
        int i;
        for (i = 0; i < nb_port_group->n_ports; i++) {
            nb_port_names[i] = nb_port_group->ports[i]->name;
        }
        sbrec_port_group_set_ports(sb_port_group,
                                   nb_port_names,
                                   nb_port_group->n_ports);
        free(nb_port_names);
    }

    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, &sb_port_groups) {
        sbrec_port_group_delete(node->data);
        shash_delete(&sb_port_groups, node);
    }
    shash_destroy(&sb_port_groups);
}

struct band_entry {
    int64_t rate;
    int64_t burst_size;
    const char *action;
};

static int
band_cmp(const void *band1_, const void *band2_)
{
    const struct band_entry *band1p = band1_;
    const struct band_entry *band2p = band2_;

    if (band1p->rate != band2p->rate) {
        return band1p->rate > band2p->rate ? -1 : 1;
    } else if (band1p->burst_size != band2p->burst_size) {
        return band1p->burst_size > band2p->burst_size ? -1 : 1;
    } else {
        return strcmp(band1p->action, band2p->action);
    }
}

static bool
bands_need_update(const struct nbrec_meter *nb_meter,
                  const struct sbrec_meter *sb_meter)
{
    if (nb_meter->n_bands != sb_meter->n_bands) {
        return true;
    }

    /* A single band is the most common scenario, so speed up that
     * check. */
    if (nb_meter->n_bands == 1) {
        struct nbrec_meter_band *nb_band = nb_meter->bands[0];
        struct sbrec_meter_band *sb_band = sb_meter->bands[0];

        return !(nb_band->rate == sb_band->rate
                 && nb_band->burst_size == sb_band->burst_size
                 && !strcmp(sb_band->action, nb_band->action));
    }

    /* Place the Northbound entries in sorted order. */
    struct band_entry *nb_bands;
    nb_bands = xmalloc(sizeof *nb_bands * nb_meter->n_bands);
    for (size_t i = 0; i < nb_meter->n_bands; i++) {
        struct nbrec_meter_band *nb_band = nb_meter->bands[i];

        nb_bands[i].rate = nb_band->rate;
        nb_bands[i].burst_size = nb_band->burst_size;
        nb_bands[i].action = nb_band->action;
    }
    qsort(nb_bands, nb_meter->n_bands, sizeof *nb_bands, band_cmp);

    /* Place the Southbound entries in sorted order. */
    struct band_entry *sb_bands;
    sb_bands = xmalloc(sizeof *sb_bands * sb_meter->n_bands);
    for (size_t i = 0; i < sb_meter->n_bands; i++) {
        struct sbrec_meter_band *sb_band = sb_meter->bands[i];

        sb_bands[i].rate = sb_band->rate;
        sb_bands[i].burst_size = sb_band->burst_size;
        sb_bands[i].action = sb_band->action;
    }
    qsort(sb_bands, sb_meter->n_bands, sizeof *sb_bands, band_cmp);

    bool need_update = false;
    for (size_t i = 0; i < nb_meter->n_bands; i++) {
        if (nb_bands[i].rate != sb_bands[i].rate
            || nb_bands[i].burst_size != sb_bands[i].burst_size
            || strcmp(nb_bands[i].action, sb_bands[i].action)) {
            need_update = true;
            goto done;
        }
    }

done:
    free(nb_bands);
    free(sb_bands);

    return need_update;
}

/* Each entry in the Meter and Meter_Band tables in OVN_Northbound have
 * a corresponding entries in the Meter and Meter_Band tables in
 * OVN_Southbound.
 */
static void
sync_meters(struct northd_context *ctx)
{
    struct shash sb_meters = SHASH_INITIALIZER(&sb_meters);

    const struct sbrec_meter *sb_meter;
    SBREC_METER_FOR_EACH (sb_meter, ctx->ovnsb_idl) {
        shash_add(&sb_meters, sb_meter->name, sb_meter);
    }

    const struct nbrec_meter *nb_meter;
    NBREC_METER_FOR_EACH (nb_meter, ctx->ovnnb_idl) {
        bool new_sb_meter = false;

        sb_meter = shash_find_and_delete(&sb_meters, nb_meter->name);
        if (!sb_meter) {
            sb_meter = sbrec_meter_insert(ctx->ovnsb_txn);
            sbrec_meter_set_name(sb_meter, nb_meter->name);
            new_sb_meter = true;
        }

        if (new_sb_meter || bands_need_update(nb_meter, sb_meter)) {
            struct sbrec_meter_band **sb_bands;
            sb_bands = xcalloc(nb_meter->n_bands, sizeof *sb_bands);
            for (size_t i = 0; i < nb_meter->n_bands; i++) {
                const struct nbrec_meter_band *nb_band = nb_meter->bands[i];

                sb_bands[i] = sbrec_meter_band_insert(ctx->ovnsb_txn);

                sbrec_meter_band_set_action(sb_bands[i], nb_band->action);
                sbrec_meter_band_set_rate(sb_bands[i], nb_band->rate);
                sbrec_meter_band_set_burst_size(sb_bands[i],
                                                nb_band->burst_size);
            }
            sbrec_meter_set_bands(sb_meter, sb_bands, nb_meter->n_bands);
            free(sb_bands);
        }

        sbrec_meter_set_unit(sb_meter, nb_meter->unit);
    }

    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, &sb_meters) {
        sbrec_meter_delete(node->data);
        shash_delete(&sb_meters, node);
    }
    shash_destroy(&sb_meters);
}

/*
 * struct 'dns_info' is used to sync the DNS records between OVN Northbound db
 * and Southbound db.
 */
struct dns_info {
    struct hmap_node hmap_node;
    const struct nbrec_dns *nb_dns; /* DNS record in the Northbound db. */
    const struct sbrec_dns *sb_dns; /* DNS record in the Soutbound db. */

    /* Datapaths to which the DNS entry is associated with it. */
    const struct sbrec_datapath_binding **sbs;
    size_t n_sbs;
};

static inline struct dns_info *
get_dns_info_from_hmap(struct hmap *dns_map, struct uuid *uuid)
{
    struct dns_info *dns_info;
    size_t hash = uuid_hash(uuid);
    HMAP_FOR_EACH_WITH_HASH (dns_info, hmap_node, hash, dns_map) {
        if (uuid_equals(&dns_info->nb_dns->header_.uuid, uuid)) {
            return dns_info;
        }
    }

    return NULL;
}

static void
sync_dns_entries(struct northd_context *ctx, struct hmap *datapaths)
{
    struct hmap dns_map = HMAP_INITIALIZER(&dns_map);
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs || !od->nbs->n_dns_records) {
            continue;
        }

        for (size_t i = 0; i < od->nbs->n_dns_records; i++) {
            struct dns_info *dns_info = get_dns_info_from_hmap(
                &dns_map, &od->nbs->dns_records[i]->header_.uuid);
            if (!dns_info) {
                size_t hash = uuid_hash(
                    &od->nbs->dns_records[i]->header_.uuid);
                dns_info = xzalloc(sizeof *dns_info);;
                dns_info->nb_dns = od->nbs->dns_records[i];
                hmap_insert(&dns_map, &dns_info->hmap_node, hash);
            }

            dns_info->n_sbs++;
            dns_info->sbs = xrealloc(dns_info->sbs,
                                     dns_info->n_sbs * sizeof *dns_info->sbs);
            dns_info->sbs[dns_info->n_sbs - 1] = od->sb;
        }
    }

    const struct sbrec_dns *sbrec_dns, *next;
    SBREC_DNS_FOR_EACH_SAFE (sbrec_dns, next, ctx->ovnsb_idl) {
        const char *nb_dns_uuid = smap_get(&sbrec_dns->external_ids, "dns_id");
        struct uuid dns_uuid;
        if (!nb_dns_uuid || !uuid_from_string(&dns_uuid, nb_dns_uuid)) {
            sbrec_dns_delete(sbrec_dns);
            continue;
        }

        struct dns_info *dns_info =
            get_dns_info_from_hmap(&dns_map, &dns_uuid);
        if (dns_info) {
            dns_info->sb_dns = sbrec_dns;
        } else {
            sbrec_dns_delete(sbrec_dns);
        }
    }

    struct dns_info *dns_info;
    HMAP_FOR_EACH_POP (dns_info, hmap_node, &dns_map) {
        if (!dns_info->sb_dns) {
            sbrec_dns = sbrec_dns_insert(ctx->ovnsb_txn);
            dns_info->sb_dns = sbrec_dns;
            char *dns_id = xasprintf(
                UUID_FMT, UUID_ARGS(&dns_info->nb_dns->header_.uuid));
            const struct smap external_ids =
                SMAP_CONST1(&external_ids, "dns_id", dns_id);
            sbrec_dns_set_external_ids(sbrec_dns, &external_ids);
            free(dns_id);
        }

        /* Set the datapaths and records. If nothing has changed, then
         * this will be a no-op.
         */
        sbrec_dns_set_datapaths(
            dns_info->sb_dns,
            (struct sbrec_datapath_binding **)dns_info->sbs,
            dns_info->n_sbs);
        sbrec_dns_set_records(dns_info->sb_dns, &dns_info->nb_dns->records);
        free(dns_info->sbs);
        free(dns_info);
    }
    hmap_destroy(&dns_map);
}

static void
destroy_datapaths_and_ports(struct hmap *datapaths, struct hmap *ports,
                            struct ovs_list *lr_list)
{
    struct ovn_datapath *router_dp;
    LIST_FOR_EACH_POP (router_dp, lr_list, lr_list) {
        if (router_dp->lr_group) {
            struct lrouter_group *lr_group = router_dp->lr_group;

            for (size_t i = 0; i < lr_group->n_router_dps; i++) {
                lr_group->router_dps[i]->lr_group = NULL;
            }

            free(lr_group->router_dps);
            sset_destroy(&lr_group->ha_chassis_groups);
            free(lr_group);
        }
    }

    struct ovn_datapath *dp, *next_dp;
    HMAP_FOR_EACH_SAFE (dp, next_dp, key_node, datapaths) {
        ovn_datapath_destroy(datapaths, dp);
    }
    hmap_destroy(datapaths);

    struct ovn_port *port, *next_port;
    HMAP_FOR_EACH_SAFE (port, next_port, key_node, ports) {
        ovn_port_destroy(ports, port);
    }
    hmap_destroy(ports);
}

static void
build_ip_mcast(struct northd_context *ctx, struct hmap *datapaths)
{
    struct ovn_datapath *od;

    HMAP_FOR_EACH (od, key_node, datapaths) {
        if (!od->nbs) {
            continue;
        }

        const struct sbrec_ip_multicast *ip_mcast =
            ip_mcast_lookup(ctx->sbrec_ip_mcast_by_dp, od->sb);

        if (!ip_mcast) {
            ip_mcast = sbrec_ip_multicast_insert(ctx->ovnsb_txn);
        }
        store_mcast_info_for_switch_datapath(ip_mcast, od);
    }

    /* Delete southbound records without northbound matches. */
    const struct sbrec_ip_multicast *sb, *sb_next;

    SBREC_IP_MULTICAST_FOR_EACH_SAFE (sb, sb_next, ctx->ovnsb_idl) {
        if (!sb->datapath ||
                !ovn_datapath_from_sbrec(datapaths, sb->datapath)) {
            sbrec_ip_multicast_delete(sb);
        }
    }
}

static void
build_mcast_groups(struct northd_context *ctx,
                   struct hmap *datapaths, struct hmap *ports,
                   struct hmap *mcast_groups,
                   struct hmap *igmp_groups)
{
    struct ovn_port *op;

    hmap_init(mcast_groups);
    hmap_init(igmp_groups);

    HMAP_FOR_EACH (op, key_node, ports) {
        if (op->nbrp && lrport_is_enabled(op->nbrp)) {
            /* If this port is configured to always flood multicast traffic
             * add it to the MC_STATIC group.
             */
            if (op->mcast_info.flood) {
                ovn_multicast_add(mcast_groups, &mc_static, op);
                op->od->mcast_info.rtr.flood_static = true;
            }
        } else if (op->nbsp && lsp_is_enabled(op->nbsp)) {
            ovn_multicast_add(mcast_groups, &mc_flood, op);

            /* If this port is connected to a multicast router then add it
             * to the MC_MROUTER_FLOOD group.
             */
            if (op->od->mcast_info.sw.flood_relay && op->peer &&
                    op->peer->od && op->peer->od->mcast_info.rtr.relay) {
                ovn_multicast_add(mcast_groups, &mc_mrouter_flood, op);
            }

            /* If this port is configured to always flood multicast reports
             * add it to the MC_MROUTER_STATIC group.
             */
            if (op->mcast_info.flood_reports) {
                ovn_multicast_add(mcast_groups, &mc_mrouter_static, op);
                op->od->mcast_info.sw.flood_reports = true;
            }

            /* If this port is configured to always flood multicast traffic
             * add it to the MC_STATIC group.
             */
            if (op->mcast_info.flood) {
                ovn_multicast_add(mcast_groups, &mc_static, op);
                op->od->mcast_info.sw.flood_static = true;
            }
        }
    }

    const struct sbrec_igmp_group *sb_igmp, *sb_igmp_next;

    SBREC_IGMP_GROUP_FOR_EACH_SAFE (sb_igmp, sb_igmp_next, ctx->ovnsb_idl) {
        /* If this is a stale group (e.g., controller had crashed,
         * purge it).
         */
        if (!sb_igmp->chassis || !sb_igmp->datapath) {
            sbrec_igmp_group_delete(sb_igmp);
            continue;
        }

        /* If the datapath value is stale, purge the group. */
        struct ovn_datapath *od =
            ovn_datapath_from_sbrec(datapaths, sb_igmp->datapath);
        if (!od) {
            sbrec_igmp_group_delete(sb_igmp);
            continue;
        }

        struct in6_addr group_address;
        if (!ovn_igmp_group_get_address(sb_igmp, &group_address)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "invalid IGMP group address: %s",
                         sb_igmp->address);
            continue;
        }

        /* Add the IGMP group entry. Will also try to allocate an ID for it
         * if the multicast group already exists.
         */
        struct ovn_igmp_group *igmp_group =
            ovn_igmp_group_add(ctx, igmp_groups, od, &group_address,
                               sb_igmp->address);

        /* Extract the IGMP group ports from the SB entry and store them
         * in the IGMP group.
         */
        size_t n_igmp_ports;
        struct ovn_port **igmp_ports =
            ovn_igmp_group_get_ports(sb_igmp, &n_igmp_ports, ports);
        ovn_igmp_group_add_entry(igmp_group, igmp_ports, n_igmp_ports);
    }

    /* Build IGMP groups for multicast routers with relay enabled. The router
     * IGMP groups are based on the groups learnt by their multicast enabled
     * peers.
     */
    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, datapaths) {

        if (ovs_list_is_empty(&od->mcast_info.groups)) {
            continue;
        }

        for (size_t i = 0; i < od->n_router_ports; i++) {
            struct ovn_port *router_port = od->router_ports[i]->peer;

            /* If the router the port connects to doesn't have multicast
             * relay enabled or if it was already configured to flood
             * multicast traffic then skip it.
             */
            if (!router_port || !router_port->od ||
                    !router_port->od->mcast_info.rtr.relay ||
                    router_port->mcast_info.flood) {
                continue;
            }

            struct ovn_igmp_group *igmp_group;
            LIST_FOR_EACH (igmp_group, list_node, &od->mcast_info.groups) {
                struct ovn_igmp_group *igmp_group_rtr =
                    ovn_igmp_group_add(ctx, igmp_groups, router_port->od,
                                       &igmp_group->address,
                                       igmp_group->mcgroup.name);
                struct ovn_port **router_igmp_ports =
                    xmalloc(sizeof *router_igmp_ports);
                router_igmp_ports[0] = router_port;
                ovn_igmp_group_add_entry(igmp_group_rtr, router_igmp_ports, 1);
            }
        }
    }

    /* Walk the aggregated IGMP groups and allocate IDs for new entries.
     * Then store the ports in the associated multicast group.
     */
    struct ovn_igmp_group *igmp_group, *igmp_group_next;
    HMAP_FOR_EACH_SAFE (igmp_group, igmp_group_next, hmap_node, igmp_groups) {

        if (!ovn_igmp_group_allocate_id(igmp_group)) {
            /* If we ran out of keys just destroy the entry. */
            ovn_igmp_group_destroy(igmp_groups, igmp_group);
            continue;
        }

        /* Aggregate the ports from all entries corresponding to this
         * group.
         */
        ovn_igmp_group_aggregate_ports(igmp_group, mcast_groups);
    }
}

static void
build_meter_groups(struct northd_context *ctx,
                   struct shash *meter_groups)
{
    const struct nbrec_meter *nb_meter;
    NBREC_METER_FOR_EACH (nb_meter, ctx->ovnnb_idl) {
        shash_add(meter_groups, nb_meter->name, nb_meter);
    }
}

static void
ovnnb_db_run(struct northd_context *ctx,
             struct ovsdb_idl_index *sbrec_chassis_by_name,
             struct ovsdb_idl_loop *sb_loop,
             struct hmap *datapaths, struct hmap *ports,
             struct ovs_list *lr_list)
{
    if (!ctx->ovnsb_txn || !ctx->ovnnb_txn) {
        return;
    }
    struct hmap port_groups;
    struct hmap mcast_groups;
    struct hmap igmp_groups;
    struct shash meter_groups = SHASH_INITIALIZER(&meter_groups);

    build_datapaths(ctx, datapaths, lr_list);
    build_ports(ctx, sbrec_chassis_by_name, datapaths, ports);
    build_ipam(datapaths, ports);
    build_port_group_lswitches(ctx, &port_groups, ports);
    build_lrouter_groups(ports, lr_list);
    build_ip_mcast(ctx, datapaths);
    build_mcast_groups(ctx, datapaths, ports, &mcast_groups, &igmp_groups);
    build_meter_groups(ctx, &meter_groups);
    build_lflows(ctx, datapaths, ports, &port_groups, &mcast_groups,
                 &igmp_groups, &meter_groups);

    sync_address_sets(ctx);
    sync_port_groups(ctx);
    sync_meters(ctx);
    sync_dns_entries(ctx, datapaths);

    struct ovn_igmp_group *igmp_group, *next_igmp_group;

    HMAP_FOR_EACH_SAFE (igmp_group, next_igmp_group, hmap_node, &igmp_groups) {
        ovn_igmp_group_destroy(&igmp_groups, igmp_group);
    }

    struct ovn_port_group *pg, *next_pg;
    HMAP_FOR_EACH_SAFE (pg, next_pg, key_node, &port_groups) {
        ovn_port_group_destroy(&port_groups, pg);
    }
    hmap_destroy(&igmp_groups);
    hmap_destroy(&mcast_groups);
    hmap_destroy(&port_groups);

    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, &meter_groups) {
        shash_delete(&meter_groups, node);
    }
    shash_destroy(&meter_groups);

    /* Sync ipsec configuration.
     * Copy nb_cfg from northbound to southbound database.
     * Also set up to update sb_cfg once our southbound transaction commits. */
    const struct nbrec_nb_global *nb = nbrec_nb_global_first(ctx->ovnnb_idl);
    if (!nb) {
        nb = nbrec_nb_global_insert(ctx->ovnnb_txn);
    }
    const struct sbrec_sb_global *sb = sbrec_sb_global_first(ctx->ovnsb_idl);
    if (!sb) {
        sb = sbrec_sb_global_insert(ctx->ovnsb_txn);
    }
    if (nb->ipsec != sb->ipsec) {
        sbrec_sb_global_set_ipsec(sb, nb->ipsec);
    }
    sbrec_sb_global_set_nb_cfg(sb, nb->nb_cfg);
    sbrec_sb_global_set_options(sb, &nb->options);
    sb_loop->next_cfg = nb->nb_cfg;

    const char *mac_addr_prefix = smap_get(&nb->options, "mac_prefix");
    if (mac_addr_prefix) {
        struct eth_addr addr;

        memset(&addr, 0, sizeof addr);
        if (ovs_scan(mac_addr_prefix, "%"SCNx8":%"SCNx8":%"SCNx8,
                     &addr.ea[0], &addr.ea[1], &addr.ea[2])) {
            mac_prefix = addr;
        }
    } else {
        struct smap options;

        smap_clone(&options, &nb->options);
        eth_addr_random(&mac_prefix);
        memset(&mac_prefix.ea[3], 0, 3);

        smap_add_format(&options, "mac_prefix",
                        "%02"PRIx8":%02"PRIx8":%02"PRIx8,
                        mac_prefix.ea[0], mac_prefix.ea[1], mac_prefix.ea[2]);
        nbrec_nb_global_verify_options(nb);
        nbrec_nb_global_set_options(nb, &options);

        smap_destroy(&options);
    }

    controller_event_en = smap_get_bool(&nb->options,
                                        "controller_event", false);

    cleanup_macam(&macam);
}

/* Stores the list of chassis which references an ha_chassis_group.
 */
struct ha_ref_chassis_info {
    const struct sbrec_ha_chassis_group *ha_chassis_group;
    struct sbrec_chassis **ref_chassis;
    size_t n_ref_chassis;
    size_t free_slots;
};

static void
add_to_ha_ref_chassis_info(struct ha_ref_chassis_info *ref_ch_info,
                           const struct sbrec_chassis *chassis)
{
    for (size_t j = 0; j < ref_ch_info->n_ref_chassis; j++) {
        if (ref_ch_info->ref_chassis[j] == chassis) {
           return;
        }
    }

    /* Allocate space for 3 chassis at a time. */
    if (!ref_ch_info->free_slots) {
        ref_ch_info->ref_chassis =
            xrealloc(ref_ch_info->ref_chassis,
                     sizeof *ref_ch_info->ref_chassis *
                     (ref_ch_info->n_ref_chassis + 3));
        ref_ch_info->free_slots = 3;
    }

    ref_ch_info->ref_chassis[ref_ch_info->n_ref_chassis] =
        CONST_CAST(struct sbrec_chassis *, chassis);
    ref_ch_info->n_ref_chassis++;
    ref_ch_info->free_slots--;
}

static void
update_sb_ha_group_ref_chassis(struct shash *ha_ref_chassis_map)
{
    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, ha_ref_chassis_map) {
        struct ha_ref_chassis_info *ha_ref_info = node->data;
        sbrec_ha_chassis_group_set_ref_chassis(ha_ref_info->ha_chassis_group,
                                               ha_ref_info->ref_chassis,
                                               ha_ref_info->n_ref_chassis);
        free(ha_ref_info->ref_chassis);
        free(ha_ref_info);
        shash_delete(ha_ref_chassis_map, node);
    }
}

/* This function checks if the port binding 'sb' references
 * a HA chassis group.
 * Eg. Suppose a distributed logical router port - lr0-public
 * uses an HA chassis group - hagrp1 and if hagrp1 has 3 ha
 * chassis - gw1, gw2 and gw3.
 * Or
 * If the distributed logical router port - lr0-public has
 * 3 gateway chassis - gw1, gw2 and gw3.
 * ovn-northd creates ha chassis group - hagrp1 in SB DB
 * and adds gw1, gw2 and gw3 to its ha_chassis list.
 *
 * If port binding 'sb' represents a logical switch port 'p1'
 * and its logical switch is connected to the logical router
 * 'lr0' directly or indirectly (i.e p1's logical switch is
 *  connected to a router 'lr1' and 'lr1' has a path to lr0 via
 *  transit logical switches) and 'sb' is claimed by chassis - 'c1' then
 * this function adds c1 to the list of the reference chassis
 *  - 'ref_chassis' of hagrp1.
 */
static void
build_ha_chassis_group_ref_chassis(struct northd_context *ctx,
                                   const struct sbrec_port_binding *sb,
                                   struct ovn_port *op,
                                   struct shash *ha_ref_chassis_map)
{
    struct lrouter_group *lr_group = NULL;
    for (size_t i = 0; i < op->od->n_router_ports; i++) {
        if (!op->od->router_ports[i]->peer) {
            continue;
        }

        lr_group = op->od->router_ports[i]->peer->od->lr_group;
        /* If a logical switch has multiple router ports, then
         * all the logical routers belong to the same logical
         * router group. */
        break;
    }

    if (!lr_group) {
        return;
    }

    const char *ha_group_name;
    SSET_FOR_EACH (ha_group_name, &lr_group->ha_chassis_groups) {
        const struct sbrec_ha_chassis_group *sb_ha_chassis_grp;
        sb_ha_chassis_grp = ha_chassis_group_lookup_by_name(
            ctx->sbrec_ha_chassis_grp_by_name, ha_group_name);

        if (sb_ha_chassis_grp) {
            struct ha_ref_chassis_info *ref_ch_info =
            shash_find_data(ha_ref_chassis_map, sb_ha_chassis_grp->name);
            ovs_assert(ref_ch_info);
            add_to_ha_ref_chassis_info(ref_ch_info, sb->chassis);
        }
    }
}

/* Handle changes to the 'chassis' column of the 'Port_Binding' table.  When
 * this column is not empty, it means we need to set the corresponding logical
 * port as 'up' in the northbound DB. */
static void
handle_port_binding_changes(struct northd_context *ctx, struct hmap *ports,
                            struct shash *ha_ref_chassis_map)
{
    const struct sbrec_port_binding *sb;
    bool build_ha_chassis_ref = false;
    if (ctx->ovnsb_txn) {
        const struct sbrec_ha_chassis_group *ha_ch_grp;
        SBREC_HA_CHASSIS_GROUP_FOR_EACH (ha_ch_grp, ctx->ovnsb_idl) {
            struct ha_ref_chassis_info *ref_ch_info =
                xzalloc(sizeof *ref_ch_info);
            ref_ch_info->ha_chassis_group = ha_ch_grp;
            build_ha_chassis_ref = true;
            shash_add(ha_ref_chassis_map, ha_ch_grp->name, ref_ch_info);
        }
    }

    SBREC_PORT_BINDING_FOR_EACH(sb, ctx->ovnsb_idl) {
        struct ovn_port *op = ovn_port_find(ports, sb->logical_port);

        if (!op || !op->nbsp) {
            /* The logical port doesn't exist for this port binding.  This can
             * happen under normal circumstances when ovn-northd hasn't gotten
             * around to pruning the Port_Binding yet. */
            continue;
        }

        bool up = (sb->chassis || !strcmp(op->nbsp->type, "router"));
        if (!op->nbsp->up || *op->nbsp->up != up) {
            nbrec_logical_switch_port_set_up(op->nbsp, &up, 1);
        }

        if (build_ha_chassis_ref && ctx->ovnsb_txn && sb->chassis) {
            /* Check and add the chassis which has claimed this 'sb'
             * to the ha chassis group's ref_chassis if required. */
            build_ha_chassis_group_ref_chassis(ctx, sb, op,
                                               ha_ref_chassis_map);
        }
    }
}

static struct gen_opts_map supported_dhcp_opts[] = {
    OFFERIP,
    DHCP_OPT_NETMASK,
    DHCP_OPT_ROUTER,
    DHCP_OPT_DNS_SERVER,
    DHCP_OPT_LOG_SERVER,
    DHCP_OPT_LPR_SERVER,
    DHCP_OPT_SWAP_SERVER,
    DHCP_OPT_POLICY_FILTER,
    DHCP_OPT_ROUTER_SOLICITATION,
    DHCP_OPT_NIS_SERVER,
    DHCP_OPT_NTP_SERVER,
    DHCP_OPT_SERVER_ID,
    DHCP_OPT_TFTP_SERVER,
    DHCP_OPT_CLASSLESS_STATIC_ROUTE,
    DHCP_OPT_MS_CLASSLESS_STATIC_ROUTE,
    DHCP_OPT_IP_FORWARD_ENABLE,
    DHCP_OPT_ROUTER_DISCOVERY,
    DHCP_OPT_ETHERNET_ENCAP,
    DHCP_OPT_DEFAULT_TTL,
    DHCP_OPT_TCP_TTL,
    DHCP_OPT_MTU,
    DHCP_OPT_LEASE_TIME,
    DHCP_OPT_T1,
    DHCP_OPT_T2,
    DHCP_OPT_WPAD,
    DHCP_OPT_BOOTFILE,
    DHCP_OPT_PATH_PREFIX,
    DHCP_OPT_TFTP_SERVER_ADDRESS,
    DHCP_OPT_DOMAIN_NAME,
};

static struct gen_opts_map supported_dhcpv6_opts[] = {
    DHCPV6_OPT_IA_ADDR,
    DHCPV6_OPT_SERVER_ID,
    DHCPV6_OPT_DOMAIN_SEARCH,
    DHCPV6_OPT_DNS_SERVER
};

static void
check_and_add_supported_dhcp_opts_to_sb_db(struct northd_context *ctx)
{
    struct hmap dhcp_opts_to_add = HMAP_INITIALIZER(&dhcp_opts_to_add);
    for (size_t i = 0; (i < sizeof(supported_dhcp_opts) /
                            sizeof(supported_dhcp_opts[0])); i++) {
        hmap_insert(&dhcp_opts_to_add, &supported_dhcp_opts[i].hmap_node,
                    dhcp_opt_hash(supported_dhcp_opts[i].name));
    }

    const struct sbrec_dhcp_options *opt_row, *opt_row_next;
    SBREC_DHCP_OPTIONS_FOR_EACH_SAFE(opt_row, opt_row_next, ctx->ovnsb_idl) {
        struct gen_opts_map *dhcp_opt =
            dhcp_opts_find(&dhcp_opts_to_add, opt_row->name);
        if (dhcp_opt) {
            hmap_remove(&dhcp_opts_to_add, &dhcp_opt->hmap_node);
        } else {
            sbrec_dhcp_options_delete(opt_row);
        }
    }

    struct gen_opts_map *opt;
    HMAP_FOR_EACH (opt, hmap_node, &dhcp_opts_to_add) {
        struct sbrec_dhcp_options *sbrec_dhcp_option =
            sbrec_dhcp_options_insert(ctx->ovnsb_txn);
        sbrec_dhcp_options_set_name(sbrec_dhcp_option, opt->name);
        sbrec_dhcp_options_set_code(sbrec_dhcp_option, opt->code);
        sbrec_dhcp_options_set_type(sbrec_dhcp_option, opt->type);
    }

    hmap_destroy(&dhcp_opts_to_add);
}

static void
check_and_add_supported_dhcpv6_opts_to_sb_db(struct northd_context *ctx)
{
    struct hmap dhcpv6_opts_to_add = HMAP_INITIALIZER(&dhcpv6_opts_to_add);
    for (size_t i = 0; (i < sizeof(supported_dhcpv6_opts) /
                            sizeof(supported_dhcpv6_opts[0])); i++) {
        hmap_insert(&dhcpv6_opts_to_add, &supported_dhcpv6_opts[i].hmap_node,
                    dhcp_opt_hash(supported_dhcpv6_opts[i].name));
    }

    const struct sbrec_dhcpv6_options *opt_row, *opt_row_next;
    SBREC_DHCPV6_OPTIONS_FOR_EACH_SAFE(opt_row, opt_row_next, ctx->ovnsb_idl) {
        struct gen_opts_map *dhcp_opt =
            dhcp_opts_find(&dhcpv6_opts_to_add, opt_row->name);
        if (dhcp_opt) {
            hmap_remove(&dhcpv6_opts_to_add, &dhcp_opt->hmap_node);
        } else {
            sbrec_dhcpv6_options_delete(opt_row);
        }
    }

    struct gen_opts_map *opt;
    HMAP_FOR_EACH(opt, hmap_node, &dhcpv6_opts_to_add) {
        struct sbrec_dhcpv6_options *sbrec_dhcpv6_option =
            sbrec_dhcpv6_options_insert(ctx->ovnsb_txn);
        sbrec_dhcpv6_options_set_name(sbrec_dhcpv6_option, opt->name);
        sbrec_dhcpv6_options_set_code(sbrec_dhcpv6_option, opt->code);
        sbrec_dhcpv6_options_set_type(sbrec_dhcpv6_option, opt->type);
    }

    hmap_destroy(&dhcpv6_opts_to_add);
}

static const char *rbac_chassis_auth[] =
    {"name"};
static const char *rbac_chassis_update[] =
    {"nb_cfg", "external_ids", "encaps", "vtep_logical_switches"};

static const char *rbac_encap_auth[] =
    {"chassis_name"};
static const char *rbac_encap_update[] =
    {"type", "options", "ip"};

static const char *rbac_port_binding_auth[] =
    {""};
static const char *rbac_port_binding_update[] =
    {"chassis"};

static const char *rbac_mac_binding_auth[] =
    {""};
static const char *rbac_mac_binding_update[] =
    {"logical_port", "ip", "mac", "datapath"};

static struct rbac_perm_cfg {
    const char *table;
    const char **auth;
    int n_auth;
    bool insdel;
    const char **update;
    int n_update;
    const struct sbrec_rbac_permission *row;
} rbac_perm_cfg[] = {
    {
        .table = "Chassis",
        .auth = rbac_chassis_auth,
        .n_auth = ARRAY_SIZE(rbac_chassis_auth),
        .insdel = true,
        .update = rbac_chassis_update,
        .n_update = ARRAY_SIZE(rbac_chassis_update),
        .row = NULL
    },{
        .table = "Encap",
        .auth = rbac_encap_auth,
        .n_auth = ARRAY_SIZE(rbac_encap_auth),
        .insdel = true,
        .update = rbac_encap_update,
        .n_update = ARRAY_SIZE(rbac_encap_update),
        .row = NULL
    },{
        .table = "Port_Binding",
        .auth = rbac_port_binding_auth,
        .n_auth = ARRAY_SIZE(rbac_port_binding_auth),
        .insdel = false,
        .update = rbac_port_binding_update,
        .n_update = ARRAY_SIZE(rbac_port_binding_update),
        .row = NULL
    },{
        .table = "MAC_Binding",
        .auth = rbac_mac_binding_auth,
        .n_auth = ARRAY_SIZE(rbac_mac_binding_auth),
        .insdel = true,
        .update = rbac_mac_binding_update,
        .n_update = ARRAY_SIZE(rbac_mac_binding_update),
        .row = NULL
    },{
        .table = NULL,
        .auth = NULL,
        .n_auth = 0,
        .insdel = false,
        .update = NULL,
        .n_update = 0,
        .row = NULL
    }
};

static bool
ovn_rbac_validate_perm(const struct sbrec_rbac_permission *perm)
{
    struct rbac_perm_cfg *pcfg;
    int i, j, n_found;

    for (pcfg = rbac_perm_cfg; pcfg->table; pcfg++) {
        if (!strcmp(perm->table, pcfg->table)) {
            break;
        }
    }
    if (!pcfg->table) {
        return false;
    }
    if (perm->n_authorization != pcfg->n_auth ||
        perm->n_update != pcfg->n_update) {
        return false;
    }
    if (perm->insert_delete != pcfg->insdel) {
        return false;
    }
    /* verify perm->authorization vs. pcfg->auth */
    n_found = 0;
    for (i = 0; i < pcfg->n_auth; i++) {
        for (j = 0; j < perm->n_authorization; j++) {
            if (!strcmp(pcfg->auth[i], perm->authorization[j])) {
                n_found++;
                break;
            }
        }
    }
    if (n_found != pcfg->n_auth) {
        return false;
    }

    /* verify perm->update vs. pcfg->update */
    n_found = 0;
    for (i = 0; i < pcfg->n_update; i++) {
        for (j = 0; j < perm->n_update; j++) {
            if (!strcmp(pcfg->update[i], perm->update[j])) {
                n_found++;
                break;
            }
        }
    }
    if (n_found != pcfg->n_update) {
        return false;
    }

    /* Success, db state matches expected state */
    pcfg->row = perm;
    return true;
}

static void
ovn_rbac_create_perm(struct rbac_perm_cfg *pcfg,
                     struct northd_context *ctx,
                     const struct sbrec_rbac_role *rbac_role)
{
    struct sbrec_rbac_permission *rbac_perm;

    rbac_perm = sbrec_rbac_permission_insert(ctx->ovnsb_txn);
    sbrec_rbac_permission_set_table(rbac_perm, pcfg->table);
    sbrec_rbac_permission_set_authorization(rbac_perm,
                                            pcfg->auth,
                                            pcfg->n_auth);
    sbrec_rbac_permission_set_insert_delete(rbac_perm, pcfg->insdel);
    sbrec_rbac_permission_set_update(rbac_perm,
                                     pcfg->update,
                                     pcfg->n_update);
    sbrec_rbac_role_update_permissions_setkey(rbac_role, pcfg->table,
                                              rbac_perm);
}

static void
check_and_update_rbac(struct northd_context *ctx)
{
    const struct sbrec_rbac_role *rbac_role = NULL;
    const struct sbrec_rbac_permission *perm_row, *perm_next;
    const struct sbrec_rbac_role *role_row, *role_row_next;
    struct rbac_perm_cfg *pcfg;

    for (pcfg = rbac_perm_cfg; pcfg->table; pcfg++) {
        pcfg->row = NULL;
    }

    SBREC_RBAC_PERMISSION_FOR_EACH_SAFE (perm_row, perm_next, ctx->ovnsb_idl) {
        if (!ovn_rbac_validate_perm(perm_row)) {
            sbrec_rbac_permission_delete(perm_row);
        }
    }
    SBREC_RBAC_ROLE_FOR_EACH_SAFE (role_row, role_row_next, ctx->ovnsb_idl) {
        if (strcmp(role_row->name, "ovn-controller")) {
            sbrec_rbac_role_delete(role_row);
        } else {
            rbac_role = role_row;
        }
    }

    if (!rbac_role) {
        rbac_role = sbrec_rbac_role_insert(ctx->ovnsb_txn);
        sbrec_rbac_role_set_name(rbac_role, "ovn-controller");
    }

    for (pcfg = rbac_perm_cfg; pcfg->table; pcfg++) {
        if (!pcfg->row) {
            ovn_rbac_create_perm(pcfg, ctx, rbac_role);
        }
    }
}

/* Updates the sb_cfg and hv_cfg columns in the northbound NB_Global table. */
static void
update_northbound_cfg(struct northd_context *ctx,
                      struct ovsdb_idl_loop *sb_loop)
{
    /* Update northbound sb_cfg if appropriate. */
    const struct nbrec_nb_global *nbg = nbrec_nb_global_first(ctx->ovnnb_idl);
    int64_t sb_cfg = sb_loop->cur_cfg;
    if (nbg && sb_cfg && nbg->sb_cfg != sb_cfg) {
        nbrec_nb_global_set_sb_cfg(nbg, sb_cfg);
    }

    /* Update northbound hv_cfg if appropriate. */
    if (nbg) {
        /* Find minimum nb_cfg among all chassis. */
        const struct sbrec_chassis *chassis;
        int64_t hv_cfg = nbg->nb_cfg;
        SBREC_CHASSIS_FOR_EACH (chassis, ctx->ovnsb_idl) {
            if (chassis->nb_cfg < hv_cfg) {
                hv_cfg = chassis->nb_cfg;
            }
        }

        /* Update hv_cfg. */
        if (nbg->hv_cfg != hv_cfg) {
            nbrec_nb_global_set_hv_cfg(nbg, hv_cfg);
        }
    }
}

/* Handle a fairly small set of changes in the southbound database. */
static void
ovnsb_db_run(struct northd_context *ctx, struct ovsdb_idl_loop *sb_loop,
             struct hmap *ports)
{
    if (!ctx->ovnnb_txn || !ovsdb_idl_has_ever_connected(ctx->ovnsb_idl)) {
        return;
    }

    struct shash ha_ref_chassis_map = SHASH_INITIALIZER(&ha_ref_chassis_map);
    handle_port_binding_changes(ctx, ports, &ha_ref_chassis_map);
    update_northbound_cfg(ctx, sb_loop);
    if (ctx->ovnsb_txn) {
        update_sb_ha_group_ref_chassis(&ha_ref_chassis_map);
    }
    shash_destroy(&ha_ref_chassis_map);
}

static void
ovn_db_run(struct northd_context *ctx,
           struct ovsdb_idl_index *sbrec_chassis_by_name,
           struct ovsdb_idl_loop *ovnsb_idl_loop)
{
    struct hmap datapaths, ports;
    struct ovs_list lr_list;
    ovs_list_init(&lr_list);
    hmap_init(&datapaths);
    hmap_init(&ports);
    ovnnb_db_run(ctx, sbrec_chassis_by_name, ovnsb_idl_loop,
                 &datapaths, &ports, &lr_list);
    ovnsb_db_run(ctx, ovnsb_idl_loop, &ports);
    destroy_datapaths_and_ports(&datapaths, &ports, &lr_list);
}

static void
parse_options(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    enum {
        DAEMON_OPTION_ENUMS,
        VLOG_OPTION_ENUMS,
        SSL_OPTION_ENUMS,
    };
    static const struct option long_options[] = {
        {"ovnsb-db", required_argument, NULL, 'd'},
        {"ovnnb-db", required_argument, NULL, 'D'},
        {"unixctl", required_argument, NULL, 'u'},
        {"help", no_argument, NULL, 'h'},
        {"options", no_argument, NULL, 'o'},
        {"version", no_argument, NULL, 'V'},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {NULL, 0, NULL, 0},
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        DAEMON_OPTION_HANDLERS;
        VLOG_OPTION_HANDLERS;
        STREAM_SSL_OPTION_HANDLERS;

        case 'd':
            ovnsb_db = optarg;
            break;

        case 'D':
            ovnnb_db = optarg;
            break;

        case 'u':
            unixctl_path = optarg;
            break;

        case 'h':
            usage();
            exit(EXIT_SUCCESS);

        case 'o':
            ovs_cmdl_print_options(long_options);
            exit(EXIT_SUCCESS);

        case 'V':
            ovs_print_version(0, 0);
            exit(EXIT_SUCCESS);

        default:
            break;
        }
    }

    if (!ovnsb_db) {
        ovnsb_db = default_sb_db();
    }

    if (!ovnnb_db) {
        ovnnb_db = default_nb_db();
    }

    free(short_options);
}

static void
add_column_noalert(struct ovsdb_idl *idl,
                   const struct ovsdb_idl_column *column)
{
    ovsdb_idl_add_column(idl, column);
    ovsdb_idl_omit_alert(idl, column);
}

int
main(int argc, char *argv[])
{
    int res = EXIT_SUCCESS;
    struct unixctl_server *unixctl;
    int retval;
    bool exiting;
    bool paused;

    fatal_ignore_sigpipe();
    ovs_cmdl_proctitle_init(argc, argv);
    set_program_name(argv[0]);
    service_start(&argc, &argv);
    parse_options(argc, argv);

    daemonize_start(false);

    if (!unixctl_path) {
        char *abs_unixctl_path = get_abs_unix_ctl_path();
        retval = unixctl_server_create(abs_unixctl_path, &unixctl);
        free(abs_unixctl_path);
    } else {
        retval = unixctl_server_create(unixctl_path, &unixctl);
    }

    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 0, ovn_northd_exit, &exiting);
    unixctl_command_register("pause", "", 0, 0, ovn_northd_pause, &paused);
    unixctl_command_register("resume", "", 0, 0, ovn_northd_resume, &paused);
    unixctl_command_register("is-paused", "", 0, 0, ovn_northd_is_paused,
                             &paused);

    daemonize_complete();

    /* We want to detect (almost) all changes to the ovn-nb db. */
    struct ovsdb_idl_loop ovnnb_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create(ovnnb_db, &nbrec_idl_class, true, true));
    ovsdb_idl_omit_alert(ovnnb_idl_loop.idl, &nbrec_nb_global_col_sb_cfg);
    ovsdb_idl_omit_alert(ovnnb_idl_loop.idl, &nbrec_nb_global_col_hv_cfg);

    /* We want to detect only selected changes to the ovn-sb db. */
    struct ovsdb_idl_loop ovnsb_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create(ovnsb_db, &sbrec_idl_class, false, true));

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_sb_global);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_sb_global_col_nb_cfg);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_sb_global_col_options);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_sb_global_col_ipsec);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_logical_flow);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_logical_flow_col_logical_datapath);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_pipeline);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_table_id);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_priority);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_match);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_actions);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_multicast_group);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_multicast_group_col_datapath);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_multicast_group_col_tunnel_key);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_multicast_group_col_name);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_multicast_group_col_ports);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_datapath_binding);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_datapath_binding_col_tunnel_key);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_datapath_binding_col_external_ids);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_port_binding);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_binding_col_datapath);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_port_binding_col_logical_port);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_port_binding_col_tunnel_key);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_port_binding_col_parent_port);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_binding_col_tag);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_binding_col_type);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_binding_col_options);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_binding_col_mac);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_port_binding_col_nat_addresses);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_port_binding_col_chassis);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_port_binding_col_gateway_chassis);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_port_binding_col_ha_chassis_group);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_port_binding_col_virtual_parent);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_gateway_chassis_col_chassis);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_gateway_chassis_col_name);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_gateway_chassis_col_priority);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_gateway_chassis_col_external_ids);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl,
                         &sbrec_gateway_chassis_col_options);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_port_binding_col_external_ids);
    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_mac_binding);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_mac_binding_col_datapath);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_mac_binding_col_ip);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_mac_binding_col_mac);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_mac_binding_col_logical_port);
    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_dhcp_options);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcp_options_col_code);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcp_options_col_type);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcp_options_col_name);
    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_dhcpv6_options);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcpv6_options_col_code);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcpv6_options_col_type);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dhcpv6_options_col_name);
    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_address_set);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_address_set_col_name);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_address_set_col_addresses);
    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_port_group);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_group_col_name);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_port_group_col_ports);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_dns);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dns_col_datapaths);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dns_col_records);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_dns_col_external_ids);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_rbac_role);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_rbac_role_col_name);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_rbac_role_col_permissions);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_rbac_permission);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_rbac_permission_col_table);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_rbac_permission_col_authorization);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_rbac_permission_col_insert_delete);
    add_column_noalert(ovnsb_idl_loop.idl, &sbrec_rbac_permission_col_update);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_meter);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_col_name);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_col_unit);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_col_bands);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_meter_band);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_band_col_action);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_band_col_rate);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_meter_band_col_burst_size);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_chassis);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_chassis_col_nb_cfg);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_chassis_col_name);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_ha_chassis);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_col_chassis);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_col_priority);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_col_external_ids);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_ha_chassis_group);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_group_col_name);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_group_col_ha_chassis);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_group_col_external_ids);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ha_chassis_group_col_ref_chassis);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_igmp_group);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_igmp_group_col_address);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_igmp_group_col_datapath);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_igmp_group_col_chassis);
    ovsdb_idl_add_column(ovnsb_idl_loop.idl, &sbrec_igmp_group_col_ports);

    ovsdb_idl_add_table(ovnsb_idl_loop.idl, &sbrec_table_ip_multicast);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_datapath);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_enabled);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_querier);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_eth_src);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_ip4_src);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_table_size);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_idle_timeout);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_query_interval);
    add_column_noalert(ovnsb_idl_loop.idl,
                       &sbrec_ip_multicast_col_query_max_resp);

    struct ovsdb_idl_index *sbrec_chassis_by_name
        = chassis_index_create(ovnsb_idl_loop.idl);

    struct ovsdb_idl_index *sbrec_ha_chassis_grp_by_name
        = ha_chassis_group_index_create(ovnsb_idl_loop.idl);

    struct ovsdb_idl_index *sbrec_mcast_group_by_name_dp
        = mcast_group_index_create(ovnsb_idl_loop.idl);

    struct ovsdb_idl_index *sbrec_ip_mcast_by_dp
        = ip_mcast_index_create(ovnsb_idl_loop.idl);

    /* Ensure that only a single ovn-northd is active in the deployment by
     * acquiring a lock called "ovn_northd" on the southbound database
     * and then only performing DB transactions if the lock is held. */
    ovsdb_idl_set_lock(ovnsb_idl_loop.idl, "ovn_northd");
    bool had_lock = false;

    /* Main loop. */
    exiting = false;
    paused = false;
    while (!exiting) {
        if (!paused) {
            struct northd_context ctx = {
                .ovnnb_idl = ovnnb_idl_loop.idl,
                .ovnnb_txn = ovsdb_idl_loop_run(&ovnnb_idl_loop),
                .ovnsb_idl = ovnsb_idl_loop.idl,
                .ovnsb_txn = ovsdb_idl_loop_run(&ovnsb_idl_loop),
                .sbrec_ha_chassis_grp_by_name = sbrec_ha_chassis_grp_by_name,
                .sbrec_mcast_group_by_name_dp = sbrec_mcast_group_by_name_dp,
                .sbrec_ip_mcast_by_dp = sbrec_ip_mcast_by_dp,
            };

            if (!had_lock && ovsdb_idl_has_lock(ovnsb_idl_loop.idl)) {
                VLOG_INFO("ovn-northd lock acquired. "
                        "This ovn-northd instance is now active.");
                had_lock = true;
            } else if (had_lock && !ovsdb_idl_has_lock(ovnsb_idl_loop.idl)) {
                VLOG_INFO("ovn-northd lock lost. "
                        "This ovn-northd instance is now on standby.");
                had_lock = false;
            }

            if (ovsdb_idl_has_lock(ovnsb_idl_loop.idl)) {
                ovn_db_run(&ctx, sbrec_chassis_by_name, &ovnsb_idl_loop);
                if (ctx.ovnsb_txn) {
                    check_and_add_supported_dhcp_opts_to_sb_db(&ctx);
                    check_and_add_supported_dhcpv6_opts_to_sb_db(&ctx);
                    check_and_update_rbac(&ctx);
                }
            }

            ovsdb_idl_loop_commit_and_wait(&ovnnb_idl_loop);
            ovsdb_idl_loop_commit_and_wait(&ovnsb_idl_loop);
        } else {
            /* ovn-northd is paused
             *    - we still want to handle any db updates and update the
             *      local IDL. Otherwise, when it is resumed, the local IDL
             *      copy will be out of sync.
             *    - but we don't want to create any txns.
             * */
            ovsdb_idl_run(ovnnb_idl_loop.idl);
            ovsdb_idl_run(ovnsb_idl_loop.idl);
            ovsdb_idl_wait(ovnnb_idl_loop.idl);
            ovsdb_idl_wait(ovnsb_idl_loop.idl);
        }

        unixctl_server_run(unixctl);
        unixctl_server_wait(unixctl);
        if (exiting) {
            poll_immediate_wake();
        }

        poll_block();
        if (should_service_stop()) {
            exiting = true;
        }
    }

    unixctl_server_destroy(unixctl);
    ovsdb_idl_loop_destroy(&ovnnb_idl_loop);
    ovsdb_idl_loop_destroy(&ovnsb_idl_loop);
    service_stop();

    exit(res);
}

static void
ovn_northd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;

    unixctl_command_reply(conn, NULL);
}

static void
ovn_northd_pause(struct unixctl_conn *conn, int argc OVS_UNUSED,
                const char *argv[] OVS_UNUSED, void *pause_)
{
    bool *pause = pause_;
    *pause = true;

    unixctl_command_reply(conn, NULL);
}

static void
ovn_northd_resume(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[] OVS_UNUSED, void *pause_)
{
    bool *pause = pause_;
    *pause = false;

    unixctl_command_reply(conn, NULL);
}

static void
ovn_northd_is_paused(struct unixctl_conn *conn, int argc OVS_UNUSED,
                     const char *argv[] OVS_UNUSED, void *paused_)
{
    bool *paused = paused_;
    if (*paused) {
        unixctl_command_reply(conn, "true");
    } else {
        unixctl_command_reply(conn, "false");
    }
}
