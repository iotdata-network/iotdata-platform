// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_diagnostics.c - host tests for iotdata_node_diagnostics.h.
//
// Built and run TWICE, with the recorder compiled in and compiled out, because the second build is
// the one that used to be wrong. When the scaffolding lived in each app, turning the recorder off
// meant deleting a block of code and hoping nothing referred to it; the point of the header is that
// the same source builds either way and that the disabled node still answers honestly -- it keeps
// no diagnostics, rather than claiming an empty recorder.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
/* the d_ layer the recorder leans on: RTC_NOINIT_ATTR for the reset-surviving ring, and
   hw_time_ms() for its clock -- platform first, then common, as everywhere else */
#include "device/d_platform_linux.h"
#include "device/d_common.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"

#define IOTDATA_NODE_BLACKBOX_IMPLEMENTATION
#include "iotdata_node_diagnostics.h"
#include "iotdata_node_console_diagnostics.h" /* the `diag` console words moved here */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* where output goes, so a test can read what an operator would see */
static char g_said[64][160];
static int g_lines = 0;
static void say(const char *const line) {
    if (g_lines < 64)
        snprintf(g_said[g_lines++], sizeof(g_said[0]), "%s", line);
}
static void said_reset(void) {
    g_lines = 0;
}
__attribute__((unused)) static bool said_contains(const char *const want) {
    for (int i = 0; i < g_lines; i++)
        if (strstr(g_said[i], want) != NULL)
            return true;
    return false;
}

static int argv_of(const char *const line, char *buf, size_t size, char **out) {
    snprintf(buf, size, "%s", line);
    int argc = 0;
    for (char *p = strtok(buf, " "); p != NULL && argc < 8; p = strtok(NULL, " "))
        out[argc++] = p;
    return argc;
}
static bool console(const char *const line) {
    char buf[128], *v[8];
    const int argc = argv_of(line, buf, sizeof(buf), v);
    return iotdata_node_diagnostics_console(argc, v);
}

int main(void) {
#if IOTDATA_NODE_DIAGNOSTICS
    printf("iotdata_node_diagnostics: the recorder compiled IN\n\n");
#else
    printf("iotdata_node_diagnostics: the recorder compiled OUT\n\n");
#endif
    (void)iotdata_node_diagnostics_emit_set(say);

    printf("starting it\n");
    const bool up = iotdata_node_diagnostics_begin(IOTDATA_NODE_REASON_POWER_ON, true);
#if IOTDATA_NODE_DIAGNOSTICS
    CHECK(up && iotdata_node_diagnostics_ready(), "it started");
    CHECK(iotdata_node_diagnostics_handle() != NULL, "and there is a handle for an app's own records");
#else
    CHECK(!up && !iotdata_node_diagnostics_ready(), "there is nothing to start");
    CHECK(iotdata_node_diagnostics_handle() == NULL, "and no handle to hand out");
#endif

    printf("what it recorded\n");
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_START, 0);
    size_t cursor = 0;
    char rec[160];
    int records = 0;
    while (iotdata_node_diagnostics_pull(&cursor, rec, sizeof(rec)) > 0)
        records++;
#if IOTDATA_NODE_DIAGNOSTICS
    CHECK(records == 2, "the boot it was started with, and the event after it");
#else
    CHECK(records == 0, "nothing, because nothing records");
#endif

    printf("what it tells a node to advertise\n");
    /* whatever the list says, the handler must service exactly that and no more: a key advertised
       and not serviced sends a manager on a wild goose chase, and one serviced but not advertised
       is a command nobody knows to send */
    const unsigned advertised = IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT; /* a variable: the count is 0 in one of the two builds */
    for (unsigned i = 0; i < advertised; i++) {
        const uint8_t subject = iotdata_node_diagnostics_control_actions[i * 2], action = iotdata_node_diagnostics_control_actions[i * 2 + 1];
        if (!iotdata_node_diagnostics_control(subject, action, NULL, 0))
            printf("  FAIL: advertises %02X/%02X but does not service it\n", subject, action), fails++;
        if (subject != IOTDATA_NODE_TLV_DIAGNOSTICS)
            printf("  FAIL: advertises %02X/%02X, which is not a DIAGNOSTICS action\n", subject, action), fails++;
    }
    CHECK(!iotdata_node_diagnostics_control(IOTDATA_NODE_SUBJECT_NODE, IOTDATA_NODE_ACTION_NODE_REBOOT, NULL, 0), "and refuses what is not its own");
#if IOTDATA_NODE_DIAGNOSTICS
    CHECK(IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT == 3, "enable, clear and dump");
    CHECK(IOTDATA_NODE_DIAGNOSTICS_PULL != NULL && IOTDATA_NODE_DIAGNOSTICS_CONTROL != NULL, "hooks a node can install");
#else
    CHECK(IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT == 0, "it advertises nothing");
    /* NULL rather than an inert function: that is how the node layer tells "keeps no diagnostics"
       from "keeps a recorder that happens to be empty" */
    CHECK(IOTDATA_NODE_DIAGNOSTICS_PULL == NULL && IOTDATA_NODE_DIAGNOSTICS_CONTROL == NULL, "and installs no hooks");
#endif

    /* that loop just serviced CLEAR, which is what CLEAR does -- put something back to read */
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_WAKE, 0);
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_START, 0);

    printf("the console words, which have no wire equivalent\n");
    said_reset();
    const bool stat = console("diag stat");
    CHECK(!console("diag sideways"), "an unknown word is not ours");
    CHECK(!console("diag"), "nor a bare verb");
#if IOTDATA_NODE_DIAGNOSTICS
    CHECK(stat && said_contains("enabled="), "`diag stat` says how the store stands");
    said_reset();
    CHECK(console("diag flush") && g_lines > 0, "`diag flush` answers");
    said_reset();
    CHECK(console("diag filter add LC") && said_contains("filter="), "`diag filter` echoes the result");
#else
    CHECK(!stat, "there is no recorder to ask about");
#endif

    printf("a dump drains a chunk at a time\n");
    said_reset();
    iotdata_node_diagnostics_dump_start();
    int passes = 0;
    while (iotdata_node_diagnostics_pump() && passes < 100)
        passes++;
#if IOTDATA_NODE_DIAGNOSTICS
    CHECK(said_contains("dump begin") && said_contains("dump end"), "it begins and it ends");
    CHECK(said_contains("LC,"), "with the records in between");
    /* the pump must stop rather than loop: a dump that never ends is a loop that never runs */
    CHECK(!iotdata_node_diagnostics_pump(), "and does not restart itself");
#else
    CHECK(g_lines == 0 && passes == 0, "nothing to dump, and nothing said about it");
#endif

    printf("clearing it\n");
    iotdata_node_diagnostics_clear();
    cursor = 0;
    CHECK(iotdata_node_diagnostics_pull(&cursor, rec, sizeof(rec)) == 0, "the store is empty");

#if IOTDATA_NODE_DIAGNOSTICS
    printf("a store that never opened\n");
    /* the failure mode this header exists to stop: blackbox_init leaves a NULL backend inside a
       non-NULL handle, and every call below used to reach straight through it */
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_BOOT, 0); /* a record IS there: only the guard hides it */
    _iotdata_node_diagnostics_ready = false;
    said_reset();
    iotdata_node_diagnostics_event(IOTDATA_NODE_BB_LC_STOP, 0);
    iotdata_node_diagnostics_flush();
    iotdata_node_diagnostics_enable(true);
    iotdata_node_diagnostics_clear();
    iotdata_node_diagnostics_tick(1000);
    cursor = 0;
    CHECK(iotdata_node_diagnostics_pull(&cursor, rec, sizeof(rec)) == 0, "nothing is read, though there is something there");
    CHECK(!iotdata_node_diagnostics_control(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAGNOSTICS_CLEAR, NULL, 0), "no command is claimed");
    iotdata_node_diagnostics_stat();
    CHECK(said_contains("unavailable"), "and an operator is told why");
#endif

    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
