//
// test_node_console.c - the command console's dispatch, on the host.
//
// The console runs on both platforms: the USB-JTAG driver is behind #if defined(ESP_PLATFORM) and a
// host reads stdin through poll(), so everything below the transport -- tokenising, the built-in and
// application tables, name precedence -- is ordinary code that runs here.
//
// A skeleton over dispatch only. The transport is not tested (there is no terminal in a test run,
// and getc is written never to block precisely so that is harmless), and neither are the built-ins
// that report chip state, since on a host they have nothing to report.
//
// Handlers record what they were given rather than the test capturing stdout: what matters is that
// the right command ran with the right arguments, not how its reply was formatted.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
/* what the five built-in verbs need, now that none of them is compiled out on a host: the line
   and the log level from the d_ layer, and the two version answers the `vers` verb reports */
#include "device/d_platform_linux.h"
#include "device/d_common.h"
#include "device/d_format.h"
#include "device/d_console_linux.h"
#include "iotdata_node_utils.h" /* iotdata_node_reason_reset(), which `stat` reports */
#include "iotdata_node_version.h"
#include "iotdata_node_firmware.h"
#include "iotdata_node_console.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

#define SEEN_ARGV_MAX 8
static struct {
    const char *who;
    int argc;
    uint32_t now_ms;
    char argv[SEEN_ARGV_MAX][32];
} seen;

static void seen_reset(void) {
    memset(&seen, 0, sizeof(seen));
}
static void record(const char *const who, const uint32_t now_ms, const int argc, char **const argv) {
    seen.who = who;
    seen.now_ms = now_ms;
    seen.argc = argc;
    for (int i = 0; i < argc && i < SEEN_ARGV_MAX; i++)
        (void)snprintf(seen.argv[i], sizeof(seen.argv[i]), "%s", argv[i]);
}

static void cmd_alpha(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const uint32_t now_ms, int argc, char **argv) {
    record("alpha", now_ms, argc, argv);
}
static void cmd_beta(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const uint32_t now_ms, int argc, char **argv) {
    record("beta", now_ms, argc, argv);
}
/* tries to take `help`, which dispatch intercepts before either table */
static void cmd_help_impostor(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const uint32_t now_ms, int argc, char **argv) {
    record("impostor", now_ms, argc, argv);
}

/* tries to take `vers`, which the built-in table owns */
static void cmd_vers_impostor(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const uint32_t now_ms, int argc, char **argv) {
    record("impostor", now_ms, argc, argv);
}

static const iotdata_node_console_t app_cmds[] = {
    { "alpha", cmd_alpha, "the first one" },
    { "beta", cmd_beta, NULL }, /* a NULL help must not crash `help` */
    { "help", cmd_help_impostor, "cannot have it" },
    { "vers", cmd_vers_impostor, "cannot have this either" },
};

/* dispatch takes a mutable line because it tokenises in place with strtok, and the pass's clock
   because a handler is never allowed to read one of its own. */
static void feed_at(const uint32_t now_ms, const char *const s) {
    char line[128];
    (void)snprintf(line, sizeof(line), "%s", s);
    seen_reset();
    iotdata_node_console_dispatch(now_ms, line);
}

static void feed(const char *const s) {
    feed_at(1000u, s);
}

/* ------------------------------------------------------------------------------------------- */

static void test_dispatch(void) {
    printf("\na registered command runs, and gets its own name as argv[0]\n");
    feed("alpha");
    CHECK(seen.who != NULL && strcmp(seen.who, "alpha") == 0, "alpha ran");
    CHECK(seen.argc == 1, "argc counts the verb");
    CHECK(strcmp(seen.argv[0], "alpha") == 0, "argv[0] is the verb, as documented");

    feed("beta");
    CHECK(seen.who != NULL && strcmp(seen.who, "beta") == 0, "beta ran");
}

static void test_arguments(void) {
    printf("\narguments split on spaces and tabs, and runs of them collapse\n");
    feed("alpha one two");
    CHECK(seen.argc == 3, "three tokens");
    CHECK(strcmp(seen.argv[1], "one") == 0 && strcmp(seen.argv[2], "two") == 0, "in order");

    feed("alpha\tone  \t two");
    CHECK(seen.argc == 3, "tabs and repeated spaces are one separator");
    CHECK(strcmp(seen.argv[2], "two") == 0, "and the last argument survives them");

    feed("alpha   ");
    CHECK(seen.argc == 1, "trailing whitespace adds no empty argument");
}

static void test_blank_and_unknown(void) {
    printf("\na blank line does nothing at all, and an unknown verb runs nothing\n");
    feed("");
    CHECK(seen.who == NULL, "empty line dispatches nothing");
    feed("   \t ");
    CHECK(seen.who == NULL, "whitespace only dispatches nothing");
    feed("nosuchcommand");
    CHECK(seen.who == NULL, "an unknown verb runs no handler");
    feed("ALPHA");
    CHECK(seen.who == NULL, "dispatch is case sensitive");
}

static void test_help_is_not_overridable(void) {
    printf("\n`help` is intercepted before either table, so it cannot be taken\n");
    /* app_cmds registers a `help` of its own above: dispatch must still run the listing, or an
       application could hide the one command that says what the others are. */
    feed("help");
    CHECK(seen.who == NULL, "the listing ran, not the application's `help`");
}

/* THE BUILT-INS ARE NOT A DEVICE FEATURE, which is what this pins. All four used to be compiled out
   on a host -- so a gateway console answered `unknown command 'vers'` about a question it could
   answer, and the precedence rule below could not be tested at all because the built-in table held
   nothing but its terminator. The platform-shaped half of each verb now lives in
   device/d_console_<platform>.h and the table is the same everywhere. */
static void test_builtins_are_on_every_platform(void) {
    printf("\nthe built-in verbs exist here too, and beat an application of the same name\n");
    const char *const want[] = { "vers", "stat", "logl", "boot" };
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
        bool found = false;
        for (size_t i = 0; iotdata_node_con_builtin[i].name != NULL; i++)
            if (strcmp(iotdata_node_con_builtin[i].name, want[w]) == 0)
                found = true;
        CHECK(found, "the verb is in the built-in table on this platform");
    }

    /* app_cmds registers a `vers` of its own: the built-in has to win, or an application could
       quietly answer the question the fleet asks to identify it. */
    feed("vers");
    CHECK(seen.who == NULL, "the built-in ran, not the application's `vers`");

    /* `logl` is the one with a readable side effect, which makes it the proof that these verbs do
       something here rather than merely existing. */
    feed("logl debug");
    CHECK(platform_log_level_get() == PLATFORM_LOG_DEBUG, "logl moved the platform's log ceiling");
    feed("logl nosuchlevel");
    CHECK(platform_log_level_get() == PLATFORM_LOG_DEBUG, "and a level it cannot parse changes nothing");
    feed("logl info");
    CHECK(platform_log_level_get() == PLATFORM_LOG_INFO, "and it goes back");

    /* NOT COVERED, DELIBERATELY: `boot`. On a host it exits the process, which is the right answer
       and would also end this test run. */
}

/* ------------------------------------------------------------------------------------------- */

/* WHAT THIS IS FOR, and it is not tidiness. A handler used to reach for hw_time_ms() itself, and
   on a relay that cost an afternoon: the dispatch runs after the radio work, so its own reading is
   LATER than the now_ms the pass is using, and `content insert` stamped its deadline a few
   milliseconds ahead of the tick that was to enforce it. now_ms - last_ms then underflows to about
   4.29e9, every timeout is instantly past, and the upload died in the pass that began it.
 *
 * So the one thing worth asserting is that the clock a handler sees is the clock the caller named,
 * exactly -- no rounding, no re-reading, no clock of its own. Nothing else can be checked here,
 * because a handler that reads its own would still pass every test about argv. */
static void test_the_handler_is_given_the_passs_clock(void) {
    printf("\na handler is handed the caller's clock exactly, and never reads one of its own\n");
    feed_at(0u, "alpha");
    CHECK(seen.now_ms == 0u, "zero is a clock like any other, and is passed through unaltered");
    feed_at(1234567u, "alpha");
    CHECK(seen.now_ms == 1234567u, "the value reaches the handler unchanged");
    feed_at(0xFFFFFFFFu, "beta");
    CHECK(seen.now_ms == 0xFFFFFFFFu, "and so does the last millisecond before the wrap");
}

int main(void) {
    printf("test_node_console\n");
    /* registers the application table; on a host this also marks the console open, and the greeting
       it prints is the only output this test does not suppress. */
    iotdata_node_console_init(app_cmds, sizeof(app_cmds) / sizeof(app_cmds[0]));
    test_dispatch();
    test_arguments();
    test_blank_and_unknown();
    test_help_is_not_overridable();
    test_builtins_are_on_every_platform();
    test_the_handler_is_given_the_passs_clock();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
