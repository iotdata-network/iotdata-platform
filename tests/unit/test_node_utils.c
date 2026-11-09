//
// test_node_utils.c - the small shared helpers: ordered insert, and node identity.
//
// A skeleton, deliberately. iotdata_node_order_insert is pure and gets covered properly; the identity
// helpers derive from the host, so they can only be checked for the properties that must hold
// wherever they run -- determinism, range, and agreement with each other.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* dup/dup2, to prove the derivation prints nothing */
#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "device/d_platform_linux.h" /* the identity and the reset reason this host can answer */
#include "device/d_format.h"
#include "iotdata_node_utils.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static void test_order_insert(void) {
    printf("\nordered insert keeps the list sorted and carries the slot with the key\n");
    iotdata_node_order_t ord[8];
    int n = 0;
    /* arrive out of order; the list must read back in key order */
    const uint16_t keys[] = { 0x300, 0x100, 0x200 };
    for (int i = 0; i < 3; i++)
        n = iotdata_node_order_insert(ord, n, 8, keys[i], (uint16_t)i);
    CHECK(n == 3, "three inserted");
    CHECK(ord[0].key == 0x100 && ord[1].key == 0x200 && ord[2].key == 0x300, "sorted by key");
    /* the slot is the point: it says WHERE the entry lives, and must travel with its key */
    CHECK(ord[0].slot == 1, "0x100 came from slot 1");
    CHECK(ord[1].slot == 2, "0x200 came from slot 2");
    CHECK(ord[2].slot == 0, "0x300 came from slot 0");

    printf("\nand refuses to overflow rather than writing past the end\n");
    iotdata_node_order_t small[3];
    int m = 0;
    for (int i = 0; i < 5; i++)
        m = iotdata_node_order_insert(small, m, 3, (uint16_t)(100 - i), (uint16_t)i);
    CHECK(m == 3, "stops at max");
    CHECK(small[0].key == 98 && small[2].key == 100, "and what it did take is still ordered");

    CHECK(iotdata_node_order_insert(NULL, 0, 8, 1, 1) == 0, "a null list returns the count unchanged");

    printf("\nequal keys do not lose an entry\n");
    iotdata_node_order_t dup[4];
    int d = 0;
    d = iotdata_node_order_insert(dup, d, 4, 0x500, 1);
    d = iotdata_node_order_insert(dup, d, 4, 0x500, 2);
    CHECK(d == 2, "both kept");
    CHECK(dup[0].key == 0x500 && dup[1].key == 0x500, "both present");

    printf("\nalready-sorted and reverse-sorted input both come out sorted\n");
    iotdata_node_order_t up[4], down[4];
    int u = 0, w = 0;
    for (int i = 0; i < 4; i++)
        u = iotdata_node_order_insert(up, u, 4, (uint16_t)(i + 1), (uint16_t)i);
    for (int i = 0; i < 4; i++)
        w = iotdata_node_order_insert(down, w, 4, (uint16_t)(4 - i), (uint16_t)i);
    CHECK(u == 4 && w == 4, "both took everything");
    for (int i = 0; i < 4; i++)
        CHECK(up[i].key == down[i].key, "and agree on the result");
}

static void test_identity(void) {
    printf("\nidentity is derived, so it must at least be stable and in range\n");
    uint8_t a[6], b[6];
    const bool ok_a = iotdata_node_mac(a), ok_b = iotdata_node_mac(b);
    CHECK(ok_a == ok_b, "the mac read agrees with itself");
    if (ok_a) {
        CHECK(memcmp(a, b, 6) == 0, "and returns the same mac twice");
        /* all-zero would mean "we made one up", which is worse than failing honestly */
        const uint8_t zero[6] = { 0 };
        CHECK(memcmp(a, zero, 6) != 0, "a derived mac is not all zero");
    } else
        printf("  (no mac available on this host -- content checks skipped)\n");

    const uint32_t m1 = iotdata_node_mac32(), m2 = iotdata_node_mac32();
    CHECK(m1 == m2, "mac32 is stable");

    const uint16_t s1 = iotdata_node_station_from_mac(NULL), s2 = iotdata_node_station_from_mac(NULL);
    CHECK(s1 == s2, "a station derived from the mac is stable -- it IS the node's name");
    CHECK(iotdata_station_is_assignable(s1), "and lands in the assignable range");

    /* THE DESCRIPTOR IS THE OTHER HALF OF THE SAME CALL, and must agree with it: the caller logs
       from this, so a station here that differs from the one returned would be a line about a
       node that does not exist. */
    iotdata_node_identity_t id;
    const uint16_t s3 = iotdata_node_station_from_mac(&id);
    CHECK(s3 == s1, "the two forms derive the same station");
    CHECK(id.station == s3, "and the descriptor carries the one that was returned");
    CHECK((id.source == IOTDATA_NODE_IDENTITY_MAC) == ok_a, "source says MAC exactly when there was one");
    if (id.source == IOTDATA_NODE_IDENTITY_MAC)
        CHECK(memcmp(id.mac, a, 6) == 0, "and the address it reports is the one the platform gave");
    else {
        /* an invented address in a boot line is worse than no address at all */
        const uint8_t zero[6] = { 0 };
        CHECK(memcmp(id.mac, zero, 6) == 0, "with no mac, the descriptor reports none rather than something");
    }
}

/* WHAT THIS CHANGE WAS FOR. These helpers used to take a tag and write their own board: line, so
   the derivation logged wherever it was called from -- a --help listing, a test run, a lazily
   cached accessor. The words stayed here as a formatter; the printing went to the callers. */
static void test_the_derivation_prints_nothing(void) {
    printf("\nthe derivation says nothing at all: the caller reports it, after the call\n");
    /* pre-set, so the formatter checks below still have something legal to read if the capture
       below could not be set up and the derivation was never run */
    iotdata_node_identity_t id = { .station = 1u, .source = IOTDATA_NODE_IDENTITY_NONE, .mac = { 0 } };

    fflush(stdout);
    fflush(stderr);
    FILE *const sink = tmpfile();
    const int saved_out = dup(fileno(stdout)), saved_err = dup(fileno(stderr));
    CHECK(sink != NULL && saved_out >= 0 && saved_err >= 0, "the capture could be set up");
    if (sink != NULL && saved_out >= 0 && saved_err >= 0) {
        (void)dup2(fileno(sink), fileno(stdout)); /* ESP_LOG* shims to PRINTF_*, which is stderr */
        (void)dup2(fileno(sink), fileno(stderr)); /* -- but capture both, so neither can hide one */
        (void)iotdata_node_station_from_mac(&id);
        (void)iotdata_node_mac32();
        (void)iotdata_node_reason_reset();
        fflush(stdout);
        fflush(stderr);
        const long wrote = ftell(sink);
        (void)dup2(saved_out, fileno(stdout));
        (void)dup2(saved_err, fileno(stderr));
        (void)close(saved_out);
        (void)close(saved_err);
        CHECK(wrote == 0, "nothing was written by the three identity calls");
        (void)fclose(sink);
    }

    /* THE WORDS ARE STILL THIS HEADER'S, which is what keeps four applications saying the same
       thing. It must name the station, and must fit the size it advertises. */
    char buf[IOTDATA_NODE_IDENTITY_STR_MAX + 1];
    char want[8];
    (void)snprintf(want, sizeof(want), "%03X", (unsigned)id.station);
    const char *const str = iotdata_node_identity_str(buf, sizeof(buf), &id);
    CHECK(str == buf, "the formatter returns the buffer it was given");
    CHECK(strstr(str, want) != NULL, "the sentence names the station it derived");
    CHECK(strlen(str) < sizeof(buf) - 1, "and fits IOTDATA_NODE_IDENTITY_STR_MAX without truncating");
    printf("  (this host: %s)\n", str);

    /* a caller with nothing to report must not have to guard the call */
    CHECK(strlen(iotdata_node_identity_str(buf, sizeof(buf), NULL)) > 0, "no descriptor still formats to something");

    /* THE OTHER TWO BRANCHES, hand-built, because this host has a mac and can never reach them --
       and they are the ones that matter most: both are a warning, and a reader has to be able to
       tell "no permanent mac" from a mac that simply was not printed. */
    const iotdata_node_identity_t alt = { .station = 0x123u, .source = IOTDATA_NODE_IDENTITY_ALT, .mac = { 0 } };
    const char *const alt_str = iotdata_node_identity_str(buf, sizeof(buf), &alt);
    CHECK(strstr(alt_str, "no permanent mac") != NULL, "a derived identity says so");
    CHECK(strstr(alt_str, "123") != NULL, "and still names the station");
    CHECK(strstr(alt_str, "from '") != NULL, "and names what it was derived from");
    CHECK(strlen(alt_str) < sizeof(buf) - 1, "and the longest sentence fits the advertised size");

    const iotdata_node_identity_t none = { .station = 1u, .source = IOTDATA_NODE_IDENTITY_NONE, .mac = { 0 } };
    const char *const none_str = iotdata_node_identity_str(buf, sizeof(buf), &none);
    CHECK(strstr(none_str, "no permanent mac") != NULL, "no identity at all says so too");
    CHECK(strstr(none_str, "001") != NULL, "and reports the legal station it fell back to");
    CHECK(strstr(none_str, "from '") == NULL, "but claims no source, because there was none");
}

static void test_reset_reason(void) {
    printf("\nthe reset reason is always answerable, even when the answer is unknown\n");
    const uint8_t r1 = iotdata_node_reason_reset(), r2 = iotdata_node_reason_reset();
    CHECK(r1 == r2, "and does not change between calls");
}

int main(void) {
    printf("test_node_utils\n");
    test_order_insert();
    test_identity();
    test_the_derivation_prints_nothing();
    test_reset_reason();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
