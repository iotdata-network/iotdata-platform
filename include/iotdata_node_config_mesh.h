#ifndef IOTDATA_NODE_CONFIG_MESH_H
#define IOTDATA_NODE_CONFIG_MESH_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config_mesh.h - CONFIG rows for MESHING, defined once and shared by every role that
// meshes. The sibling of iotdata_node_config_device_e22900t22.h, one layer up: that one is what of the HARDWARE
// is settable, this is what of the MESH is.
//
// THREE GROUPS, BECAUSE THERE ARE THREE AUDIENCES and a row belongs to exactly one of them:
//
//     IOTDATA_NODE_CONFIG_ENTRIES_MESH(X)           what any mesh participant has, which turns out to be
//                                              only two things: whether it meshes, and whether it
//                                              says so in the log
//     IOTDATA_NODE_CONFIG_ENTRIES_MESH_RELAY(X)     what only a relay has -- it has a PARENT to lose,
//                                              a beacon to rebroadcast, frames to forward, and a
//                                              PEER TABLE to age out. The peer TTL and the initial
//                                              TTL started in the group above and did not belong
//                                              there: a root keeps no peer table (its station table
//                                              is a different thing, holding sensors that never
//                                              mesh) and originates no FORWARD, so both were rows
//                                              it carried and nothing read.
//     IOTDATA_NODE_CONFIG_ENTRIES_MESH_GATEWAY(X)   what only the root has -- it SETS the tree's cadence
//
// A group is a macro, not a table, so one file can hold all three and each application composes
// only the ones its role has. Nothing gateway-shaped is compiled into a relay:
//
//     #include "iotdata_node_mesh_tuning.h"     // the numbers (see the note below)
//     #include "iotdata_node_config.h"         // the types
//     #include "iotdata_node_config_mesh.h"    // the validators and the row blocks
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(...this app's own...)
//         IOTDATA_NODE_CONFIG_ENTRIES_MESH(X)
//         IOTDATA_NODE_CONFIG_ENTRIES_MESH_RELAY(X)
//     #include "iotdata_node_config.h"         // expand
//
// A GATEWAY NEED NOT MESH AT ALL, and one that does not simply composes neither mesh group; its
// CONFIG report then has no mesh rows in it, which is the honest answer rather than a set of
// numbers nothing reads.
//
// NOT IN HERE: the stations and filters tables. A filter blocking downstream frames from an origin
// is useful to a gateway with no mesh, and would be useful to a plain sensor -- it is a different
// axis that happens to have been built alongside this one, and it gets its own block.
//
// IDS ARE FIXED AND NEVER REUSED. 0x040 mesh, 0x050 relay, 0x060 gateway -- fixed so that one
// name->id map fragment covers a whole fleet and a human reading a hex dump recognises them, and
// never reused so that a cached map stays valid across firmware versions. Retire an id by burning
// it, not by recycling it; iotdata_node_config.h turns a reuse into a compile error.
//
//   ...with ONE exception on the record, taken deliberately while that is still free. 0x042 and
//   0x050 held MESH_PEER_TTL_MS and MESH_PARENT_TIMEOUT_MS, absolute times; they now hold
//   MESH_PEER_TTL_ROUNDS and MESH_PARENT_MISS_ROUNDS, the same decisions counted in the root's
//   beacon rounds (NOTES_ISSUES.md I.1). Same meaning, different unit and width, so a stale cached
//   map would read a live value and be wrong about it -- which is exactly what burning an id
//   prevents. It is safe here only because nothing is deployed that cannot be reflashed. Once
//   something is, this rule has no exceptions.
//
// THE DEFAULTS LIVE IN iotdata_node_mesh_tuning.h, which this header requires. They are separate so
// that a mesh MODULE can have the numbers without the CONFIG machinery -- see that file.
//
// UNITS ARE IN THE NAME because the roles genuinely differ: a relay's queues are in milliseconds,
// the root's cadence is in seconds, and a relay's tolerance is in ROUNDS of that cadence -- which
// is the one unit that cannot be stated wrongly, since it is relative to what the root says.
// Converting silently at any of those seams would be one more place to be wrong.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_MESH_ENABLE
#define IOTDATA_NODE_CONFIG_MESH_ENABLE 1
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_DEBUG
#define IOTDATA_NODE_CONFIG_MESH_DEBUG 0
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_PEER_TTL_ROUNDS
#define IOTDATA_NODE_CONFIG_MESH_PEER_TTL_ROUNDS 5u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_TTL_INIT
#define IOTDATA_NODE_CONFIG_MESH_TTL_INIT IOTDATA_MESH_TTL_DEFAULT
#endif

#ifndef IOTDATA_NODE_CONFIG_MESH_REPORT_PEERS_MS
#define IOTDATA_NODE_CONFIG_MESH_REPORT_PEERS_MS 900000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_PARENT_MISS_ROUNDS
#define IOTDATA_NODE_CONFIG_MESH_PARENT_MISS_ROUNDS 3u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_HYSTERESIS_DB
#define IOTDATA_NODE_CONFIG_MESH_HYSTERESIS_DB 10
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MIN_MS
#define IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MIN_MS 1000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MAX_MS
#define IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MAX_MS 5000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_BEACON_EXPIRY_MS
#define IOTDATA_NODE_CONFIG_MESH_BEACON_EXPIRY_MS 8000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_RERR_EXPIRY_MS
#define IOTDATA_NODE_CONFIG_MESH_RERR_EXPIRY_MS 8000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_FORWARD_SEEN_TTL_MS
#define IOTDATA_NODE_CONFIG_MESH_FORWARD_SEEN_TTL_MS 8000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_FORWARD_SUPPRESS
#define IOTDATA_NODE_CONFIG_MESH_FORWARD_SUPPRESS 0 /* 0=ack 1=lower-cost 2=always */
#endif
/* 500-3000, not 200-1000: latency is cheap here, air time is not. At 2.4k a gateway's direct ACK
   reaches a relay ~370ms after the frame, and a shorter floor forwards what the gateway already has
   (lab: 26% of direct-ACKed frames at 200, 3% at 400). A wide window lets the first relay's forward,
   and its ACK (~550ms), silence the others that heard the same frame. */
#ifndef IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MIN_MS
#define IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MIN_MS 500u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MAX_MS
#define IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MAX_MS 3000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_FORWARD_EXPIRY_MS
#define IOTDATA_NODE_CONFIG_MESH_FORWARD_EXPIRY_MS 3000u
#endif

/* 150s, not 60. A network here is planned and static: a relay goes in where a gap was found, so
   losing a parent is the exception, and the cadence is paid for by every relay, once per round,
   for ever. At 150s a lost parent is still noticed in 3 rounds (7.5 min) and a peer forgotten in 5. */
#ifndef IOTDATA_NODE_CONFIG_MESH_BEACON_INTERVAL_S
#define IOTDATA_NODE_CONFIG_MESH_BEACON_INTERVAL_S 150u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT
#define IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT 1
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT_SLOT
#define IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT_SLOT 0u /* 0 = from the station id */
#endif

#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_MAX_RETRIES
#define IOTDATA_NODE_CONFIG_MESH_ACK_MAX_RETRIES 3 /* 0 = best-effort: no pending table, no retry */
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_TIMEOUT_MS
#define IOTDATA_NODE_CONFIG_MESH_ACK_TIMEOUT_MS 3000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_EVICT
#define IOTDATA_NODE_CONFIG_MESH_ACK_EVICT 0 /* 0=oldest 1=oldest-of-station */
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_STATION_MAX
#define IOTDATA_NODE_CONFIG_MESH_ACK_STATION_MAX 0 /* 0 = no per-station cap */
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF
#define IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF 0 /* 0=linear 1=scaled */
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_FACTOR_PCT
#define IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_FACTOR_PCT 200u /* x2 per attempt, when scaled */
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_MAX_MS
#define IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_MAX_MS 30000u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_REQUEUE_MS
#define IOTDATA_NODE_CONFIG_MESH_ACK_REQUEUE_MS 500u
#endif
#ifndef IOTDATA_NODE_CONFIG_MESH_ACK_PARK_MS
#define IOTDATA_NODE_CONFIG_MESH_ACK_PARK_MS 30000u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

__attribute__((unused)) static bool iotdata_node_config_mesh_jitter_min_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
__attribute__((unused)) static bool iotdata_node_config_mesh_jitter_max_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
__attribute__((unused)) static bool iotdata_node_config_mesh_backoff_min_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
__attribute__((unused)) static bool iotdata_node_config_mesh_backoff_max_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
__attribute__((unused)) static bool iotdata_node_config_mesh_parent_miss_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
__attribute__((unused)) static bool iotdata_node_config_mesh_ack_backoff_max_ok(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_MESH_NOTIFY
static inline bool iotdata_node_config_mesh_changed(__attribute__((unused)) const iotdata_node_config_row_t *const row, __attribute__((unused)) const iotdata_node_config_value_t *const was,
                                                    __attribute__((unused)) const iotdata_node_config_info_t *const info) {
    return false;
}
#define IOTDATA_NODE_CONFIG_MESH_NOTIFY iotdata_node_config_mesh_changed
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CFGID_MESH_ENABLE                    0x040
#define IOTDATA_NODE_CFGID_MESH_DEBUG                     0x041
#define IOTDATA_NODE_CFGID_MESH_PEER_TTL_ROUNDS           0x042
#define IOTDATA_NODE_CFGID_MESH_TTL_INIT                  0x043
#define IOTDATA_NODE_CFGID_MESH_PARENT_MISS_ROUNDS        0x050
#define IOTDATA_NODE_CFGID_MESH_HYSTERESIS_DB             0x051
#define IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MIN_MS 0x052
#define IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MAX_MS 0x053
#define IOTDATA_NODE_CFGID_MESH_BEACON_EXPIRY_MS          0x054
#define IOTDATA_NODE_CFGID_MESH_RERR_EXPIRY_MS            0x055
#define IOTDATA_NODE_CFGID_MESH_FORWARD_SEEN_TTL_MS       0x056
#define IOTDATA_NODE_CFGID_MESH_FORWARD_SUPPRESS          0x057
#define IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MIN_MS    0x058
#define IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MAX_MS    0x059
#define IOTDATA_NODE_CFGID_MESH_FORWARD_EXPIRY_MS         0x05A
#define IOTDATA_NODE_CFGID_MESH_REPORT_PEERS_MS           0x05B
#define IOTDATA_NODE_CFGID_MESH_BEACON_INTERVAL_S         0x060
#define IOTDATA_NODE_CFGID_MESH_ACK_DIRECT                0x061
#define IOTDATA_NODE_CFGID_MESH_ACK_DIRECT_SLOT           0x062

#define IOTDATA_NODE_CFGID_MESH_ACK_MAX_RETRIES           0x070
#define IOTDATA_NODE_CFGID_MESH_ACK_TIMEOUT_MS            0x071
#define IOTDATA_NODE_CFGID_MESH_ACK_EVICT                 0x072
#define IOTDATA_NODE_CFGID_MESH_ACK_STATION_MAX           0x073
#define IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF               0x074
#define IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF_FACTOR_PCT    0x075
#define IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF_MAX_MS        0x076
#define IOTDATA_NODE_CFGID_MESH_ACK_REQUEUE_MS            0x077
#define IOTDATA_NODE_CFGID_MESH_ACK_PARK_MS               0x078

/* 0x040-0x04F -- any mesh participant */
#define IOTDATA_NODE_CONFIG_ENTRIES_MESH(X) \
    X(MESH_ENABLE, IOTDATA_NODE_CFGID_MESH_ENABLE, BOOL, 0, 1, IOTDATA_NODE_CONFIG_MESH_ENABLE, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, "take part in the mesh at all") \
    X(MESH_DEBUG, IOTDATA_NODE_CFGID_MESH_DEBUG, BOOL, 0, 1, IOTDATA_NODE_CONFIG_MESH_DEBUG, IOTDATA_NODE_CONFIG_FLAG_LOCAL, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, "log every mesh decision")

/* 0x050-0x05F -- a relay: it has a parent to lose, a beacon to rebroadcast, frames to forward */
#define IOTDATA_NODE_CONFIG_ENTRIES_MESH_RELAY(X) \
    /* KEEPING THEIR 0x04x IDS while sitting in the 0x05x group, because an id is fixed for the life \
       of the fleet and a row that moved between groups is still the same row. Renumbering them to \
       look tidy would strand every cached name->id map and every hex dump anybody has read. */ \
    X(MESH_PEER_TTL_ROUNDS, IOTDATA_NODE_CFGID_MESH_PEER_TTL_ROUNDS, U8, 1, 60, IOTDATA_NODE_CONFIG_MESH_PEER_TTL_ROUNDS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "forget a peer after this many of the root's beacon rounds without hearing it") \
    X(MESH_TTL_INIT, IOTDATA_NODE_CFGID_MESH_TTL_INIT, U8, 1, IOTDATA_MESH_TTL_MAX, IOTDATA_NODE_CONFIG_MESH_TTL_INIT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, "how many hops a frame we originate may take") \
    X(MESH_PARENT_MISS_ROUNDS, IOTDATA_NODE_CFGID_MESH_PARENT_MISS_ROUNDS, U8, 1, 60, IOTDATA_NODE_CONFIG_MESH_PARENT_MISS_ROUNDS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_parent_miss_ok, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "declare the parent lost after this many of its beacon rounds are missed") \
    X(MESH_HYSTERESIS_DB, IOTDATA_NODE_CFGID_MESH_HYSTERESIS_DB, U8, 0, 30, IOTDATA_NODE_CONFIG_MESH_HYSTERESIS_DB, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how much stronger an equal-cost parent must be before switching to it") \
    X(MESH_REBROADCAST_JITTER_MIN_MS, IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MIN_MS, U32, 0, 60000, IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MIN_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_jitter_min_ok, \
      IOTDATA_NODE_CONFIG_MESH_NOTIFY, "the shortest wait before rebroadcasting a beacon") \
    X(MESH_REBROADCAST_JITTER_MAX_MS, IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MAX_MS, U32, 0, 60000, IOTDATA_NODE_CONFIG_MESH_REBROADCAST_JITTER_MAX_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_jitter_max_ok, \
      IOTDATA_NODE_CONFIG_MESH_NOTIFY, "the longest, so a tree does not rebroadcast in unison") \
    X(MESH_BEACON_EXPIRY_MS, IOTDATA_NODE_CFGID_MESH_BEACON_EXPIRY_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_BEACON_EXPIRY_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "drop a queued beacon unsent this long past due") \
    X(MESH_RERR_EXPIRY_MS, IOTDATA_NODE_CFGID_MESH_RERR_EXPIRY_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_RERR_EXPIRY_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "drop a queued route error unsent this long past due") \
    X(MESH_FORWARD_SEEN_TTL_MS, IOTDATA_NODE_CFGID_MESH_FORWARD_SEEN_TTL_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_FORWARD_SEEN_TTL_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how long an origin+sequence stays remembered, for dedup") \
    X(MESH_FORWARD_SUPPRESS, IOTDATA_NODE_CFGID_MESH_FORWARD_SUPPRESS, U8, 0, 2, IOTDATA_NODE_CONFIG_MESH_FORWARD_SUPPRESS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "what retires a queued forward: 0 an ack, 1 a closer peer, 2 any peer") \
    X(MESH_FORWARD_BACKOFF_MIN_MS, IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MIN_MS, U32, 0, 60000, IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MIN_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_backoff_min_ok, \
      IOTDATA_NODE_CONFIG_MESH_NOTIFY, "the shortest wait before forwarding") \
    X(MESH_FORWARD_BACKOFF_MAX_MS, IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MAX_MS, U32, 0, 60000, IOTDATA_NODE_CONFIG_MESH_FORWARD_BACKOFF_MAX_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_backoff_max_ok, \
      IOTDATA_NODE_CONFIG_MESH_NOTIFY, "the longest, so two relays do not forward together") \
    X(MESH_FORWARD_EXPIRY_MS, IOTDATA_NODE_CFGID_MESH_FORWARD_EXPIRY_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_FORWARD_EXPIRY_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "drop a queued forward unsent this long past due") \
    X(MESH_REPORT_PEERS_MS, IOTDATA_NODE_CFGID_MESH_REPORT_PEERS_MS, U32, 1000, 3600000, IOTDATA_NODE_CONFIG_MESH_REPORT_PEERS_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how often to tell the gateway what this node can hear")

/* 0x070-0x07F -- the relay's ACK tracking, appended to the relay group above. */
#define IOTDATA_NODE_CONFIG_ENTRIES_MESH_ACK(X) \
    X(MESH_ACK_MAX_RETRIES, IOTDATA_NODE_CFGID_MESH_ACK_MAX_RETRIES, U8, 0, 15, IOTDATA_NODE_CONFIG_MESH_ACK_MAX_RETRIES, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "retries for an un-acked forward before giving up (0 = best effort)") \
    X(MESH_ACK_TIMEOUT_MS, IOTDATA_NODE_CFGID_MESH_ACK_TIMEOUT_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_ACK_TIMEOUT_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how long to wait for an ack before retrying") \
    X(MESH_ACK_EVICT, IOTDATA_NODE_CFGID_MESH_ACK_EVICT, U8, 0, 1, IOTDATA_NODE_CONFIG_MESH_ACK_EVICT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "whose entry goes when the pending table is full: 0 the oldest, 1 the oldest of that station") \
    X(MESH_ACK_STATION_MAX, IOTDATA_NODE_CFGID_MESH_ACK_STATION_MAX, U8, 0, 255, IOTDATA_NODE_CONFIG_MESH_ACK_STATION_MAX, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "cap on pending entries for one station (0 = no cap)") \
    X(MESH_ACK_BACKOFF, IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF, U8, 0, 1, IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "0 the same wait every attempt, 1 widening with each failure") \
    X(MESH_ACK_BACKOFF_FACTOR_PCT, IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF_FACTOR_PCT, U16, 100, 1000, IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_FACTOR_PCT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how much each failure widens the wait, as a percentage") \
    X(MESH_ACK_BACKOFF_MAX_MS, IOTDATA_NODE_CFGID_MESH_ACK_BACKOFF_MAX_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_ACK_BACKOFF_MAX_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, iotdata_node_config_mesh_ack_backoff_max_ok, \
      IOTDATA_NODE_CONFIG_MESH_NOTIFY, "the ceiling that widening stops at") \
    X(MESH_ACK_REQUEUE_MS, IOTDATA_NODE_CFGID_MESH_ACK_REQUEUE_MS, U32, 0, 600000, IOTDATA_NODE_CONFIG_MESH_ACK_REQUEUE_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "wait before re-submitting a retry the radio could not take") \
    X(MESH_ACK_PARK_MS, IOTDATA_NODE_CFGID_MESH_ACK_PARK_MS, U32, 100, 600000, IOTDATA_NODE_CONFIG_MESH_ACK_PARK_MS, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how long a frame with its retries spent sleeps before being looked at again")

/* 0x060-0x06F -- the root: it sets the tree's cadence, and has no parent to lose.
 *
 * ONE NUMBER, ON ONE BOX, SETTING THE WHOLE FLEET'S CLOCK
 *
 * A relay does not beacon on a timer; it rebroadcasts once when what it advertises changes. So this
 * interval is how often every relay below hears anything at all, and every relay's ageing constant
 * is a multiple of.
 */
#define IOTDATA_NODE_CONFIG_ENTRIES_MESH_GATEWAY(X) \
    X(MESH_BEACON_INTERVAL_S, IOTDATA_NODE_CFGID_MESH_BEACON_INTERVAL_S, U16, 5, 3600, IOTDATA_NODE_CONFIG_MESH_BEACON_INTERVAL_S, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "how often the root beacons, which sets the whole tree's cadence") \
    X(MESH_ACK_DIRECT, IOTDATA_NODE_CFGID_MESH_ACK_DIRECT, BOOL, 0, 1, IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "acknowledge sensor frames heard directly, so a relay that heard them too drops its forward") \
    X(MESH_ACK_DIRECT_SLOT, IOTDATA_NODE_CFGID_MESH_ACK_DIRECT_SLOT, U8, 0, 4, IOTDATA_NODE_CONFIG_MESH_ACK_DIRECT_SLOT, IOTDATA_NODE_CONFIG_FLAG_NONE, NULL, IOTDATA_NODE_CONFIG_MESH_NOTIFY, \
      "which 200ms slot that ack goes in, so two gateways do not talk over each other: 1 at once, 2-4 later, 0 from the station id")

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_MESH_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_EXPANDED) && !defined(IOTDATA_NODE_CONFIG_MESH_APPLIED)
#define IOTDATA_NODE_CONFIG_MESH_APPLIED

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool iotdata_node_config_mesh_ack_backoff_max_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t base;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_ACK_TIMEOUT_MS, &base) && v->u >= base.u;
}
static bool iotdata_node_config_mesh_jitter_min_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t mx;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MAX_MS, &mx) && v->u <= mx.u;
}
static bool iotdata_node_config_mesh_jitter_max_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t mn;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_REBROADCAST_JITTER_MIN_MS, &mn) && v->u >= mn.u;
}
static bool iotdata_node_config_mesh_backoff_min_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t mx;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MAX_MS, &mx) && v->u <= mx.u;
}
static bool iotdata_node_config_mesh_backoff_max_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t mn;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_FORWARD_BACKOFF_MIN_MS, &mn) && v->u >= mn.u;
}
static bool iotdata_node_config_mesh_parent_miss_ok(__attribute__((unused)) const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    iotdata_node_config_value_t ttl;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFGID_MESH_PEER_TTL_ROUNDS, &ttl) && v->u <= ttl.u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONFIG_EXPANDED && !IOTDATA_NODE_CONFIG_MESH_APPLIED */
