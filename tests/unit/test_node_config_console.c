// test_node_config_console.c - the `conf` console command: name resolution, parsing, and refusal.
//
// The console itself is ESP32-only, so this stands in the two things it needs -- the include guard
// that makes the handler appear, and the emit it is handed -- and drives the handler directly.
// Worth doing on the host: name resolution and per-type parsing are where this goes wrong, and
// finding that out over a serial cable is a slow way to learn it.

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata.h"
#include "iotdata_node.h"
#include "iotdata_node_partial.h"

#include "device/d_format.h" /* snprintf_inline, which the console formatter returns through */
#include "device/d_module_datastore_linux.h"

/* Stand in for the console: claim the guard, declare the emit type the handler is written against,
   and capture what it says. The emit being a PARAMETER is what makes this possible at all -- a
   handler that reached for a global would have to be linked against the real console to be tested. */
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
static void out_reset(void) {
    g_out[0] = '\0';
    g_out_len = 0;
}
static bool said(const char *needle) {
    return strstr(g_out, needle) != NULL;
}

#include "iotdata_node_config.h" /* the types */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define IOTDATA_NODE_CONFIG_ENTRIES(X) \
    /*  NAME          id     TYPE min   max    default flags                        validate notify */ \
    X(TX_PERIOD_S, 0x001, U16, 10, 3600, 60, 0, NULL, NULL, "how often a reading goes out") \
    X(TRIM_MV, 0x002, I16, -500, 500, 0, 0, NULL, NULL, "a signed calibration offset") \
    X(RADIO_ON, 0x003, BOOL, 0, 1, 1, 0, NULL, NULL, "whether the radio is powered") \
    X(GAIN, 0x004, FLOAT, 0.0f, 10.0f, 1.5f, 0, NULL, NULL, "the input gain") \
    X(SERIAL_NO, 0x005, U32, 0, 0xFFFFFFFF, 7, IOTDATA_NODE_CONFIG_FLAG_READONLY, NULL, NULL, "the unit's serial number, which is a fact and not a setting") \
    X(DEBUG_MS, 0x006, U16, 1, 60000, 500, IOTDATA_NODE_CONFIG_FLAG_LOCAL, NULL, NULL, "how often the debug line prints") \
    X(CPU_MHZ, 0x007, U8, 10, 160, 80, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, NULL, "the CPU clock")

#include "iotdata_node_config.h" /* expand -- and with the guard set, the console handler too */

static void conf(const char *a, const char *b) {
    char *argv[3] = { (char *)(uintptr_t)"conf", (char *)(uintptr_t)a, (char *)(uintptr_t)b };
    out_reset();
    iotdata_node_config_console(test_emit, 1000u, b != NULL ? 3 : (a != NULL ? 2 : 1), argv);
}

int main(void) {
    printf("the `conf` console command: names, types, and what it refuses\n\n");
    datastore_t ds;
    assert(datastore_open(&ds, "/tmp/iotdata-cfg-console-test"));
    iotdata_node_config_defaults();
    iotdata_node_config_console_attach(&ds);

    printf("with no argument: the whole table\n");
    conf(NULL, NULL);
    CHECK(said("config: 7 entries"), "counts what it has");
    CHECK(said("TX_PERIOD_S") && said("CPU_MHZ"), "and lists every row");
    CHECK(said("ro") && said("local") && said("reboot"), "marking what is special about each");

    printf("\nfinding a row: by name, case-insensitively, by dash, or by id\n");
    conf("TX_PERIOD_S", NULL);
    CHECK(said("TX_PERIOD_S = 60"), "exact name");
    conf("tx_period_s", NULL);
    CHECK(said("TX_PERIOD_S = 60"), "lower case");
    conf("tx-period-s", NULL);
    CHECK(said("TX_PERIOD_S = 60"), "dashes, which is how an operator will type it");
    conf("0x001", NULL);
    CHECK(said("TX_PERIOD_S = 60"), "by hex id");
    conf("1", NULL);
    CHECK(said("TX_PERIOD_S = 60"), "by decimal id");
    conf("nonesuch", NULL);
    CHECK(said("no such entry"), "and says so when there is none");

    printf("\ndetail: the bounds and the flags, so a refusal is predictable before it happens\n");
    conf("cpu_mhz", NULL);
    CHECK(said("id=0x007") && said("type=u8") && said("range=10..160"), "id, type, range");
    CHECK(said("reboot-required"), "and what the flag means, in words");

    printf("\nsetting: parsed against the ROW's type\n");
    conf("tx_period_s", "120");
    CHECK(said("TX_PERIOD_S") && said("120"), "an integer");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 120, "took");
    conf("trim_mv", "-250");
    CHECK(iotdata_node_config_i16(TRIM_MV) == -250, "a negative one, where the type is signed");
    conf("radio_on", "off");
    CHECK(!iotdata_node_config_bool(RADIO_ON), "off/on, not just 0/1");
    conf("radio_on", "yes");
    CHECK(iotdata_node_config_bool(RADIO_ON), "and the other way");
    conf("gain", "2.75");
    CHECK(iotdata_node_config_float(GAIN) > 2.7f && iotdata_node_config_float(GAIN) < 2.8f, "a float");

    printf("\nwhat it refuses, and what it says about it\n");
    conf("trim_mv", "banana");
    CHECK(said("is not a i16"), "a word where a number goes");
    CHECK(iotdata_node_config_i16(TRIM_MV) == -250, "and nothing moved");
    conf("tx_period_s", "-5");
    CHECK(said("is not a u16"), "a negative into an unsigned: caught, not wrapped to 65531");
    conf("tx_period_s", "5");
    CHECK(said("refused") && said("range=10..3600"), "out of range: refused, with the range shown");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 120, "and still what it was");
    conf("serial_no", "9");
    CHECK(said("refused") && said("read-only"), "read-only: refused whoever is asking");
    CHECK(iotdata_node_config_u32(SERIAL_NO) == 7, "unmoved");

    printf("\na console write is LOCAL, which is the whole point of the flag\n");
    conf("debug_ms", "2000");
    CHECK(iotdata_node_config_u16(DEBUG_MS) == 2000, "settable here");
    { /* the same row, off the air */
        uint8_t w[4];
        const uint16_t rec = iotdata_node_config_rec(IOTDATA_NODE_CFG_DEBUG_MS, IOTDATA_NODE_CONFIG_TYPE_U16);
        w[0] = (uint8_t)(rec >> 8);
        w[1] = (uint8_t)rec;
        w[2] = 0x0B;
        w[3] = 0xB8; /* 3000 */
        CHECK(!iotdata_node_config_apply(w, sizeof(w), &ds, NULL), "and refused from the radio");
        CHECK(iotdata_node_config_u16(DEBUG_MS) == 2000, "unmoved");
    }

    printf("\na restart-needed row says so, once it has taken\n");
    conf("cpu_mhz", "40");
    CHECK(iotdata_node_config_u8(CPU_MHZ) == 40 && said("requires restart"), "applied, and announced");

    printf("\nand it persists, because the console attached a store\n");
    iotdata_node_config_defaults();
    CHECK(iotdata_node_config_load(&ds), "loaded");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 120 && iotdata_node_config_u8(CPU_MHZ) == 40, "what the console wrote came back");

    printf("\nwithout a store it still applies, and does not pretend otherwise\n");
    iotdata_node_config_console_attach(NULL);
    conf("tx_period_s", "300");
    CHECK(iotdata_node_config_u16(TX_PERIOD_S) == 300, "applied");
    CHECK(said("not persisted"), "and says it will not survive a restart");

    datastore_close(&ds);
    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
