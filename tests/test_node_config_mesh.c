// test_node_config_mesh.c - the shared MESH config blocks: three groups, one file, composed by role.
//
// Composes all three here on purpose. No real node does -- a relay takes MESH+MESH_RELAY and the
// root takes MESH+MESH_GATEWAY -- but composing them together is what proves the ids do not collide
// and that one file really can serve both roles.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* the config table's text parser: strtof, strtoll */
#include <string.h>

#include "iotdata.h"
#include "iotdata_mesh.h"
#include "iotdata_node.h"
#include "iotdata_node_partial.h"

#include "device/d_format.h" /* snprintf_inline, which the config formatter returns through */
#include "device/d_module_datastore_linux.h"
#include "iotdata_node_config.h"      /* the types */
#include "device/d_module_buffers.h"  /* the down store holds frames in a pool, so its types come first */
#include "iotdata_node_down.h"        /* iotdata_node_down_t, for the DOWN block */
#include "iotdata_node_config_down.h" /* the downstream hold store's rows */
#include "iotdata_node_config_mesh.h" /* the validators and the row blocks */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_DOWN(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_RELAY(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_ACK(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_GATEWAY(X)

#include "iotdata_node_config.h"      /* expand */
#include "iotdata_node_config_down.h" /* and now iotdata_node_config_down_apply() */
#include "iotdata_node_config_mesh.h" /* and now the cross-entry validator bodies */

static bool set_one(datastore_t *const ds, const uint16_t id, const uint64_t u) {
    iotdata_node_config_update_t up;
    iotdata_node_config_update_begin(&up, false);
    (void)iotdata_node_config_update_stage(&up, id, &(iotdata_node_config_value_t){ .u = u });
    return iotdata_node_config_update_commit(&up, ds, NULL);
}

int main(void) {
    printf("iotdata_node_config_mesh: three groups, one file, composed by role\n\n");
    datastore_t ds;
    assert(datastore_open(&ds, "/tmp/iotdata-cfg-mesh-test"));

    printf("the three groups compose, and the ids do not collide\n");
    /* the build-time switch in iotdata_node_config.h is what actually proves this: a reused id is a
       duplicate case label and this file would not have compiled. The count is the visible half. */
    CHECK(IOTDATA_NODE_CFG_COUNT == 31, "three down + four common + twelve relay + nine ack + three gateway");
    CHECK(IOTDATA_NODE_CFG_DOWN_TTL_MIN == 0x020, "the downstream hold store at 0x020 -- not mesh: a gateway with no mesh still holds");
    CHECK(IOTDATA_NODE_CFG_MESH_ENABLE == 0x040, "common is based at 0x040");
    CHECK(IOTDATA_NODE_CFG_MESH_HYSTERESIS_DB == 0x051, "relay at 0x05x");
    /* the round counts KEPT the ids their millisecond predecessors held -- the one recorded
       exception to never-reuse, taken while nothing deployed can be confused by it. See the note
       in iotdata_node_config_mesh.h and NOTES_ISSUES.md I.1. */
    CHECK(IOTDATA_NODE_CFG_MESH_PEER_TTL_ROUNDS == 0x042 && IOTDATA_NODE_CFG_MESH_PARENT_MISS_ROUNDS == 0x050, "same ids, new unit");
    CHECK(IOTDATA_NODE_CFG_MESH_REPORT_PEERS_MS == 0x05B, "the neighbour-report cadence paces a FRAME, so it is mesh config and not the app's");
    CHECK(IOTDATA_NODE_CFG_MESH_BEACON_INTERVAL_S == 0x060, "gateway at 0x060");
    CHECK(IOTDATA_NODE_CFG_MESH_ACK_DIRECT == 0x061 && IOTDATA_NODE_CFG_MESH_ACK_DIRECT_SLOT == 0x062, "the direct ack with it: a gateway's, so 0x06x");
    CHECK(IOTDATA_NODE_CFG_MESH_ACK_MAX_RETRIES == 0x070, "the relay ack block at 0x070 -- a SECOND range, not a renumbering of the first");

    printf("\ndefaults are the values the code used as #defines\n");
    iotdata_node_config_defaults();
    CHECK(iotdata_node_config_u8(MESH_PEER_TTL_ROUNDS) == 5u, "peer ttl, in the root's beacon rounds");
    CHECK(iotdata_node_config_u8(MESH_PARENT_MISS_ROUNDS) == 3u, "parent loss, likewise -- see NOTES_ISSUES.md I.1");
    CHECK(iotdata_node_config_u8(MESH_HYSTERESIS_DB) == 10, "hysteresis");
    CHECK(iotdata_node_config_u8(MESH_TTL_INIT) == IOTDATA_MESH_TTL_DEFAULT, "ttl from the protocol's own default");
    CHECK(iotdata_node_config_u16(MESH_BEACON_INTERVAL_S) == 150u, "beacon cadence, in seconds: 150, for a planned network that rarely loses a parent");
    CHECK(iotdata_node_config_bool(MESH_ACK_DIRECT) && iotdata_node_config_u8(MESH_ACK_DIRECT_SLOT) == 0u, "direct acks on, in the slot the station id gives");
    CHECK(iotdata_node_config_bool(MESH_ENABLE), "meshing on by default here; a gateway overrides that");

    printf("\na min/max pair is judged as a pair, whichever half arrives\n");
    CHECK(!set_one(&ds, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MIN_MS, 9000), "a floor above the ceiling is refused");
    CHECK(iotdata_node_config_u32(MESH_REBROADCAST_JITTER_MIN_MS) == 1000u, "and nothing moved");
    CHECK(!set_one(&ds, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MAX_MS, 500), "a ceiling below the floor likewise");
    CHECK(set_one(&ds, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MIN_MS, 2000), "inside the pair is fine");
    CHECK(iotdata_node_config_u32(MESH_REBROADCAST_JITTER_MIN_MS) == 2000u, "and took");
    { /* Moving the pair PAST its old ceiling: the ceiling has to be staged first, because a
         validator runs when its own value is staged and peeks at whatever is staged BY THEN.
         Order-dependent, and the same rule test_node_config.c states for TX_MIN/TX_MAX. */
        iotdata_node_config_update_t up;
        iotdata_node_config_update_begin(&up, false);
        CHECK(!iotdata_node_config_update_stage(&up, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MIN_MS, &(iotdata_node_config_value_t){ .u = 20000 }), "floor first, against the OLD ceiling: refused");
        iotdata_node_config_update_begin(&up, false);
        CHECK(iotdata_node_config_update_stage(&up, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MAX_MS, &(iotdata_node_config_value_t){ .u = 30000 }), "raise the ceiling first");
        CHECK(iotdata_node_config_update_stage(&up, IOTDATA_NODE_CFG_MESH_REBROADCAST_JITTER_MIN_MS, &(iotdata_node_config_value_t){ .u = 20000 }), "and the floor now fits, because the peek sees the STAGED ceiling");
        CHECK(iotdata_node_config_update_commit(&up, &ds, NULL), "committed");
        CHECK(iotdata_node_config_u32(MESH_REBROADCAST_JITTER_MAX_MS) == 30000u && iotdata_node_config_u32(MESH_REBROADCAST_JITTER_MIN_MS) == 20000u, "the pair moved as one");
    }

    printf("\na parent loss later than the peer ttl is unreachable, so it is refused\n");
    CHECK(!set_one(&ds, IOTDATA_NODE_CFG_MESH_PARENT_MISS_ROUNDS, 9), "beyond the ttl: the peer is gone first");
    CHECK(set_one(&ds, IOTDATA_NODE_CFG_MESH_PARENT_MISS_ROUNDS, 4), "below it");

    printf("\nover the air: a CONFIG record stream is the write, and all-or-nothing\n");
    iotdata_node_config_defaults();
    uint8_t wire[IOTDATA_NODE_CONFIG_IMAGE_MAX];
    iotdata_node_partial_t p = { 0 };
    const int n = iotdata_node_config_pack(wire, sizeof(wire), &p);
    CHECK(n > 0 && !p.more, "the whole table fits one frame");
    CHECK(p.total == IOTDATA_NODE_CFG_COUNT && p.chunk == IOTDATA_NODE_CFG_COUNT, "and says so");
    { /* hand-build a two-record write: hysteresis 20, forward-suppress 2 */
        uint8_t w[8];
        size_t at = 0;
        const uint16_t r1 = iotdata_node_config_rec(IOTDATA_NODE_CFG_MESH_HYSTERESIS_DB, IOTDATA_NODE_CONFIG_TYPE_U8);
        w[at++] = (uint8_t)(r1 >> 8);
        w[at++] = (uint8_t)r1;
        w[at++] = 20;
        const uint16_t r2 = iotdata_node_config_rec(IOTDATA_NODE_CFG_MESH_FORWARD_SUPPRESS, IOTDATA_NODE_CONFIG_TYPE_U8);
        w[at++] = (uint8_t)(r2 >> 8);
        w[at++] = (uint8_t)r2;
        w[at++] = 2;
        bool reboot = false;
        CHECK(iotdata_node_config_apply(w, at, &ds, &reboot), "accepted");
        CHECK(iotdata_node_config_u8(MESH_HYSTERESIS_DB) == 20 && iotdata_node_config_u8(MESH_FORWARD_SUPPRESS) == 2, "both took");
        CHECK(!reboot, "neither needs one");
    }
    { /* one bad value rejects the pair: half a radio reconfiguration is how a node is lost */
        uint8_t w[8];
        size_t at = 0;
        const uint16_t r1 = iotdata_node_config_rec(IOTDATA_NODE_CFG_MESH_HYSTERESIS_DB, IOTDATA_NODE_CONFIG_TYPE_U8);
        w[at++] = (uint8_t)(r1 >> 8);
        w[at++] = (uint8_t)r1;
        w[at++] = 5;
        const uint16_t r2 = iotdata_node_config_rec(IOTDATA_NODE_CFG_MESH_FORWARD_SUPPRESS, IOTDATA_NODE_CONFIG_TYPE_U8);
        w[at++] = (uint8_t)(r2 >> 8);
        w[at++] = (uint8_t)r2;
        w[at++] = 99; /* out of range */
        CHECK(!iotdata_node_config_apply(w, at, &ds, NULL), "refused");
        CHECK(iotdata_node_config_u8(MESH_HYSTERESIS_DB) == 20, "and the GOOD one did not take either");
    }
    { /* an id this build does not have is SKIPPED, not fatal: an older node, a newer manager */
        uint8_t w[8];
        size_t at = 0;
        const uint16_t r1 = iotdata_node_config_rec(0x7FF, IOTDATA_NODE_CONFIG_TYPE_U8); /* nothing has this */
        w[at++] = (uint8_t)(r1 >> 8);
        w[at++] = (uint8_t)r1;
        w[at++] = 1;
        const uint16_t r2 = iotdata_node_config_rec(IOTDATA_NODE_CFG_MESH_HYSTERESIS_DB, IOTDATA_NODE_CONFIG_TYPE_U8);
        w[at++] = (uint8_t)(r2 >> 8);
        w[at++] = (uint8_t)r2;
        w[at++] = 7;
        CHECK(iotdata_node_config_apply(w, at, &ds, NULL), "the unknown one is ignored, not a rejection");
        CHECK(iotdata_node_config_u8(MESH_HYSTERESIS_DB) == 7, "and the known one took");
    }

    printf("\nthe downstream hold store: knobs that had setters and no way to reach them\n");
    {
        iotdata_node_down_t ds_down;
        iotdata_node_down_init(&ds_down, NULL);
        CHECK(iotdata_node_down_ttl_ms(&ds_down) == IOTDATA_NODE_DOWN_TTL_MS_DEFAULT, "init leaves the built-in default");
        CHECK(set_one(&ds, IOTDATA_NODE_CFG_DOWN_TTL_MIN, 120), "two hours, in minutes as an operator would say it");
        iotdata_node_config_down_apply(&ds_down);
        CHECK(iotdata_node_down_ttl_ms(&ds_down) == 120u * 60000u, "and the store counts it in ms");
        CHECK(set_one(&ds, IOTDATA_NODE_CFG_DOWN_TTL_MIN, 0), "zero is a real answer: hold it forever");
        iotdata_node_config_down_apply(&ds_down);
        CHECK(iotdata_node_down_ttl_ms(&ds_down) == 0u, "which the store already understood");
    }

    printf("\na backoff ceiling below the base timeout is meaningless, so it is refused\n");
    CHECK(!set_one(&ds, IOTDATA_NODE_CFG_MESH_ACK_BACKOFF_MAX_MS, 1000), "below the 3000ms base");
    CHECK(set_one(&ds, IOTDATA_NODE_CFG_MESH_ACK_BACKOFF_MAX_MS, 45000), "above it");
    CHECK(iotdata_node_config_u8(MESH_ACK_MAX_RETRIES) == 3 && iotdata_node_config_u8(MESH_ACK_EVICT) == 0, "ack defaults are what the module had as #defines");

    printf("\nMESH_ENABLE needs a restart, and says so\n");
    {
        bool reboot = false;
        iotdata_node_config_update_t up;
        iotdata_node_config_update_begin(&up, false);
        (void)iotdata_node_config_update_stage(&up, IOTDATA_NODE_CFG_MESH_ENABLE, &(iotdata_node_config_value_t){ .b = false });
        CHECK(iotdata_node_config_update_commit(&up, &ds, &reboot), "accepted");
        CHECK(reboot, "and asks for one: it gates whether the module comes up at all");
    }

    printf("\npersistence: a restart comes back where it left off\n");
    CHECK(iotdata_node_config_load(&ds), "loaded");
    CHECK(iotdata_node_config_u8(MESH_HYSTERESIS_DB) == 7, "the written value survived");
    CHECK(!iotdata_node_config_bool(MESH_ENABLE), "and so did the one that asked for the restart");

    datastore_close(&ds);
    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
