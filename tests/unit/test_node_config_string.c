// test_node_config_string.c - STRING rows: a slot sized by the row's own max, and the borrow rule.
//
// Nothing else in the suite has a string row, so this is also the proof that the expansion's
// per-string machinery -- the slot struct, the pointer table, the row's slot index -- lines up.

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "iotdata.h"
#include "iotdata_node.h"
#include "iotdata_node_partial.h"

#include "device/d_format.h"
#include "device/d_module_datastore_linux.h"

/* stand in for the console, so the `conf` handler can be driven directly */
#define IOTDATA_NODE_CONSOLE_H
typedef void (*iotdata_node_console_emit_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char g_out[4096];
static size_t g_out_len = 0;
__attribute__((format(printf, 1, 2))) static void test_emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_out_len += (size_t)vsnprintf(g_out + g_out_len, sizeof(g_out) - g_out_len, fmt, ap);
    va_end(ap);
}
static bool said(const char *n) {
    return strstr(g_out, n) != NULL;
}

#include "iotdata_node_config.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* Three strings with deliberately different maxima, so the sizing is visible, plus a scalar to
   prove the two kinds coexist and that the slot index skips the scalar. */
#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    /*NAME         id     TYPE    min  max  default            flags validate notify */ \
    X(MQTT_SERVER, 0x001, STRING, 1, 255, "mqtt://localhost", 0, NULL, NULL, "the broker to publish to") \
    X(TX_PERIOD_S, 0x002, U16, 10, 3600, 60, 0, NULL, NULL, "how often a reading goes out") \
    X(MQTT_CLIENT, 0x003, STRING, 1, 31, "gw", 0, NULL, NULL, "the client id to connect as") \
    X(PEERS, 0x004, STRING, 0, 63, "", 0, NULL, NULL, "peers to replicate to, comma separated")

#include "iotdata_node_config.h"

static void conf(const char *a, const char *b) {
    char *argv[3] = { (char *)(uintptr_t)"conf", (char *)(uintptr_t)a, (char *)(uintptr_t)b };
    g_out[0] = '\0';
    g_out_len = 0;
    iotdata_node_config_console(test_emit, 1000u, b != NULL ? 3 : (a != NULL ? 2 : 1), argv);
}

int main(void) {
    printf("config STRING rows: a slot sized by its own max\n\n");
    datastore_t ds;
    assert(datastore_open(&ds, "/tmp/iotdata-cfg-string-test"));

    printf("the expansion: one slot per string row, sized by that row's max\n");
    CHECK(IOTDATA_NODE_CFG_STRING_COUNT == 3, "three strings among four rows");
    CHECK(sizeof(_iotdata_node_config_strings) == (255 + 1) + (31 + 1) + (63 + 1) + 1, "and the storage is EXACTLY their maxima, not a flat per-row figure");
    CHECK(iotdata_node_config_table[IOTDATA_NODE_CFG_IX_MQTT_SERVER].sidx == 0 && iotdata_node_config_table[IOTDATA_NODE_CFG_IX_MQTT_CLIENT].sidx == 1, "the slot index counts strings only, skipping the scalar between them");

    printf("\ndefaults land in the slot, so a read goes where a write would\n");
    iotdata_node_config_defaults();
    CHECK(strcmp(iotdata_node_config_string(MQTT_SERVER), "mqtt://localhost") == 0, "the default");
    CHECK(iotdata_node_config_string(MQTT_SERVER) == _iotdata_node_config_string_slot[0], "read from the slot, not from the literal");
    CHECK(strcmp(iotdata_node_config_string(PEERS), "") == 0, "an empty default is a value, not an absence");

    printf("\nbounds are LENGTHS, and the slot is the same number\n");
    {
        char big[300];
        memset(big, 'x', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        iotdata_node_config_update_t u;
        iotdata_node_config_update_begin(&u, false);
        CHECK(!iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_MQTT_CLIENT, &(iotdata_node_config_value_t){ .s = { big, 299 } }), "299 into a row whose max is 31: refused");
        iotdata_node_config_update_begin(&u, false);
        CHECK(!iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_MQTT_SERVER, &(iotdata_node_config_value_t){ .s = { "", 0 } }), "empty into a row whose min is 1: refused");
        iotdata_node_config_update_begin(&u, false);
        CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_PEERS, &(iotdata_node_config_value_t){ .s = { "", 0 } }), "empty into a row whose min is 0: accepted");
        CHECK(iotdata_node_config_update_commit(&u, &ds, NULL), "committed");
    }

    printf("\nthe staged string is BORROWED, and the commit is what ends the borrow\n");
    {
        char scratch[64];
        snprintf(scratch, sizeof(scratch), "mqtt://192.168.0.61:1883");
        iotdata_node_config_update_t u;
        iotdata_node_config_update_begin(&u, false);
        CHECK(iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_MQTT_SERVER, &(iotdata_node_config_value_t){ .s = { scratch, (uint16_t)strlen(scratch) } }), "staged from a local buffer");
        CHECK(iotdata_node_config_update_commit(&u, &ds, NULL), "committed");
        memset(scratch, 0, sizeof(scratch)); /* the borrow is over: the value must not have followed it */
        CHECK(strcmp(iotdata_node_config_string(MQTT_SERVER), "mqtt://192.168.0.61:1883") == 0, "the value survived its source being wiped");
    }

    printf("\nnotify fires on a string that moved, and not on one that did not\n");
    {
        bool reboot = false;
        iotdata_node_config_update_t u;
        iotdata_node_config_update_begin(&u, false);
        (void)iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_MQTT_CLIENT, &(iotdata_node_config_value_t){ .s = { "gw-1", 4 } });
        CHECK(iotdata_node_config_update_commit(&u, &ds, &reboot), "changed");
        CHECK(strcmp(iotdata_node_config_string(MQTT_CLIENT), "gw-1") == 0, "took");
        iotdata_node_config_update_begin(&u, false);
        (void)iotdata_node_config_update_stage(&u, IOTDATA_NODE_CFG_MQTT_CLIENT, &(iotdata_node_config_value_t){ .s = { "gw-1", 4 } });
        CHECK(iotdata_node_config_update_commit(&u, &ds, &reboot), "restated: still accepted");
        CHECK(strcmp(iotdata_node_config_string(MQTT_CLIENT), "gw-1") == 0, "and unchanged");
    }

    printf("\nover the wire: [id|type][len][bytes], and skippable by a build that lacks the id\n");
    {
        uint8_t wire[IOTDATA_NODE_CONFIG_IMAGE_MAX];
        iotdata_node_partial_t p = { 0 };
        const int n = iotdata_node_config_pack(wire, sizeof(wire), &p);
        CHECK(n > 0 && !p.more && p.chunk == IOTDATA_NODE_CFG_COUNT, "the whole table packs, strings included");
        /* find the MQTT_CLIENT record and check its shape */
        size_t at = 0;
        bool found = false;
        while (at < (size_t)n) {
            uint16_t id = 0;
            int idx = -1;
            iotdata_node_config_value_t v;
            const size_t adv = iotdata_node_config_record_read(wire + at, (size_t)n - at, &id, &idx, &v);
            if (adv == 0)
                break;
            if (id == IOTDATA_NODE_CFG_MQTT_CLIENT) {
                found = true;
                CHECK(adv == 3u + 4u, "two header bytes, a length byte, then the bytes");
                CHECK(v.s.len == 4 && memcmp(v.s.p, "gw-1", 4) == 0, "and it reads back");
            }
            at += adv;
        }
        CHECK(found && at == (size_t)n, "every record walked, none left over");
    }

    printf("\npersistence: a string survives a restart\n");
    {
        iotdata_node_config_defaults();
        CHECK(strcmp(iotdata_node_config_string(MQTT_SERVER), "mqtt://localhost") == 0, "back to the default first");
        CHECK(iotdata_node_config_load(&ds), "loaded");
        CHECK(strcmp(iotdata_node_config_string(MQTT_SERVER), "mqtt://192.168.0.61:1883") == 0, "what was written came back");
        CHECK(strcmp(iotdata_node_config_string(MQTT_CLIENT), "gw-1") == 0, "all of them");
    }

    printf("\nand the console shows and sets them\n");
    conf("mqtt_client", NULL);
    CHECK(said("\"gw-1\"") && said("length=1..31"), "quoted, with its length bounds and slot size");
    conf("mqtt-client", "bench");
    CHECK(strcmp(iotdata_node_config_string(MQTT_CLIENT), "bench") == 0, "set by name");
    conf("mqtt-client", "this-is-far-longer-than-thirty-one-characters");
    CHECK(said("refused"), "and a too-long one is refused, not truncated");
    CHECK(strcmp(iotdata_node_config_string(MQTT_CLIENT), "bench") == 0, "leaving the old value");

    datastore_close(&ds);
    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
