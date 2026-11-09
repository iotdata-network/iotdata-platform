//
// test_node_config_blocks.c - every CONFIG row block, composed into one table.
//
// The row blocks in iotdata_node_config_*.h are declarative, so there is little behaviour to test in
// any one of them. What IS worth testing is what no single project can: that they all COMPOSE. A
// project takes the handful of blocks it needs, so nothing in a normal build ever puts DDUP next to
// BBOX next to CART -- and an id collision between two blocks a project never combines is invisible
// until the project that combines them appears.
//
// It matters because iotdata_node_config_index() linear-scans for the first row with a matching id. A
// duplicate does not fail, warn, or assert: the first row wins and the second becomes permanently
// unreachable, silently, including over the air.
//
// So this composes ALL of them and checks the table invariants. It is also the test to re-run after
// adding a row, which is when ids get picked.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "iotdata_mesh.h"
#include "iotdata_node_partial.h"
#include "device/d_format.h"                 /* snprintf_inline, the config formatter's return path */
#include "device/d_module_datastore_linux.h" /* datastore_t, which the expansion persists through */
#include "device/d_module_buffers.h"         /* the down store holds frames in a pool */

/* THE DEVICE BLOCKS JOIN HERE WITHOUT ANY DEVICE, two mechanisms making it possible. Their defaults
   defer to the driver's own -- IOTDATA_NODE_CONFIG_LORA_AIR_RATE falls back to LORA_AIR_DATA_RATE_DEFAULT,
   BATT_TYPE to BATTERY_TYPE -- and the #ifndef guards let a host supply values instead; the numbers
   below are stand-ins for a collision check, not the real defaults, and nothing asserts on them.
   IOTDATA_NODE_CONFIG_NO_APPLY then takes the rows and leaves the apply() functions, which are the only
   part that genuinely needs cart_config_t, gpio_num_t and battery_profile_set. */
#define IOTDATA_NODE_CONFIG_NO_APPLY
#define IOTDATA_NODE_CONFIG_LORA_MODULE         0
#define IOTDATA_NODE_CONFIG_LORA_ADDRESS        1
#define IOTDATA_NODE_CONFIG_LORA_NETWORK        0
#define IOTDATA_NODE_CONFIG_LORA_CHANNEL        23
#define IOTDATA_NODE_CONFIG_LORA_TRANSMIT_POWER 22
#define IOTDATA_NODE_CONFIG_LORA_PACKET_RATE    2400
#define IOTDATA_NODE_CONFIG_LORA_PACKET_SIZE    240
#define IOTDATA_NODE_CONFIG_LORA_LBT            0
#define IOTDATA_NODE_CONFIG_LORA_CRYPT          0
#define IOTDATA_NODE_CONFIG_LORA_DEBUG          0
#define IOTDATA_NODE_CONFIG_BATT_TYPE           0
#define IOTDATA_NODE_CONFIG_BATT_CAPACITY_MAH   2600
#define IOTDATA_NODE_CONFIG_BATT_CHARGE_MV_MIN  3200
#define IOTDATA_NODE_CONFIG_BATT_CHARGE_MV_MAX  4170
#define IOTDATA_NODE_CONFIG_BATT_OFFSET_MV      155

/* iotdata_node_down.h for DOWN's ttl bounds, which its block references and does not include. */
#include "iotdata_node_down.h"

/* THE THREE-PASS INCLUDE, as the block headers document it: config.h for the TYPES (every block
   declares validators taking iotdata_node_config_row_t and none includes what defines it), then the
   blocks, then ENTRIES, then config.h to EXPAND, then the blocks AGAIN -- their validator bodies and
   apply() functions sit behind IOTDATA_NODE_CONFIG_EXPANDED and only appear on that second pass. */
#include "iotdata_node_config.h"

#include "iotdata_node_config_down.h"
#include "iotdata_node_config_device_e22900t22.h"
#include "iotdata_node_config_mesh.h"
#include "iotdata_node_config_mqtt.h"
#include "iotdata_node_config_ddup.h"
#include "iotdata_node_config_bbox.h"
#include "iotdata_node_config_stat.h"
#include "iotdata_node_config_cart.h"
#include "iotdata_node_config_device_batt.h"

#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_DOWN(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_LORA(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_RELAY(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_GATEWAY(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MESH_ACK(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_MQTT(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_DDUP(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_BBOX(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_BBOX_FILE(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_STAT_DISPLAY(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_STAT_PUBLISH(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_STAT_PUBLISH_MQTT(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_STAT_DISPLAY_NETW(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_STAT_PUBLISH_NETW(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_CART(X) \
    IOTDATA_NODE_CONFIG_ENTRIES_BATT(X)
#include "iotdata_node_config.h"

/* pass two: the validator bodies (the apply()s are off, see above) */
#include "iotdata_node_config_down.h"
#include "iotdata_node_config_device_e22900t22.h"
#include "iotdata_node_config_mesh.h"
#include "iotdata_node_config_mqtt.h"
#include "iotdata_node_config_ddup.h"
#include "iotdata_node_config_bbox.h"
#include "iotdata_node_config_stat.h"
#include "iotdata_node_config_cart.h"
#include "iotdata_node_config_device_batt.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static void test_ids_are_unique(void) {
    printf("\nno two rows share an id -- the first would shadow the second forever\n");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
        for (int j = i + 1; j < (int)IOTDATA_NODE_CFG_COUNT; j++)
            if (iotdata_node_config_table[i].id == iotdata_node_config_table[j].id) {
                printf("  FAIL: id 0x%03X is both %s and %s\n", (unsigned)iotdata_node_config_table[i].id, iotdata_node_config_table[i].name, iotdata_node_config_table[j].name);
                fails++;
            }
    /* and the lookup really does find each one, which is the thing a collision breaks */
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const int at = iotdata_node_config_index(iotdata_node_config_table[i].id);
        if (at != i) {
            printf("  FAIL: %s (id 0x%03X) resolves to row %d, not %d\n", iotdata_node_config_table[i].name, (unsigned)iotdata_node_config_table[i].id, at, i);
            fails++;
        }
    }
}

static void test_names_are_unique(void) {
    printf("\nnor a name -- it is what an operator types and what a report prints\n");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
        for (int j = i + 1; j < (int)IOTDATA_NODE_CFG_COUNT; j++)
            if (strcmp(iotdata_node_config_table[i].name, iotdata_node_config_table[j].name) == 0) {
                printf("  FAIL: name %s appears at 0x%03X and 0x%03X\n", iotdata_node_config_table[i].name, (unsigned)iotdata_node_config_table[i].id, (unsigned)iotdata_node_config_table[j].id);
                fails++;
            }
}

static void test_defaults_are_in_range(void) {
    printf("\nevery default sits inside its own bounds\n");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const iotdata_node_config_row_t *const r = &iotdata_node_config_table[i];
        /* min/max are lengths for STRING and BLOB, and meaningless for BOOL; FLOAT compares in its
           own member. Everything else is an integer in .u or .i depending on signedness. */
        if (r->type == IOTDATA_NODE_CONFIG_TYPE_STRING || r->type == IOTDATA_NODE_CONFIG_TYPE_BLOB || r->type == IOTDATA_NODE_CONFIG_TYPE_BOOL)
            continue;
        if (r->type == IOTDATA_NODE_CONFIG_TYPE_FLOAT) {
            if (r->dflt.f < r->min.f || r->dflt.f > r->max.f) {
                printf("  FAIL: %s default %g outside [%g, %g]\n", r->name, (double)r->dflt.f, (double)r->min.f, (double)r->max.f);
                fails++;
            }
            continue;
        }
        const bool signed_row = (r->type == IOTDATA_NODE_CONFIG_TYPE_I8 || r->type == IOTDATA_NODE_CONFIG_TYPE_I16 || r->type == IOTDATA_NODE_CONFIG_TYPE_I32 || r->type == IOTDATA_NODE_CONFIG_TYPE_I64);
        if (signed_row ? (r->dflt.i < r->min.i || r->dflt.i > r->max.i) : (r->dflt.u < r->min.u || r->dflt.u > r->max.u)) {
            printf("  FAIL: %s default %lld outside [%lld, %lld]\n", r->name, signed_row ? (long long)r->dflt.i : (long long)r->dflt.u, signed_row ? (long long)r->min.i : (long long)r->min.u,
                   signed_row ? (long long)r->max.i : (long long)r->max.u);
            fails++;
        }
    }
}

static void test_bounds_are_ordered(void) {
    printf("\nand min is never above max\n");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const iotdata_node_config_row_t *const r = &iotdata_node_config_table[i];
        const bool signed_row = (r->type == IOTDATA_NODE_CONFIG_TYPE_I8 || r->type == IOTDATA_NODE_CONFIG_TYPE_I16 || r->type == IOTDATA_NODE_CONFIG_TYPE_I32 || r->type == IOTDATA_NODE_CONFIG_TYPE_I64);
        if (r->type == IOTDATA_NODE_CONFIG_TYPE_BOOL)
            continue;
        if (signed_row ? (r->min.i > r->max.i) : (r->min.u > r->max.u)) {
            printf("  FAIL: %s has min above max\n", r->name);
            fails++;
        }
    }
}

static void test_string_defaults_fit(void) {
    printf("\na string default fits the length its row allows\n");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const iotdata_node_config_row_t *const r = &iotdata_node_config_table[i];
        if (r->type != IOTDATA_NODE_CONFIG_TYPE_STRING)
            continue;
        const char *const v = _iotdata_node_config_value[i].s.p;
        CHECK(v != NULL, "a string row has a value");
        if (v != NULL && strlen(v) > (size_t)r->max.u) {
            printf("  FAIL: %s default is %zu chars, max is %llu\n", r->name, strlen(v), (unsigned long long)r->max.u);
            fails++;
        }
    }
}

static void test_every_row_round_trips(void) {
    printf("\nevery row accepts its own default back through a staged write\n");
    iotdata_node_config_defaults();
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const iotdata_node_config_row_t *const r = &iotdata_node_config_table[i];
        if (r->type == IOTDATA_NODE_CONFIG_TYPE_STRING || r->type == IOTDATA_NODE_CONFIG_TYPE_BLOB)
            continue;
        if ((r->flags & IOTDATA_NODE_CONFIG_FLAG_READONLY) != 0u)
            continue;
        iotdata_node_config_update_t u;
        iotdata_node_config_update_begin(&u, false);
        if (!iotdata_node_config_update_stage(&u, r->id, &r->dflt)) {
            printf("  FAIL: %s refuses its own default\n", r->name);
            fails++;
        }
    }
}

/* Not an assertion -- the realm map, printed so a review can see the allocation at a glance and
   pick the next id without reading nine headers. */
static void show_realms(void) {
    int realms = 0;
    printf("\n  realm  rows  span            example\n");
    for (unsigned realm = 0; realm <= 0xFFu; realm++) {
        int n = 0, lo = 0x1000, hi = -1;
        const char *first = NULL;
        for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
            const unsigned id = iotdata_node_config_table[i].id;
            if ((id >> 4) != realm)
                continue;
            n++;
            if ((int)id < lo) {
                lo = (int)id;
                first = iotdata_node_config_table[i].name;
            }
            if ((int)id > hi)
                hi = (int)id;
        }
        if (n > 0) {
            realms++;
            printf("  0x%02X0  %4d  0x%03X-0x%03X     %s\n", realm, n, (unsigned)lo, (unsigned)hi, first ? first : "");
        }
    }
    printf("\n  %u rows over %d realms\n", (unsigned)IOTDATA_NODE_CFG_COUNT, realms);
}

int main(void) {
    printf("test_node_config_blocks -- %u rows composed from every block\n", (unsigned)IOTDATA_NODE_CFG_COUNT);
    test_ids_are_unique();
    test_names_are_unique();
    test_defaults_are_in_range();
    test_bounds_are_ordered();
    test_string_defaults_fit();
    test_every_row_round_trips();
    show_realms();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
