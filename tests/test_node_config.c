// Config: the table is compiled in, the accessors are type-checked, a write is all-or-nothing.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* the config table's text parser: strtof, strtoll */
#include <string.h>

#include "iotdata.h"
#include "iotdata_node.h"
#include "iotdata_node_partial.h"

#include "device/d_format.h" /* snprintf_inline, which the config formatter returns through */
#include "device/d_module_datastore_linux.h"
#include "iotdata_node_config.h" /* the types, so the handler below can be written */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* A handler that acts, and one that rejects across two entries at once. */
static int g_notified = 0;
static bool on_channel(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *old, const iotdata_node_config_info_t *info) {
    (void)row;
    (void)old;
    (void)info;
    g_notified++;
    return false;
}

/* Cross-entry: the floor must not exceed the ceiling, which can only be judged once both proposed
   values are in hand. This is the whole reason a write is staged before it is applied. */
static bool tx_min_le_max(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);
static bool tx_max_ge_min(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u);

#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    /*  NAME            id     TYPE   min    max     default  flags                       validate     notify        help */ \
    X(TX_PERIOD_S, 0x001, U16, 10, 3600, 60, 0, NULL, NULL, "how often a reading goes out") \
    X(LORA_CHANNEL, 0x002, U8, 0, 83, 23, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, on_channel, "the radio channel") \
    X(TRIM_MV, 0x003, I16, -500, 500, 0, 0, NULL, NULL, "a signed calibration offset") \
    X(SERIAL_NO, 0x004, U32, 0, 0xFFFFFFFF, 7, IOTDATA_NODE_CONFIG_FLAG_READONLY, NULL, NULL, "the unit's serial number, which is a fact and not a setting") \
    X(DEBUG_MS, 0x007, U16, 1, 60000, 500, IOTDATA_NODE_CONFIG_FLAG_LOCAL, NULL, NULL, "how often the debug line prints") \
    X(TX_MIN_S, 0x005, U16, 1, 3600, 20, 0, tx_min_le_max, NULL, "the shortest gap between transmissions") \
    X(TX_MAX_S, 0x006, U16, 1, 3600, 40, 0, tx_max_ge_min, NULL, "the longest")

#include "iotdata_node_config.h"

static bool tx_min_le_max(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u) {
    iotdata_node_config_value_t mx;
    if (v->u < row->min.u || v->u > row->max.u)
        return false;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFG_TX_MAX_S, &mx) && v->u <= mx.u;
}
static bool tx_max_ge_min(const iotdata_node_config_row_t *row, const iotdata_node_config_value_t *v, const struct iotdata_node_config_update *u) {
    iotdata_node_config_value_t mn;
    if (v->u < row->min.u || v->u > row->max.u)
        return false;
    return iotdata_node_config_update_peek(u, IOTDATA_NODE_CFG_TX_MIN_S, &mn) && v->u >= mn.u;
}

int main(void) {
    printf("iotdata_node_config: a compiled-in table, checked at build time\n\n");

    printf("defaults, and reading them back\n");
    /* BEFORE anything has run: the table is already at its defaults, not at zero. An init path
       ordered ahead of the load then reads a plausible value instead of a zero it would act on. */
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 60 && iotdata_node_config_u8(LORA_CHANNEL) == 23, "a read before any init sees the defaults, not zero");
    iotdata_node_config_defaults();
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 60, "a default is the table's");
    CHECK(iotdata_node_config_u8(LORA_CHANNEL) == 23, "for every row");
    CHECK(iotdata_node_config_i16(TRIM_MV) == 0, "signed included");
    /* iotdata_node_config_u16(LORA_CHANNEL) would not COMPILE: the id carries its type as a sibling */
    CHECK(IOTDATA_NODE_CFG_LORA_CHANNEL__TYPE == IOTDATA_NODE_CONFIG_TYPE_U8, "the type sibling is what an accessor asserts on");

    printf("a write is all-or-nothing\n");
    iotdata_node_config_update_t u;
    iotdata_node_config_update_begin(&u, false);
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TX_PERIOD_S, &(iotdata_node_config_value_t){ .u = 120 }), "in bounds");
    CHECK(!iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_LORA_CHANNEL, &(iotdata_node_config_value_t){ .u = 200 }), "out of bounds is refused");
    bool reboot = false;
    CHECK(!iotdata_node_config_update_commit(&u, NULL, &reboot), "and the whole update with it");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 60, "so the GOOD value did not take either: half-applied is worse than none");

    printf("a good one commits, and only what changed is announced\n");
    g_notified = 0;
    iotdata_node_config_update_begin(&u, false);
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TX_PERIOD_S, &(iotdata_node_config_value_t){ .u = 120 }), "staged");
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_LORA_CHANNEL, &(iotdata_node_config_value_t){ .u = 23 }), "staged, but unchanged");
    CHECK(iotdata_node_config_update_commit(&u, NULL, &reboot), "committed");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 120, "the new value is live");
    CHECK(g_notified == 0, "and a value written to what it already was announces nothing");
    CHECK(!reboot, "nor asks for a reboot it does not need");

    printf("a changed value announces, and the reboot flag is the table's to insist on\n");
    iotdata_node_config_update_begin(&u, false);
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_LORA_CHANNEL, &(iotdata_node_config_value_t){ .u = 42 }), "staged");
    CHECK(iotdata_node_config_update_commit(&u, NULL, &reboot), "committed");
    CHECK(g_notified == 1, "the handler was told, once");
    CHECK(reboot, "and the row's flag asked for a restart even though the handler did not");

    printf("read-only is reportable, not writable\n");
    iotdata_node_config_update_begin(&u, false);
    CHECK(!iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_SERIAL_NO, &(iotdata_node_config_value_t){ .u = 9 }), "refused");
    CHECK(iotdata_node_config_u32(SERIAL_NO) == 7, "and unchanged");

    printf("a pair that must agree is judged together, which is why staging exists\n");
    iotdata_node_config_update_begin(&u, false);
    /* each alone is in bounds; together they are nonsense, and only the SET can see that */
    CHECK(!iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TX_MIN_S, &(iotdata_node_config_value_t){ .u = 100 }), "a floor of 100 against a ceiling of 40 is refused");
    CHECK(u.rejected, "and the whole update is poisoned, not just that entry");
    iotdata_node_config_update_begin(&u, false);
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TX_MAX_S, &(iotdata_node_config_value_t){ .u = 200 }), "raise the ceiling first");
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TX_MIN_S, &(iotdata_node_config_value_t){ .u = 100 }), "and the floor now fits, because the peek sees the STAGED ceiling");
    CHECK(iotdata_node_config_update_commit(&u, NULL, &reboot), "committed together");
    CHECK(iotdata_node_config_u16(TX_MIN_S) == 100 && iotdata_node_config_u16(TX_MAX_S) == 200, "both live");

    printf("the image is self-describing, and survives the table changing under it\n");
    datastore_t ds;
    CHECK(datastore_open(&ds, "/tmp/iotdata-cfg-test"), "store opened");
    CHECK(iotdata_node_config_save(&ds), "saved");
    iotdata_node_config_defaults();
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 60, "back to defaults");
    CHECK(iotdata_node_config_load(&ds), "loaded");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 120, "and the written values came back");
    CHECK(iotdata_node_config_u8(LORA_CHANNEL) == 42, "all of them");

    /* a record for an id this build does not have is SKIPPED, and everything after it still reads:
       that is what lets a firmware change drop a config without wiping the rest */
    uint8_t img[32];
    size_t at = 0;
    const uint16_t gone = iotdata_node_config_rec(0x7FF, IOTDATA_NODE_CONFIG_TYPE_U16);
    img[at++] = (uint8_t)(gone >> 8);
    img[at++] = (uint8_t)gone;
    img[at++] = 0x00;
    img[at++] = 0x05;
    at += iotdata_node_config_encode(IOTDATA_NODE_CFG_IX_TRIM_MV, img + at, sizeof(img) - at);
    iotdata_node_config_defaults();
    for (size_t i = 0; i < at;) {
        const size_t n = iotdata_node_config_decode(img + i, at - i);
        if (n == 0)
            break;
        i += n;
    }
    CHECK(iotdata_node_config_i16(TRIM_MV) == 0, "a retired id is skipped");

    /* sign extension: a negative i16 must come back negative, not as 65036 */
    iotdata_node_config_update_begin(&u, false);
    CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_TRIM_MV, &(iotdata_node_config_value_t){ .i = -500 }), "staged");
    CHECK(iotdata_node_config_update_commit(&u, &ds, NULL), "committed");
    iotdata_node_config_defaults();
    CHECK(iotdata_node_config_load(&ds), "loaded");
    CHECK(iotdata_node_config_i16(TRIM_MV) == -500, "a negative value survives the round trip");

    printf("\nFLAG_LOCAL: settable where a person is, refused off the air\n");
    iotdata_node_config_defaults();
    { /* locally: an ordinary write */
        iotdata_node_config_update_t lu;
        iotdata_node_config_update_begin(&lu, false);
        CHECK(iotdata_node_config_update_stage(&lu, IOTDATA_NODE_CFG_DEBUG_MS, &(iotdata_node_config_value_t){ .u = 2000 }), "console, file, command line: yes");
        CHECK(iotdata_node_config_update_commit(&lu, &ds, NULL), "committed");
        CHECK(iotdata_node_config_u16(DEBUG_MS) == 2000, "and took");
    }
    { /* the same id, the same value, off the radio */
        iotdata_node_config_update_t ru;
        iotdata_node_config_update_begin(&ru, true);
        CHECK(!iotdata_node_config_update_stage(&ru, IOTDATA_NODE_CFG_DEBUG_MS, &(iotdata_node_config_value_t){ .u = 3000 }), "over the air: no");
        CHECK(!iotdata_node_config_update_commit(&ru, &ds, NULL), "and it poisons the update, like any refusal");
        CHECK(iotdata_node_config_u16(DEBUG_MS) == 2000, "nothing moved");
    }
    { /* a remote batch containing one: all or nothing still holds */
        iotdata_node_config_update_t ru;
        iotdata_node_config_update_begin(&ru, true);
        (void)iotdata_node_config_update_stage(&ru, IOTDATA_NODE_CFG_TX_PERIOD_S, &(iotdata_node_config_value_t){ .u = 90 });
        (void)iotdata_node_config_update_stage(&ru, IOTDATA_NODE_CFG_DEBUG_MS, &(iotdata_node_config_value_t){ .u = 3000 });
        CHECK(!iotdata_node_config_update_commit(&ru, &ds, NULL), "one local row refuses the whole remote batch");
        CHECK(iotdata_node_config_u16(TX_PERIOD_S) != 90, "so the writable one did not take either");
    }
    /* and it is still REPORTED: a manager that cannot read it cannot say what the node is doing */
    {
        uint8_t wire[IOTDATA_NODE_CONFIG_IMAGE_MAX];
        iotdata_node_partial_t p = { 0 };
        const int n = iotdata_node_config_pack(wire, sizeof(wire), &p);
        CHECK(n > 0 && p.chunk == IOTDATA_NODE_CFG_COUNT, "every row is in the report, local ones included");
    }

    printf("\na record read without the table: all a gateway has of another node's rows\n");
    {
        const uint8_t rec[] = {
            0x7F, 0xE5, 0x00, 0x00, 0x01, 0xF4, /* 0x7FE u32 = 500: no such row in this build */
            0x7F, 0xD2, 0xFB,                   /* 0x7FD i8 = -5 */
            0x7F, 0xCA, 0x02, 'h',  'i',        /* 0x7FC string = "hi" */
        };
        uint16_t id = 0;
        uint8_t type = 0;
        int i = 0;
        iotdata_node_config_value_t v;
        CHECK(iotdata_node_config_record_peek(rec, sizeof(rec), &id, &type, &v) == 6 && id == 0x7FE && type == IOTDATA_NODE_CONFIG_TYPE_U32 && v.u == 500, "a row this build does not have is still read");
        CHECK(iotdata_node_config_record_read(rec, sizeof(rec), &id, &i, &v) == 6 && i == -1, "where record_read skips it, by the same length");
        CHECK(iotdata_node_config_record_peek(rec + 6, 3, &id, &type, &v) == 3 && v.i == -5, "signed, sign-extended");
        CHECK(iotdata_node_config_record_peek(rec + 9, 5, &id, &type, &v) == 5 && v.s.len == 2 && memcmp(v.s.p, "hi", 2) == 0, "a string by its own length, borrowed");
        CHECK(iotdata_node_config_record_peek(rec, 5, &id, &type, &v) == 0, "and a truncated record is not read at all");
    }

    datastore_close(&ds);
    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
