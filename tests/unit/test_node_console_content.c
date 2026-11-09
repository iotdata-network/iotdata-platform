//
// test_node_console_content.c - the store's console and CONTROL words, on the host.
//
// The half of the content work that a device was going to be needed for, done here instead. Every
// piece of it is ordinary code: the console's line assembly and sink run on a host (the USB-JTAG
// driver is the only thing behind #if defined(ESP_PLATFORM)), the framing is text, base64 is
// arithmetic, and the store underneath is the RAM backend. What is left for a device is SPIFFS, and
// nothing else.
//
// THIS ONE DOES CAPTURE STDOUT, unlike test_node_console.c, and the difference is the point. There
// a reply was a reply and what mattered was that the right handler ran. Here the reply IS the
// protocol -- `d <offset> <base64>` is what esp32-tool parses -- so a test that trusted the
// handler to have meant well would not be testing the thing that can break.
//
// THE SINK IS FED DIRECTLY rather than through stdin. iotdata_node_console_tick() reads a real
// descriptor, and a test that pushed bytes into stdin would be testing the read rather than the
// framing; the sink is a plain function taking a buffer, so the awkward cases -- a line split
// across two reads, a transfer that ends exactly on a buffer boundary, a command arriving behind
// the last data line -- are reachable exactly and on purpose. The one case that needs the console
// itself is that a verb can install a sink and the data behind it in the same read, and that goes
// through poll's own loop.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "device/d_format.h"
/* CRC-64 LIVES IN d_common.h, which needs a platform under it for its timer and delay helpers --
   so the host platform goes in first, exactly as the gateway does it. This suite calls neither;
   it is here for the checksum iotdata_node_content.h sums an upload with. */
#include "d_platform_linux.h"
#include "d_common.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"
#include "d_console_linux.h"      /* the console line, and the platform half of its five verbs */
#include "iotdata_node_utils.h"   /* iotdata_node_reason_reset(), which `stat` reports */
#include "iotdata_node_version.h" /* which `vers` reports */
#include "iotdata_node_firmware.h"
#include "iotdata_node_console.h"
#include "d_module_filestore.h" /* the seam: content.h is written against it */
#include "iotdata_node_content.h"
#include "d_module_filestore_ram.h"
#include "iotdata_node_content_manifest.h" /* the offers half: content_control.h speaks it too */
#include "d_module_otaslot.h"
#include "iotdata_node_content_ota.h"
#include "iotdata_node_content_control.h"
#include "iotdata_node_console_content.h"

/* Every id in this suite is 0x053xxx, so this node is station 0x053 -- §4 puts the originating
   station in an id's top twelve bits, and the offers table holds both sides of that rule. */
#define MY_STATION 0x053u

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */
/* stdout, caught in a file and read back. fd 1 and not the FILE*, so that printf's buffering is
   not something every assertion has to reason about. */

#define CAP_MAX 65536
static char cap[CAP_MAX];
static int cap_saved = -1;
static FILE *cap_file = NULL;

static void cap_begin(void) {
    fflush(stdout);
    cap_file = tmpfile();
    cap_saved = dup(1);
    (void)dup2(fileno(cap_file), 1);
}

static const char *cap_end(void) {
    fflush(stdout);
    (void)dup2(cap_saved, 1);
    (void)close(cap_saved);
    rewind(cap_file);
    const size_t n = fread(cap, 1u, sizeof(cap) - 1u, cap_file);
    cap[n] = '\0';
    (void)fclose(cap_file);
    cap_file = NULL;
    return cap;
}

/* ------------------------------------------------------------------------------------------- */

static char said[64][IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX];
static int said_n = 0;

static void say(const char *const line) {
    if (said_n < (int)(sizeof(said) / sizeof(said[0])))
        snprintf(said[said_n++], sizeof(said[0]), "%s", line);
}

static void said_reset(void) {
    said_n = 0;
    memset(said, 0, sizeof(said));
}

static bool said_has(const char *const needle) {
    for (int i = 0; i < said_n; i++)
        if (strstr(said[i], needle) != NULL)
            return true;
    return false;
}

/* ------------------------------------------------------------------------------------------- */

static filestore_ram_t fs;
static iotdata_node_content_t store;
static uint32_t clock_ms = 1000u;

/* The offers table, and a stand-in for the application's down path: an advertisement has to reach a
   radio, and what the handler can be held to is that it tried, with the right audience and the
   right bytes. */
static iotdata_node_content_manifests_t offers;
typedef struct {
    int calls;
    uint16_t audience;
    uint8_t val[128];
    size_t len;
    bool fail;
} aired_t;
static aired_t aired;

static bool aired_cb(__attribute__((unused)) void *ctx, const uint16_t audience, const uint8_t *const tlv, const size_t len) {
    aired.calls++;
    aired.audience = audience;
    aired.len = (len < sizeof(aired.val)) ? len : sizeof(aired.val);
    memcpy(aired.val, tlv, aired.len);
    return !aired.fail;
}

static void rig_offers(const bool can_originate) {
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    iotdata_node_content_control_offers_init(&offers, can_originate ? aired_cb : NULL, NULL);
    aired = (aired_t){ 0 };
}

static void rig(void) {
    const d_filestore_t *const be = filestore_ram_init(&fs);
    iotdata_node_content_init(&store, be);
    (void)iotdata_node_content_begin(&store, clock_ms); /* the tick does nothing until it has */
    iotdata_node_console_content_init(&store);
    iotdata_node_content_control_init(&store, NULL);
    rig_offers(true);
    (void)iotdata_node_content_emit_set(say);
    iotdata_node_console_sink_clear();
    said_reset();
}

/* One verb, as the console would dispatch it: strtok over a mutable copy, exactly as
   iotdata_node_console_dispatch does, so argv looks the way the real one looks. */
static bool verb(const char *const cmdline) {
    char line[160];
    char *argv[IOTDATA_NODE_CONSOLE_ARGC_MAX];
    int argc = 0;
    snprintf(line, sizeof(line), "%s", cmdline);
    for (char *tok = strtok(line, " \t"); tok != NULL && argc < IOTDATA_NODE_CONSOLE_ARGC_MAX; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    return iotdata_node_console_content(clock_ms, argc, argv);
}

static size_t sink(const char *const s) {
    return _iotdata_node_console_content_sink(&_iotdata_node_console_content, clock_ms, (const uint8_t *)s, strlen(s));
}

/* A `d <offset> <base64>` line for one slice, built the way the host will build it. */
static void line_for(char *const out, const size_t size, const uint8_t *const data, const uint32_t off, const uint32_t len) {
    char b64[IOTDATA_NODE_CONSOLE_CONTENT_B64 + 1];
    (void)_iotdata_node_console_content_encode(data + off, len, b64);
    snprintf(out, size, "d %u %s\n", (unsigned)off, b64);
}

static void upload(const uint8_t *const data, const uint32_t len, const uint32_t skip_line) {
    char l[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    uint32_t n = 0;
    for (uint32_t off = 0; off < len; off += IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD, n++) {
        const uint32_t want = ((len - off) < IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD) ? (len - off) : IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD;
        if (n == skip_line)
            continue;
        line_for(l, sizeof(l), data, off, want);
        (void)sink(l);
    }
}

static iotdata_node_content_event_t tick(void) {
    const iotdata_node_content_event_t ev = iotdata_node_content_tick(&store, clock_ms);
    iotdata_node_console_content_tick(clock_ms);
    return ev;
}

static void fill(uint8_t *const buf, const uint32_t len, const uint8_t seed) {
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + (i * 31u) + (i >> 5));
}

/* ------------------------------------------------------------------------------------------- */

static void test_base64(void) {
    printf("\nbase64 round-trips, and refuses rather than guesses\n");
    uint8_t in[200], out[256];
    char b64[512];
    fill(in, sizeof(in), 7u);
    bool all = true;
    for (size_t len = 0; len <= sizeof(in); len++) {
        const size_t n = _iotdata_node_console_content_encode(in, len, b64);
        if (len == 0u) {
            all = all && (n == 0u) && (b64[0] == '\0');
            continue;
        }
        all = all && (n == ((len + 2u) / 3u) * 4u);
        const int back = _iotdata_node_console_content_decode(b64, out, sizeof(out));
        all = all && (back == (int)len) && (memcmp(in, out, len) == 0);
    }
    CHECK(all, "every length from 1 to 200 comes back exactly");

    /* The one encoding the framing depends on: 192 bytes is 256 characters with no padding, which
       is why that payload size was chosen and what a host will assume. */
    CHECK(_iotdata_node_console_content_encode(in, IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD, b64) == IOTDATA_NODE_CONSOLE_CONTENT_B64, "a full line is exactly B64 characters");
    CHECK(strchr(b64, '=') == NULL, "and needs no padding");

    CHECK(_iotdata_node_console_content_decode("", out, sizeof(out)) < 0, "an empty string is not a line");
    CHECK(_iotdata_node_console_content_decode("QUJD", out, sizeof(out)) == 3, "QUJD is ABC");
    CHECK(_iotdata_node_console_content_decode("QUJDR", out, sizeof(out)) < 0, "a length that is not a multiple of four");
    CHECK(_iotdata_node_console_content_decode("QU!D", out, sizeof(out)) < 0, "a character outside the alphabet");
    CHECK(_iotdata_node_console_content_decode("QU=D", out, sizeof(out)) < 0, "a pad with something after it in its own group");
    CHECK(_iotdata_node_console_content_decode("QUJDQUJD", out, sizeof(out)) == 6, "two groups");
    CHECK(_iotdata_node_console_content_decode("QU==QUJD", out, sizeof(out)) < 0, "padding anywhere but the last group");
    CHECK(_iotdata_node_console_content_decode("QUJD", out, 2u) < 0, "and it refuses rather than overrun what it was given");
}

/* ------------------------------------------------------------------------------------------- */

static void test_insert(void) {
    printf("\nan upload arrives, is summed, and lands at its real id\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 3u);
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a1 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    const bool took = verb(cmd);
    const char *out = cap_end();
    CHECK(took, "`content insert` is the console's own word");
    CHECK(strstr(out, "insert 0537a1 ready") != NULL, "and it says it is ready");
    /* The device announces the line size rather than the host assuming it: a build option that a
       host had hardcoded would break silently the day it changed. */
    char want[64];
    snprintf(want, sizeof(want), "lines of up to %u", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD);
    CHECK(strstr(out, want) != NULL, "and how long a line it will take");
    CHECK(iotdata_node_console_sink_active(), "the sink is installed");

    /* Nothing is at the real id yet: the bytes go to scratch and only a passed checksum renames. */
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537a1u, &total, &held), "nothing at the id while it is in flight");

    cap_begin();
    upload(data, sizeof(data), 0xFFFFFFFFu);
    (void)cap_end();
    CHECK(!iotdata_node_console_sink_active() || !_iotdata_node_console_content.up, "the sink stands down when the last byte has been offered");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    iotdata_node_console_content_event(ev);
    out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "the tick says it arrived");
    CHECK(strstr(out, "arrived") != NULL, "and says so");
    CHECK(iotdata_node_content_stat(&store, 0x0537a1u, &total, &held), "and it is at the real id now");
    CHECK(total == sizeof(data) && held == sizeof(data), "whole");

    uint8_t back[sizeof(data)];
    CHECK(iotdata_node_content_read(&store, 0x0537a1u, 0u, back, sizeof(back)) == (int)sizeof(back), "readable");
    CHECK(memcmp(data, back, sizeof(data)) == 0, "and byte for byte what was sent");

    /* And the store agrees with itself afterwards, which the insert's own running sum cannot say. */
    uint64_t sum = 0u;
    uint32_t len = 0u;
    CHECK(iotdata_node_content_checksum(&store, 0x0537a1u, &sum, &len) == IOTDATA_NODE_CONTENT_OK, "and sums complete");
    CHECK(sum == crc, "to the checksum that was declared before a byte was sent");
}

/* ------------------------------------------------------------------------------------------- */

static void test_dropped_line(void) {
    printf("\na dropped line is caught by its offset, and the rest is swallowed not obeyed\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 11u);
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a2 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    (void)verb(cmd);
    upload(data, sizeof(data), 2u); /* line 2 never sent */
    (void)cap_end();

    /* THE SINK STAYED INSTALLED to the declared end: the alternative is reading the remaining
       base64 as commands, and `d 576 SGVsbG8...` is not a command anybody wants resolved. */
    CHECK(_iotdata_node_console_content.up_lines == 5u, "every line that was sent was consumed");
    CHECK(!_iotdata_node_console_content.up, "and the sink stood down at the end, not at the error");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    iotdata_node_console_content_event(ev);
    const char *out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the insert failed");
    CHECK(iotdata_node_content_insert_why(&store) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "at the offset, before the checksum had to save it");
    CHECK(strstr(out, "not the bytes expected next") != NULL, "and says which");
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537a2u, &total, &held), "and nothing was left behind at the id");
}

static void test_corrupt_line(void) {
    printf("\na line that will not decode is a hole, and is treated as one\n");
    rig();
    uint8_t data[400];
    fill(data, sizeof(data), 5u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a3 %u 0000000000000000", (unsigned)sizeof(data));
    cap_begin();
    (void)verb(cmd);
    char l[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    line_for(l, sizeof(l), data, 0u, IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD);
    (void)sink(l);
    (void)sink("d 192 not!valid!base64!\n");
    line_for(l, sizeof(l), data, 384u, 16u);
    (void)sink(l);
    (void)cap_end();
    CHECK(_iotdata_node_console_content.up_bad == 1u, "the bad line was counted");
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "and the insert died rather than leaving a gap");
}

static void test_end_abandons(void) {
    printf("\n`end` gives the console back without waiting out the timeout\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 9u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a4 %u 0000000000000000", (unsigned)sizeof(data));
    cap_begin();
    (void)verb(cmd);
    upload(data, 384u, 0xFFFFFFFFu); /* two lines of six, then second thoughts */
    const size_t took = sink("end\nstat\n");
    const char *out = cap_end();
    CHECK(took == 4u, "the sink took `end\\n` and left `stat\\n` for the console");
    CHECK(!_iotdata_node_console_content.up, "it stood down");
    CHECK(strstr(out, "abandoned") != NULL, "and said so");
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the insert was abandoned too, not left in flight");
    CHECK(!iotdata_node_console_sink_active(), "and the console is back");
}

static void test_timeout_frees_the_console(void) {
    printf("\na host that walks away does not keep the console shut\n");
    rig();
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537a5 1000 0000000000000000");
    cap_begin();
    (void)verb(cmd);
    (void)sink("d 0 QUJD\n");
    (void)cap_end();
    CHECK(iotdata_node_console_sink_active(), "the sink is installed and the host is gone");
    clock_ms += (IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S + 1u) * 1000u;
    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    const char *out = cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the module timed it out");
    CHECK(strstr(out, "gave up") != NULL, "and the console said so");
    /* THE AUTHORITY IS THE MODULE'S TIMEOUT and the console follows it: nothing in
       iotdata_node_console.h knows what a transfer is, so nothing there could have done this. */
    CHECK(!iotdata_node_console_sink_active(), "and the sink was cleared, so a verb can be typed again");
    clock_ms = 1000u;
}

/* ------------------------------------------------------------------------------------------- */
/* The credit window. Not an error check -- the offsets do that -- but the only thing that stops a
   host outrunning a device whose transport drops what will not fit. */

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle))
        n++;
    return n;
}

static void test_window(void) {
    printf("\nthe host is given credit, and is given more of it as the bytes go by\n");
    rig();
    /* Three windows and a bit, so there is an ack per window AND a short last one. */
    const uint32_t len = 3u * IOTDATA_NODE_CONSOLE_CONTENT_WINDOW + IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD;
    uint8_t *data = (uint8_t *)malloc(len);
    CHECK(data != NULL, "a buffer");
    if (data == NULL)
        return;
    fill(data, len, 77u);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537f9 %u %016llx", (unsigned)len, (unsigned long long)_iotdata_crc64(data, len, _IOTDATA_CRC64_INIT));

    cap_begin();
    (void)verb(cmd);
    const char *out = cap_end();
    char want[64];
    snprintf(want, sizeof(want), "window %u", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW);
    CHECK(strstr(out, want) != NULL, "the ready line says how far ahead the host may run");

    cap_begin();
    upload(data, len, 0xFFFFFFFFu);
    out = cap_end();
    /* One per full window, plus one for the short tail -- the tail's is unconditional, because a
       host waiting on credit for a last short window would wait out the insert timeout instead. */
    CHECK(count_of(out, " ack ") == 4, "an ack per window, and one at the end");
    char at[64];
    snprintf(at, sizeof(at), " ack %u\n", (unsigned)IOTDATA_NODE_CONSOLE_CONTENT_WINDOW);
    CHECK(strstr(out, at) != NULL, "the first lands exactly on the window");
    snprintf(at, sizeof(at), " ack %u\n", (unsigned)len);
    CHECK(strstr(out, at) != NULL, "and the last on the final byte");

    cap_begin();
    const iotdata_node_content_event_t ev = tick();
    (void)cap_end();
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and the file landed");

    /* A REFUSED insert must keep acking. The credit counts what was OFFERED, so a host feeding to
       the declared end after a dropped line is still let through rather than stalling on a window
       that will never open again. */
    rig();
    cap_begin();
    (void)verb(cmd);
    (void)cap_end();
    cap_begin();
    upload(data, len, 1u); /* line 1 never sent: doomed from the second line onwards */
    out = cap_end();
    CHECK(count_of(out, " ack ") == 4, "a doomed insert is credited exactly the same");
    CHECK(!_iotdata_node_console_content.up, "and still stands down at the declared end");
    free(data);
}

/* ------------------------------------------------------------------------------------------- */

static void test_sink_boundaries(void) {
    printf("\nthe sink gives back exactly what is not its own\n");
    rig();
    uint8_t data[384];
    fill(data, sizeof(data), 21u);
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "content insert 0537b1 %u %016llx", (unsigned)sizeof(data), (unsigned long long)crc);
    cap_begin();
    (void)verb(cmd);

    /* Both data lines and a command behind them, in ONE buffer -- which is what a USB packet looks
       like when a host writes the whole thing at once. */
    char buf[2 * (IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32) + 16];
    char a[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], b[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32];
    line_for(a, sizeof(a), data, 0u, 192u);
    line_for(b, sizeof(b), data, 192u, 192u);
    snprintf(buf, sizeof(buf), "%s%sstat\n", a, b);
    const size_t n = sink(buf);
    (void)cap_end();
    CHECK(n == strlen(a) + strlen(b), "it took both data lines");
    CHECK(n < strlen(buf), "and less than it was offered, which is how it says it has finished");
    CHECK(strcmp(buf + n, "stat\n") == 0, "leaving the command behind it untouched");

    /* The other boundary: the transfer ends exactly where the buffer does, so there is nothing
       left to hand back and the sink has to stand down on the NEXT call instead. */
    rig();
    cap_begin();
    (void)verb(cmd);
    snprintf(buf, sizeof(buf), "%s%s", a, b);
    const size_t m = sink(buf);
    const size_t again = sink("stat\n");
    (void)cap_end();
    CHECK(m == strlen(buf), "it took the whole buffer");
    CHECK(again == 0u, "and then took none of the next one");

    /* And a line split across two reads is still one line. */
    rig();
    cap_begin();
    (void)verb(cmd);
    char half[256];
    snprintf(half, sizeof(half), "%.40s", a);
    (void)sink(half);
    (void)sink(a + 40);
    (void)cap_end();
    CHECK(_iotdata_node_console_content.up_lines == 1u, "a line split across two reads is one line");
    CHECK(_iotdata_node_console_content.up_bad == 0u, "and not a broken one");
}

/* The one case that needs poll() itself: a verb and the data behind it in the SAME read. The sink
   is installed by the dispatch, mid-buffer, and the bytes behind it must reach the sink rather than
   the verb table -- which is the whole reason poll() re-checks the sink per byte and not per read. */
static void con_content(__attribute__((unused)) const iotdata_node_console_emit_fn emit, const uint32_t now_ms, const int argc, char **const argv) {
    /* now_ms AND NOT clock_ms, which is the whole point of the parameter: the dispatch is handed
       the clock the pass began with, and a handler that reached for the test's own global would be
       reproducing the bug this signature exists to prevent. */
    (void)iotdata_node_console_content(now_ms, argc, argv);
}

static void test_through_poll(void) {
    printf("\na verb can install a sink and have its data follow in the same read\n");
    rig();
    uint8_t data[384];
    fill(data, sizeof(data), 33u);
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);
    static const iotdata_node_console_t cons[] = { { "content", con_content, "the store" } };
    char a[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], b[IOTDATA_NODE_CONSOLE_CONTENT_LINE + 32], all[2048];
    line_for(a, sizeof(a), data, 0u, 192u);
    line_for(b, sizeof(b), data, 192u, 192u);
    snprintf(all, sizeof(all), "content insert 0537c1 %u %016llx\n%s%s", (unsigned)sizeof(data), (unsigned long long)crc, a, b);

    /* A pipe on fd 0, so this goes through the host transport exactly as it ships -- poll() asking
       first, then a chunked read -- rather than through a test-only entry point. */
    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe stands in for the cable");
    const int saved = dup(0);
    (void)dup2(fds[0], 0);
    CHECK(write(fds[1], all, strlen(all)) == (ssize_t)strlen(all), "written in one go");
    cap_begin();
    iotdata_node_console_init(cons, 1u); /* opens the host transport and clears the line buffer */
    iotdata_node_console_tick(clock_ms);
    const iotdata_node_content_event_t ev = tick();
    const char *out = cap_end();
    (void)dup2(saved, 0);
    (void)close(saved);
    (void)close(fds[0]);
    (void)close(fds[1]);

    CHECK(_iotdata_node_console_content.up_lines == 2u, "both data lines reached the sink");
    CHECK(_iotdata_node_console_content.up_bad == 0u, "and none of them reached the verb table");
    CHECK(strstr(out, "unknown command") == NULL, "which is what that would have looked like");
    CHECK(ev == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and the file landed");
    uint8_t back[sizeof(data)];
    CHECK(iotdata_node_content_read(&store, 0x0537c1u, 0u, back, sizeof(back)) == (int)sizeof(back), "readable");
    CHECK(memcmp(back, data, sizeof(data)) == 0, "byte for byte");
}

/* ------------------------------------------------------------------------------------------- */

static void test_download(void) {
    printf("\na download comes back byte for byte, a slice at a time\n");
    rig();
    uint8_t data[1000];
    fill(data, sizeof(data), 17u);
    CHECK(filestore_ram_plant(&fs, 0x0537d1u, data, sizeof(data)), "planted, as a factory image would be");

    cap_begin();
    const bool took = verb("content download 0537d1");
    const char *out = cap_end();
    CHECK(took, "`content download` is the console's own word");
    CHECK(strstr(out, "download 0537d1 begin, 1000 bytes") != NULL, "and declares the length up front");
    CHECK(strstr(out, "INCOMPLETE") == NULL, "a whole file is not flagged");

    /* Ticked, not dumped: a line a pass, with the radio serviced in between. */
    cap_begin();
    int passes = 0;
    while (_iotdata_node_console_content.down && passes < 100) {
        (void)tick();
        passes++;
    }
    out = cap_end();
    CHECK(passes == 1 + (int)((sizeof(data) + IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD - 1u) / IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD), "one pass per line, plus the pass that finishes it");

    /* Reassembled from the replies, the way esp32-tool will do it. */
    uint8_t got[sizeof(data)];
    uint32_t seen = 0u;
    memset(got, 0, sizeof(got));
    for (const char *p = out; (p = strstr(p, " d ")) != NULL;) {
        p += 3;
        char *end = NULL;
        const unsigned long off = strtoul(p, &end, 10);
        CHECK(end != NULL && *end == ' ', "an offset and a space");
        char b64[IOTDATA_NODE_CONSOLE_CONTENT_B64 + 1];
        snprintf(b64, sizeof(b64), "%.*s", (int)strcspn(end + 1, "\n"), end + 1);
        uint8_t raw[IOTDATA_NODE_CONSOLE_CONTENT_PAYLOAD];
        const int n = _iotdata_node_console_content_decode(b64, raw, sizeof(raw));
        CHECK(n > 0, "a line that decodes");
        if (n > 0 && off + (unsigned long)n <= sizeof(got)) {
            memcpy(got + off, raw, (size_t)n);
            seen += (uint32_t)n;
        }
        p = end;
    }
    CHECK(seen == sizeof(data), "every byte came back");
    CHECK(memcmp(got, data, sizeof(data)) == 0, "and they are the bytes that were there");

    char want[80];
    snprintf(want, sizeof(want), "crc64=0x%016llX", (unsigned long long)_iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT));
    CHECK(strstr(out, want) != NULL, "and the trailer's sum is the file's sum");
    CHECK(strstr(out, "done, 1000 bytes") != NULL, "and it says it finished");
}

static void test_download_refusals(void) {
    printf("\nand a download refuses what it cannot do\n");
    rig();
    cap_begin();
    (void)verb("content download 0537ff");
    const char *out = cap_end();
    CHECK(strstr(out, "refused") != NULL, "no such id");

    /* An incomplete file downloads what is there and says it is not all of it, rather than
       refusing: half a file is sometimes exactly what is wanted when working out why. */
    uint8_t data[400];
    fill(data, sizeof(data), 41u);
    (void)iotdata_node_content_insert_begin(&store, 0x0537e1u, sizeof(data), 0u, clock_ms);
    (void)iotdata_node_content_insert_write(&store, 0u, data, 192u, clock_ms);
    cap_begin();
    (void)verb("content download 000001"); /* the scratch the insert is writing to */
    out = cap_end();
    CHECK(strstr(out, "INCOMPLETE") != NULL, "a partial file says so");
    CHECK(strstr(out, "begin, 192 bytes") != NULL, "and offers what it has");

    /* One job at a time: the insert owns the console until it is done. */
    cap_begin();
    (void)verb("content insert 0537e2 100 0");
    out = cap_end();
    CHECK(strstr(out, "refused") != NULL, "a second insert is refused while one is in flight");
}

/* ------------------------------------------------------------------------------------------- */

static void test_control_words(void) {
    printf("\nlist, delete and validate, through the handler a radio would reach\n");
    rig();
    uint8_t a[300], b[64];
    fill(a, sizeof(a), 1u);
    fill(b, sizeof(b), 2u);
    CHECK(filestore_ram_plant(&fs, 0x0537f1u, a, sizeof(a)), "two files");
    CHECK(filestore_ram_plant(&fs, 0x0537f2u, b, sizeof(b)), "planted");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST, NULL, 0u, 1000u, NULL), "list is ours");
    CHECK(said_has("0537f1"), "and lists the first");
    CHECK(said_has("0537f2"), "and the second");
    CHECK(said_has("2 files"), "and counts them last");

    /* The id is read back out of the three bytes iotdata_node_control_build writes, which is the
       only check that the two layers agree about the width of a content id. */
    said_reset();
    const uint8_t id_f1[3] = { 0x05u, 0x37u, 0xf1u };
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_VALIDATE, id_f1, 3u, 1000u, NULL), "validate is ours");
    char want[80];
    snprintf(want, sizeof(want), "crc64=0x%016llX", (unsigned long long)_iotdata_crc64(a, sizeof(a), _IOTDATA_CRC64_INIT));
    CHECK(said_has(want), "and reports the store's own sum");
    CHECK(!said_has("INCOMPLETE"), "over a whole file");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, id_f1, 3u, 1000u, NULL), "delete is ours");
    CHECK(said_has("deleted"), "and says so");
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537f1u, &total, &held), "and it is gone");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, id_f1, 3u, 1000u, NULL), "a second delete is still ours");
    CHECK(!said_has("deleted"), "but deletes nothing");
    /* And says WHY in the words an operator needs: a well-formed id with nothing under it is not
       the same complaint as an id that could never name anything. */
    CHECK(said_has("no such content id"), "and distinguishes absent from malformed");

    said_reset();
    /* OURS AND REFUSED -- and the answer now says which, instead of leaving a caller to read
       "claimed" and assume it ran. The reason comes back with it, because the operator who typed
       the command is the only one who can act on it. */
    const char *why = NULL;
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, NULL, 0u, 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "an id-less command is OURS and refused");
    CHECK(said_has("no id"), "and says which");
    CHECK(why != NULL && strcmp(why, "no id given") == 0, "to the seam as well as to the console");
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST, NULL, 0u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "a command that ran is DONE, not merely claimed");
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, 0x7Fu, NULL, 0u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_UNKNOWN, "an action we do not have is not ours");
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_CONTENT_LIST, NULL, 0u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_UNKNOWN, "nor another subject's");
}

/* ------------------------------------------------------------------------------------------- */

/* THE OFFERS HALF, through the same handler a radio reaches. What the words have to get right is
   not the table -- that has its own suite -- but the four things only this layer does: reading the
   arguments the control vocabulary packed, taking the size and the hash off the STORE rather than
   from the request, refusing honestly what this build cannot do, and putting the right bytes in
   front of the application's down path. */
static void test_offer_words(void) {
    printf("\nauthoring, listing, advertising and revoking an offer, through the handler\n");
    rig();
    uint8_t a[448];
    fill(a, sizeof(a), 3u);
    CHECK(filestore_ram_plant(&fs, 0x0537a1u, a, sizeof(a)), "a file is held");
    const uint64_t crc = _iotdata_crc64(a, sizeof(a), _IOTDATA_CRC64_INIT);

    /* { u24 id, u8 encoding, u16 expiry-m, u24 sig-id, name NUL } -- exactly what
       iotdata_node_control_build writes for ARG_MANIFEST, which is the only check that the two
       layers agree about the shape. */
    const uint8_t set[] = { 0x05u, 0x37u, 0xa1u, 0x00u, 0x05u, 0xA0u, 0x05u, 0x37u, 0xa2u, 'i', 'm', 'g', '\0' };
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set, (uint8_t)sizeof(set), 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "the manifest is authored");
    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, 0x0537a1u);
    CHECK(m != NULL, "and the table has it");
    if (m != NULL) {
        CHECK(m->size == sizeof(a) && m->hash == crc, "with the size and the hash READ OFF THE STORE, not taken from the request");
        CHECK(m->expiry_m == 1440u && m->sig_id == 0x0537a2u, "and the fields that were");
        CHECK(strcmp(m->name, "img") == 0, "the name included");
        CHECK(m->local, "it is ours, so we may advertise it");
    }
    /* THE REPLY STATES WHAT WAS WRITTEN DOWN, which is the only chance an operator has to see that
       the file they think they uploaded is the file the node summed. */
    CHECK(said_has("448 bytes"), "the reply says the size it found");
    CHECK(said_has("14 chunks"), "what that is in chunks");
    CHECK(said_has("img"), "and the name");

    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST, NULL, 0u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "the offers list");
    CHECK(said_has("0537A1"), "naming the id");
    CHECK(said_has("1 offer of"), "and how many of the table is in use");

    /* ADVERTISING: the audience is the second field, and the bytes handed to the down path have to
       be a MANIFEST TLV for that id -- which is checked by parsing them back. */
    said_reset();
    const uint8_t adv[] = { 0x05u, 0x37u, 0xa1u, 0x05u, 0x7Du };
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv, (uint8_t)sizeof(adv), 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "it is advertised");
    CHECK(aired.calls == 1, "once");
    CHECK(aired.audience == 0x057Du, "to the station the request named");
    iotdata_content_manifest_t w;
    CHECK(iotdata_content_manifest_parse(aired.val, aired.len, &w), "and what went out is a MANIFEST");
    CHECK(w.id == 0x0537a1u && w.size == sizeof(a) && w.hash == crc, "for that content, as described");
    CHECK(w.generation == 1u, "with the first origination spent on it");

    /* AND COMING ROUND AGAIN AT ONCE IS REFUSED, which is the floor under re-origination: four
       bits of generation exhaust in sixteen, so a loop would burn them in under a minute. */
    said_reset();
    const char *why = NULL;
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv, (uint8_t)sizeof(adv), 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "a second origination at once is refused");
    CHECK(aired.calls == 1, "and nothing more went on the air");
    CHECK(said_has("too recently"), "and the reason is the floor, not the two rules it is not");
    CHECK(why != NULL && strstr(why, "four bits") != NULL, "to the seam as well");

    /* A SECOND ORIGINATION A FLOOR LATER DIFFERS, because a byte-identical refresh is thrown by
       every relay that still remembers the last one -- which would silence the one message whose
       job is to keep arriving. */
    const uint32_t spaced_ms = 1000u + (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u + 1000u;
    uint8_t first[128];
    const size_t first_len = aired.len;
    memcpy(first, aired.val, first_len);
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv, (uint8_t)sizeof(adv), spaced_ms, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "advertised again");
    CHECK(aired.len == first_len && memcmp(aired.val, first, first_len) != 0, "same size, different bytes");

    /* A TRANSMIT THAT FAILS IS REFUSED RATHER THAN CLAIMED, so the operator knows nothing went. */
    said_reset();
    aired.fail = true;
    const uint32_t spaced2_ms = spaced_ms + (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u + 1000u;
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv, (uint8_t)sizeof(adv), spaced2_ms, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "a failed send is refused");
    CHECK(why != NULL && strstr(why, "could not be sent") != NULL, "and says why");
    aired.fail = false;

    /* REVOKING: the fleet is told FIRST and the bytes destroyed after, which is the order that
       cannot leave a holder serving what this node has already thrown away. */
    said_reset();
    const uint8_t id_a1[3] = { 0x05u, 0x37u, 0xa1u };
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_REVOKE, id_a1, 3u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "it is revoked");
    CHECK(iotdata_content_manifest_parse(aired.val, aired.len, &w) && w.expiry_m == IOTDATA_CONTENT_EXPIRY_REVOKED, "the frame that went out is a zero expiry");
    CHECK(w.id == 0x0537a1u, "for that id");
    uint32_t total = 0u, held = 0u;
    CHECK(!iotdata_node_content_stat(&store, 0x0537a1u, &total, &held), "and the bytes are gone");
    CHECK(iotdata_node_content_manifest_find(&offers, 0x0537a1u) == NULL, "with the offer");
    CHECK(said_has("announced"), "and the reply says the fleet was told");
}

static void test_offer_words_refuse_honestly(void) {
    printf("\nand what they refuse: an absent file, a cache's offer, and a build that cannot originate\n");
    rig();

    /* A MANIFEST FOR SOMETHING WE DO NOT HOLD. The size and the hash come off the store, so there
       is nothing to describe and nothing is invented. */
    const uint8_t set[] = { 0x05u, 0x37u, 0xb9u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, '\0' };
    said_reset();
    const char *why = NULL;
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set, (uint8_t)sizeof(set), 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "refused");
    CHECK(said_has("no such content id"), "naming what the store said");
    CHECK(iotdata_node_content_manifests_count(&offers) == 0u, "and no row is left behind");

    /* TOO SHORT TO BE A MANIFEST. Nine bytes is the fixed part with no terminator for the name,
       and there is no length field to consult for a second opinion. */
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set, 9u, 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "a truncated argument is refused");
    CHECK(why != NULL && strstr(why, "malformed") != NULL, "as malformed");

    /* AN OFFER WE ONLY HEARD MUST NOT BE RE-ORIGINATED, which is a rule and not a fault: the
       expiry is not decremented when forwarded, so a cache re-stating one would extend its life
       indefinitely. */
    uint8_t frame[64];
    const iotdata_content_manifest_t heard = { .id = 0x0601a1u, .size = 64u, .hash = 5u, .sig_id = 0x0601a2u, .expiry_m = 600u, .name = "somebody else's" };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &store, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "we know of it");
    said_reset();
    const uint8_t adv_heard[] = { 0x06u, 0x01u, 0xa1u, 0x0Fu, 0xFFu };
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv_heard, (uint8_t)sizeof(adv_heard), 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "and must not re-originate it");
    CHECK(aired.calls == 0, "nothing went on the air");
    CHECK(said_has("not ours to originate"), "and the reason says which of the three it is");
    CHECK(why != NULL && strstr(why, "not ours") != NULL, "to the seam too");

    /* A BUILD THAT CANNOT ORIGINATE -- which is every relay and every sensor today -- says so
       rather than going quiet, so the verb is not "unknown" on some nodes and real on others. */
    rig_offers(false);
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv_heard, (uint8_t)sizeof(adv_heard), 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "refused, not unknown");
    CHECK(said_has("cannot originate"), "and says what this build is");
    /* ... but it can still be TOLD, and still answer what it knows. */
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST, NULL, 0u, 1000u, NULL) == IOTDATA_NODE_CONTROL_RESULT_DONE, "listing still works");

    /* AND A BUILD WITH NO OFFERS TABLE AT ALL. Same honesty, one layer further out. */
    iotdata_node_content_control_offers_init(NULL, NULL, NULL);
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST, NULL, 0u, 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "no table is refused");
    CHECK(said_has("tracks none"), "and says so");
    rig_offers(true);
    /* AND THE FOURTH REASON, which destroys the evidence for itself: advertise() DROPS an offer
       whose bytes have been replaced, so by the time the handler looks for a row to explain the
       refusal there is none -- and "no offer of ours for that id" is exactly the wrong thing to
       tell somebody whose id was right. */
    {
        rig();
        static uint8_t one[448], two[448];
        for (size_t i = 0; i < sizeof(one); i++) {
            one[i] = (uint8_t)(0x10u + i);
            two[i] = (uint8_t)(0x99u + i); /* same length, different object: what a size check misses */
        }
        CHECK(filestore_ram_plant(&fs, 0x0537a1u, one, sizeof(one)), "a file");
        CHECK(iotdata_node_content_manifest_author(&offers, &store, 0x0537a1u, "img", 0u, 600u, 0x0537a2u, 1000u) == IOTDATA_NODE_CONTENT_OK, "an offer of ours");
        CHECK(filestore_ram_plant(&fs, 0x0537a1u, two, sizeof(two)), "and a different build dropped in under the same id");
        const uint8_t adv[] = { 0x05u, 0x37u, 0xa1u, 0x0Fu, 0xFFu }; /* id then audience: broadcast */
        said_reset();
        CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, adv, (uint8_t)sizeof(adv), 1000u, &why) == IOTDATA_NODE_CONTROL_RESULT_REFUSED, "advertising it is refused");
        CHECK(said_has("replaced"), "saying the bytes under it were replaced");
        CHECK(!said_has("no offer of ours"), "and NOT that the id was unknown, which is what it used to say");
    }
}

/* ------------------------------------------------------------------------------------------- */
/* `content inspect` over faux bundles. The header parser has its own suite; what is new here is
   that a blob in the STORE is found, read out of it, parsed and judged -- and that the length check
   gets the length from what the store is actually holding, which is the one check that cannot be
   made from a header alone. */

static const uint8_t HASH_T[IOTDATA_NODE_OTA_HASH_LEN] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };

static uint32_t faux(const uint32_t id, const char *const app, const uint32_t outsize, const uint32_t paysize, const uint32_t payload_bytes) {
    uint8_t blob[512];
    memcpy(blob, IOTDATA_NODE_OTA_MAGIC, IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, blob + IOTDATA_NODE_OTA_MAGIC_LEN, sizeof(blob) - IOTDATA_NODE_OTA_MAGIC_LEN);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_APP, app);
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_SEMVER, "1.2.3");
    iotdata_kvr_add_str(&kv, IOTDATA_NODE_OTA_KEY_STAMP, "202610111200");
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FORM, IOTDATA_NODE_OTA_FORM_FULL);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_CODEC, IOTDATA_NODE_OTA_CODEC_NONE);
    iotdata_kvr_add_u8(&kv, IOTDATA_NODE_OTA_KEY_FLAGS, 0u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_BOOTLOADER_MIN, 1u);
    iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_OUTPUT_SIZE, outsize);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_TARGET_HASH, HASH_T, IOTDATA_NODE_OTA_HASH_LEN);
    if (paysize != 0u)
        iotdata_kvr_add_u32(&kv, IOTDATA_NODE_OTA_KEY_PAYLOAD_SIZE, paysize);
    iotdata_kvr_add(&kv, IOTDATA_NODE_OTA_KEY_END, NULL, 0);
    const uint32_t head = (uint32_t)(IOTDATA_NODE_OTA_MAGIC_LEN + kv.len);
    for (uint32_t i = 0; i < payload_bytes && head + i < sizeof(blob); i++)
        blob[head + i] = (uint8_t)(i * 7u);
    return filestore_ram_plant(&fs, id, blob, head + payload_bytes) ? head : 0u;
}

static void test_inspect_ota(void) {
    printf("\ninspect reads a bundle out of the store and judges it\n");
    rig();
    static const iotdata_node_ota_device_t relay = {
        .app = "relay",
        .stamp = NULL,
        .bootloader = 1u,
        .slot_size = 0x100000u,
        .base_hash = NULL,
        .codec_deflate = true,
    };
    iotdata_node_content_control_init(&store, &relay);
    const uint8_t id_ok[3] = { 0x05u, 0x38u, 0x01u }, id_app[3] = { 0x05u, 0x38u, 0x02u }, id_short[3] = { 0x05u, 0x38u, 0x03u };
    const uint8_t id_plain[3] = { 0x05u, 0x38u, 0x04u }, id_trunc[3] = { 0x05u, 0x38u, 0x05u };

    const uint32_t head = faux(0x053801u, "relay", 0x40000u, 64u, 64u);
    CHECK(head > 0u, "a bundle this build could take");
    said_reset();
    CHECK(iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_ok, 3u, 1000u, NULL), "inspect is ours");
    CHECK(said_has("app=relay semver=1.2.3 stamp=202610111200"), "the identity");
    CHECK(said_has("form=full codec=none"), "the shape");
    CHECK(said_has("payload=64"), "and what the payload should weigh");
    CHECK(said_has("ota check: ok"), "and this build would take it");
    CHECK(said_has("nobody has claimed it"), "while no consumer owns it, which is true and worth saying");

    (void)faux(0x053802u, "gateway", 0x40000u, 64u, 64u);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_app, 3u, 1000u, NULL);
    CHECK(said_has("app=gateway"), "a bundle for something else parses");
    CHECK(said_has("ota check: different application"), "and is refused for the right reason");

    /* The one that needs the store: a header claiming 64 payload bytes over a file that holds 32.
       Nothing in the header is wrong -- it is the FILE that is short, and only the store knows. */
    (void)faux(0x053803u, "relay", 0x40000u, 64u, 32u);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_short, 3u, 1000u, NULL);
    CHECK(said_has("ota check: not equal to the header length"), "a short file is caught here, not at the far end of a decode");

    /* Most content is not firmware, and inspect says nothing about OTA for it rather than
       announcing that it failed to be something nobody claimed it was. */
    uint8_t plain[200];
    fill(plain, sizeof(plain), 61u);
    CHECK(filestore_ram_plant(&fs, 0x053804u, plain, sizeof(plain)), "an ordinary file");
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_plain, 3u, 1000u, NULL);
    CHECK(said_has("053804 200 of 200 bytes"), "is described");
    CHECK(!said_has("ota"), "and is not accused of being a bundle");

    /* The magic and nothing else: it IS claiming to be one, so saying why it is not readable is
       the useful answer. */
    CHECK(filestore_ram_plant(&fs, 0x053805u, IOTDATA_NODE_OTA_MAGIC, (uint32_t)IOTDATA_NODE_OTA_MAGIC_LEN), "a bundle that is only its magic");
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_trunc, 3u, 1000u, NULL);
    CHECK(said_has("ota header:"), "says the header is the problem");
    CHECK(!said_has("ota check:"), "and does not go on to judge what it could not read");

    /* And with nothing declared about this build, it parses and declines to judge rather than
       judging against zeroes and calling a good bundle unacceptable. */
    iotdata_node_content_control_init(&store, NULL);
    said_reset();
    (void)iotdata_node_content_control(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, id_ok, 3u, 1000u, NULL);
    CHECK(said_has("app=relay"), "still parses");
    CHECK(said_has("not judged"), "and says why it will not say");
}

/* ------------------------------------------------------------------------------------------- */

static void test_control_argv_agrees(void) {
    printf("\nthe vocabulary builds the id the handler reads\n");
    iotdata_node_control_args_t args;
    char *argv[] = { (char *)"content", (char *)"delete", (char *)"1a2b3c" };
    const iotdata_node_control_command_t *const c = iotdata_node_control_from_argv(3, argv, &args, NULL);
    CHECK(c != NULL && strcmp(c->name, "content-delete") == 0, "`content delete 1a2b3c` resolves");
    CHECK(args.content == 0x1a2b3cu, "and a bare id is read as HEX, which is the only way §4's ids are legible");

    uint8_t buf[32];
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    CHECK(iotdata_node_control_build(&kv, c, &args), "and packs");
    size_t cur = 0;
    uint8_t key = 0, vlen = 0;
    const uint8_t *val = NULL;
    CHECK(iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen), "one entry");
    CHECK(key == IOTDATA_NODE_CONTROL_CONTROL, "a CONTROL");
    CHECK(vlen == 5u, "subject, action and three bytes of id");
    CHECK(val[0] == IOTDATA_NODE_TLV_CONTENT && val[1] == IOTDATA_NODE_ACTION_CONTENT_DELETE, "for the store");
    uint32_t id = 0u;
    CHECK(_iotdata_node_content_control_id(val + 2, (uint8_t)(vlen - 2u), &id), "the handler reads it");
    CHECK(id == 0x1a2b3cu, "as the same id");

    /* And what is not an id is refused before it can be mistaken for one. */
    char *bad[] = { (char *)"content", (char *)"delete", (char *)"1a2b3c4" };
    CHECK(iotdata_node_control_from_argv(3, bad, &args, NULL) == NULL, "seven hex digits is not a 24-bit id");
    char *none[] = { (char *)"content", (char *)"delete" };
    CHECK(iotdata_node_control_from_argv(2, none, &args, NULL) == NULL, "and an id is not optional");
    char *zz[] = { (char *)"content", (char *)"delete", (char *)"zz" };
    CHECK(iotdata_node_control_from_argv(3, zz, &args, NULL) == NULL, "nor is a word an id");
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_console_content: the store's words\n");
    test_base64();
    test_insert();
    test_dropped_line();
    test_corrupt_line();
    test_end_abandons();
    test_timeout_frees_the_console();
    test_window();
    test_sink_boundaries();
    test_through_poll();
    test_download();
    test_download_refusals();
    test_control_words();
    test_offer_words();
    test_offer_words_refuse_honestly();
    test_inspect_ota();
    test_control_argv_agrees();
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
