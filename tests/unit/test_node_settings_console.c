// test_node_settings_console.c - the `node` console bridge: reading, writing, and the default.
//
// The thing worth pinning is that EVERY SETTING IS A VALUE. There is no unset, no revert and no
// fallback: seeding fills the block, persistence restores it, a write replaces it. What the command
// adds is the one thing the persisted blob cannot show -- what this build's default WAS, so that
// putting a value back is a thing an operator can actually do, by stating it.

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata.h"
#include "iotdata_node.h"
#include "iotdata_node_partial.h"

#include "device/d_format.h"
#include "device/d_module_datastore_linux.h"

/* stand in for the console */
#define IOTDATA_NODE_CONSOLE_H
typedef void (*iotdata_node_console_emit_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char g_out[8192];
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

#include "iotdata_node_state.h"
#include "iotdata_node_settings.h"
#include "iotdata_node_console_settings.h" /* the `node` console words moved here */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define DERIVED_STATION 0x537

static void node(const char *a, const char *b, const char *c, const char *d) {
    char *argv[5] = { (char *)(uintptr_t)"node", (char *)(uintptr_t)a, (char *)(uintptr_t)b, (char *)(uintptr_t)c, (char *)(uintptr_t)d };
    const int argc = (d != NULL) ? 5 : (c != NULL) ? 4 : (b != NULL) ? 3 : (a != NULL) ? 2 : 1;
    g_out[0] = '\0';
    g_out_len = 0;
    iotdata_node_settings_console(test_emit, 1000u, argc, argv);
}

int main(void) {
    printf("the `node` console command: settings, and where each one came from\n\n");
    datastore_t ds;
    assert(datastore_open(&ds, "/tmp/iotdata-node-console-test"));
    static iotdata_node_state_t state;
    static iotdata_node_settings_t settings;
    iotdata_node_state_init(&state, &ds, "state");
    iotdata_node_settings_defaults(&settings, DERIVED_STATION);
    assert(iotdata_node_settings_attach(&settings, &state));
    iotdata_node_settings_console_attach(&settings, &state);

    printf("as built: every value stated, and nothing flagged because nothing differs\n");
    node(NULL, NULL, NULL, NULL);
    CHECK(said("station        1335"), "the station is the one the hardware derived -- seeded, not deferred");
    CHECK(!said("(default"), "and no default is printed, because the node IS running as built");
    CHECK(said("version") && said("status"), "every reportable type is listed");
    CHECK(!said("discriminator"), "and only the reportable ones -- a type nobody can ask for has no schedule");
    CHECK(!said("(no record"), "each of which has a record: seeding is dense");

    printf("\nwriting one shows what it was, which is the only way back to it\n");
    node("station", "10", NULL, NULL);
    CHECK(said("station") && said("10") && said("default 1335"), "the write answers with what is true, and with what it was");
    CHECK(iotdata_node_settings_station(&settings) == 10, "and the reader agrees");
    node("station", "1335", NULL, NULL);
    CHECK(iotdata_node_settings_station(&settings) == DERIVED_STATION, "stating the default is how you get back to it");
    node("station", "unset", NULL, NULL);
    CHECK(said("is not a number"), "and 'unset' is not a value, so it is not a setting");
    CHECK(iotdata_node_settings_station(&settings) == DERIVED_STATION, "nothing having changed");

    printf("\nthe protocol's own rule, not just the field's width\n");
    node("station", "0", NULL, NULL);
    CHECK(said("not assignable"), "0 is not a station");
    node("station", "4095", NULL, NULL);
    CHECK(said("not assignable"), "nor is the broadcast id");
    CHECK(iotdata_node_settings_station(&settings) == DERIVED_STATION, "and neither took");

    printf("\nthe window and the interval, each named with the unit it is in\n");
    node("window-s", "30", NULL, NULL);
    CHECK(iotdata_node_settings_window_s(&settings) == 30, "window-s took");
    node("window_s", "45", NULL, NULL);
    CHECK(iotdata_node_settings_window_s(&settings) == 45, "and the underscore spelling is the same field");
    node("window", "60", NULL, NULL);
    CHECK(said("try `node`"), "the bare name is NOT the field: the unit is part of what it is called");
    CHECK(iotdata_node_settings_window_s(&settings) == 45, "and nothing moved");
    node("interval-m", "90", NULL, NULL);
    CHECK(iotdata_node_settings_interval_s(&settings) == 90u * 60u, "interval-m is MINUTES, and the reader answers in seconds");

    printf("\nboth ends of a slipped digit, refused rather than clamped\n");
    node("window-s", "7200", NULL, NULL);
    CHECK(said("is not a window"), "past the cap: an hour is already past any real duty cycle");
    node("window-s", "0", NULL, NULL);
    CHECK(said("is not a window"), "and 0 is not a decision here -- on the wire it reads as the default, which this node would not be awake for");
    CHECK(iotdata_node_settings_window_s(&settings) == 45, "neither took");
    node("interval-m", "20000", NULL, NULL);
    CHECK(said("past the cap"), "a week is the interval's limit, for the opposite reason");
    CHECK(iotdata_node_settings_interval_s(&settings) == 90u * 60u, "and that did not take either");

    printf("\nthe same caps over the air, where there is nobody to read the refusal\n");
    {
        /* 5000 is the millisecond window a node persisted before the unit changed, and what a
           manager that has not been updated would still write. Per FIELD, not per record: the
           triple arrives whole, and one bad value must not cost the other two. */
        uint8_t kvbuf[32], r[IOTDATA_NODE_SETTINGS_RECEIVE_SIZE];
        iotdata_kvr_t kv;
        iotdata_kvr_init(&kv, kvbuf, sizeof(kvbuf));
        iotdata_node_settings_put_u16(r, 0, 5000);
        iotdata_node_settings_put_u16(r, 2, 120);
        iotdata_node_settings_put_u16(r, 4, 0);
        iotdata_kvr_add(&kv, IOTDATA_NODE_SETTINGS_RECEIVE, r, (uint8_t)sizeof(r));
        CHECK(iotdata_node_settings_apply(&settings, kvbuf, kv.len), "the write took something");
        CHECK(iotdata_node_settings_window_s(&settings) == 45, "but not the window past the cap");
        CHECK(iotdata_node_settings_interval_s(&settings) == 120u * 60u, "and the interval beside it still landed");
    }

    printf("\na report schedule, by type name\n");
    node("report", "STATUS", "period-s", "3600");
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_STATUS) == 3600, "the period took");
    node("report", "STATUS", "at-startup", "on");
    CHECK(iotdata_node_settings_at_startup(&settings, IOTDATA_NODE_TLV_STATUS), "at-startup too");
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_STATUS) == 3600, "and changing one did not clear the other");
    node("report", "STATUS", "period_s", "off");
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_STATUS) == 0, "'off' means NEVER, which is a value -- the flag carries it, not a sentinel");
    CHECK(iotdata_node_settings_at_startup(&settings, IOTDATA_NODE_TLV_STATUS), "and still says nothing about at-startup");
    node("report", "STATUS", "period", "900");
    CHECK(said("expected"), "and the bare name is not the word here either: the unit is part of it");
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_STATUS) == 0, "so nothing moved");
    node("report", "nonesuch", "period-s", "10");
    CHECK(said("not a reportable type"), "an unknown subject is named");

    printf("\nthe vocabulary as a whole: a key that names a quantity names its unit\n");
    {
        /* Asserted over the GENERATED list rather than name by name, because the rule is the point:
           seconds and minutes are what the wire and the operator see, milliseconds are what the code
           works in, and the only thing that keeps the two from being confused is that every stated
           quantity says which it is. Anything added later that measures a time and does not say in
           what fails here. */
        char key[IOTDATA_NODE_SETTINGS_KEY_MAX];
        int named = 0;
        for (int i = 0; i < IOTDATA_NODE_SETTINGS_KEY_COUNT; i++)
            /* a subject nobody can report has no row at all, which is not a naming fault */
            if (iotdata_node_settings_key_name(i, key, sizeof(key))) {
                named++;
                const size_t n = strlen(key);
                /* a station is a name and at-startup is a yes or no: neither is a quantity */
                const bool unitless = strcmp(key, "node-station") == 0 || (n > 11 && strcmp(key + n - 11, "-at-startup") == 0);
                const bool suffixed = n > 2 && key[n - 2] == '-' && (key[n - 1] == 's' || key[n - 1] == 'm');
                if (!unitless && !suffixed) {
                    printf("  FAIL: '%s' names a quantity and not its unit (line %d)\n", key, __LINE__);
                    fails++;
                }
            }
        CHECK(named > IOTDATA_NODE_SETTINGS_SCALAR_COUNT, "and there were rows to check");
        CHECK(iotdata_node_settings_key_name(1, key, sizeof(key)) && strcmp(key, "node-window-s") == 0, "the window row is spelled in seconds");
        CHECK(iotdata_node_settings_key_index("node-report-status-period-s") >= 0, "a report cadence says seconds too");
        CHECK(iotdata_node_settings_key_index("node-report-status-period") < 0, "and the unsuffixed spelling is not a key: a unit change breaks it, loudly");
    }

    printf("\nit persists, because the state block is where settings live\n");
    node("station", "0x123", NULL, NULL);
    CHECK(iotdata_node_settings_station(&settings) == 0x123, "hex accepted");
    {
        static iotdata_node_state_t s2;
        static iotdata_node_settings_t back;
        iotdata_node_state_init(&s2, &ds, "state");
        iotdata_node_settings_defaults(&back, DERIVED_STATION);
        assert(iotdata_node_settings_attach(&back, &s2));
        (void)iotdata_node_state_load(&s2);
        CHECK(iotdata_node_settings_station(&back) == 0x123, "and came back after a restart");
        CHECK(iotdata_node_settings_period_s(&back, IOTDATA_NODE_TLV_STATUS) == 0, "with the rest of it");
    }

    printf("\nMANY BLOCKS ON ONE NODE: its own tag, and its own defaults to go back to\n");
    {
        /* what a simulator pretending to be a fleet needs, and what the plain attach() cannot do:
           one tag would register both under the same name, and one frozen defaults image would
           hand BOTH blocks the last one's values on a restore. */
        static iotdata_node_state_t s3;
        static iotdata_node_settings_t a, b, a_dflt, b_dflt;
        iotdata_node_state_init(&s3, &ds, "fleet");
        iotdata_node_settings_defaults(&a, 0x101);
        iotdata_node_settings_defaults(&b, 0x202);
        CHECK(iotdata_node_settings_attach_as(&a, &s3, 0x53530101u, &a_dflt), "one");
        CHECK(iotdata_node_settings_attach_as(&b, &s3, 0x53530202u, &b_dflt), "and another");
        CHECK(a_dflt.station == 0x101 && b_dflt.station == 0x202, "each froze ITS OWN defaults, not the last one's");

        a.station = 0x111;
        b.station = 0x222;
        CHECK(iotdata_node_settings_commit(&a, &s3), "written");
        {
            static iotdata_node_state_t s4;
            static iotdata_node_settings_t a2, b2, a2_dflt, b2_dflt;
            iotdata_node_state_init(&s4, &ds, "fleet");
            iotdata_node_settings_defaults(&a2, 0x101);
            iotdata_node_settings_defaults(&b2, 0x202);
            CHECK(iotdata_node_settings_attach_as(&a2, &s4, 0x53530101u, &a2_dflt), "re-attached");
            CHECK(iotdata_node_settings_attach_as(&b2, &s4, 0x53530202u, &b2_dflt), "both");
            (void)iotdata_node_state_load(&s4);
            CHECK(iotdata_node_settings_station(&a2) == 0x111, "each came back as itself");
            CHECK(iotdata_node_settings_station(&b2) == 0x222, "and not as the other");
        }
    }

    printf("\nand a node with nowhere to write says so rather than pretending\n");
    iotdata_node_settings_console_attach(&settings, NULL);
    node(NULL, NULL, NULL, NULL);
    CHECK(said("NOT PERSISTED"), "the listing warns");
    node("station", "11", NULL, NULL);
    CHECK(said("not persisted"), "and so does a write");
    CHECK(iotdata_node_settings_station(&settings) == 11, "which still applied");

    datastore_close(&ds);
    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
