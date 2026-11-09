// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_format.c - host tests for d_format.h.
//
// These helpers were application-private before, in files no test could reach, and the cost showed:
// secs2str shipped without a NUL terminator, so a relay's stat line printed a stray byte after its
// uptime and the fault was invisible until somebody read the log closely. Every check here is
// against that class of bug -- the terminator, the bound, and what happens at the edges.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include "device/d_format.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define POISON 0x7F /* an unterminated result reads as trailing junk, which is the bug we had */

static void test_secs2str(void) {
    printf("secs2str: a duration as the units that fit\n");
    static const struct {
        long sec;
        const char *want;
    } cases[] = {
        { 0, "0" },       { 1, "1s" },    { 59, "59s" },      { 60, "1m" }, /* no trailing "0s": a skipped component, not a padded one */
        { 501, "8m21s" }, { 3600, "1h" }, { 3661, "1h1m1s" }, { 86400, "1d" }, { 90061, "1d1h1m1s" }, { 86399, "23h59m59s" }, { -501, "-8m21s" }, { 999999999L, "11574d1h46m39s" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char b[48];
        memset(b, POISON, sizeof(b));
        const char *const r = secs2str(cases[i].sec, b, (int)sizeof(b));
        if (r == NULL) {
            printf("  FAIL: %ld returned NULL, which is undefined behaviour in a \"%%s\"\n", cases[i].sec);
            fails++;
        } else if (strcmp(r, cases[i].want) != 0) {
            printf("  FAIL: %ld -> \"%s\", wanted \"%s\"\n", cases[i].sec, r, cases[i].want);
            fails++;
        }
    }

    /* Truncation: still terminated, still never NULL, and never a byte past the buffer. This is
       the whole reason the function returns `out` rather than 0 on truncation -- the result goes
       into a "%s" where a null pointer is not a diagnosable error. */
    for (int len = 0; len <= 12; len++) {
        char b[24];
        memset(b, POISON, sizeof(b));
        const char *const r = secs2str(90061, b, len);
        CHECK(r != NULL, "a too-small buffer still yields a string, not NULL");
        if (r == NULL)
            continue;
        if (len > 0)
            CHECK(memchr(b, '\0', (size_t)len) != NULL, "and it is terminated inside the buffer");
        for (size_t i = (size_t)(len < 0 ? 0 : len); i < sizeof(b); i++)
            if (b[i] != (char)POISON) {
                printf("  FAIL: len=%d wrote past the buffer at byte %zu\n", len, i);
                fails++;
                break;
            }
    }
    /* what truncation actually looks like, so the shape is pinned and not just "some prefix" */
    char t[6];
    CHECK(strcmp(secs2str(90061, t, (int)sizeof(t)), "1d1h1") == 0, "truncates at the buffer, keeping the largest units");
}

static void test_hexdump_line(void) {
    printf("format_hexdump_line: one line, bounded by construction\n");
    const uint8_t data[] = { 0x35, 0x38, 0x00, 0x01, 0x2C, 0xC1, 0x68, 0x97, 0x3E, 'h', 'i', 0x7F, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB };
    char line[FORMAT_HEXDUMP_LINE_MAX];

    memset(line, POISON, sizeof(line));
    CHECK(format_hexdump_line(line, sizeof(line), data, sizeof(data), 0), "first line rendered");
    CHECK(strcmp(line, "[0000] 35 38 00 01 2C C1 68 97  3E 68 69 7F 00 00 00 00 |58..,.h.>hi.....|") == 0, "offset, hex with a mid gap, and a delimited ascii column");

    /* the short last line: hex columns blank-padded so the ascii column still lines up, and the
       delimiters make the padding unambiguous */
    CHECK(format_hexdump_line(line, sizeof(line), data, sizeof(data), 16), "second line rendered");
    CHECK(strcmp(line, "[0010] AA BB                                            |..              |") == 0, "a short line pads rather than ragged-ending");
    char full[FORMAT_HEXDUMP_LINE_MAX];
    (void)format_hexdump_line(full, sizeof(full), data, sizeof(data), 0);
    CHECK(strlen(line) == strlen(full), "every line is the same width, which is what keeps the columns readable");

    /* the loop terminates where the data does */
    CHECK(!format_hexdump_line(line, sizeof(line), data, sizeof(data), 18), "nothing at the end");
    CHECK(!format_hexdump_line(line, sizeof(line), data, sizeof(data), 32), "nor past it");
    CHECK(!format_hexdump_line(line, sizeof(line), data, 0, 0), "nor for an empty buffer");
    CHECK(!format_hexdump_line(line, sizeof(line), NULL, 4, 0), "nor for no data at all");

    /* FORMAT_HEXDUMP_LINE_MAX has to actually be the bound, or a caller sizing by it overflows */
    memset(line, POISON, sizeof(line));
    (void)format_hexdump_line(line, sizeof(line), data, sizeof(data), 0);
    const size_t written = strlen(line);
    printf("  line is %zu chars, bound is %d\n", written, FORMAT_HEXDUMP_LINE_MAX);
    CHECK(written + 1 <= (size_t)FORMAT_HEXDUMP_LINE_MAX, "the rendered line fits the advertised bound");
    CHECK(line[written + 1] == (char)POISON, "and nothing was written past its terminator");

    /* a buffer smaller than the bound is refused outright rather than half-filled: a truncated
       hex dump is worse than none, because the bytes no longer line up with the offsets */
    char small[FORMAT_HEXDUMP_LINE_MAX - 1];
    memset(small, POISON, sizeof(small));
    CHECK(!format_hexdump_line(small, sizeof(small), data, sizeof(data), 0), "too small is refused");
    CHECK(small[0] == (char)POISON, "and nothing was written");

    /* every walk of a whole frame must cover it exactly once */
    size_t covered = 0, lines = 0;
    for (size_t off = 0; format_hexdump_line(line, sizeof(line), data, sizeof(data), off); off += FORMAT_HEXDUMP_COLUMNS) {
        covered += (sizeof(data) - off < FORMAT_HEXDUMP_COLUMNS) ? sizeof(data) - off : FORMAT_HEXDUMP_COLUMNS;
        lines++;
    }
    CHECK(lines == 2, "18 bytes is two lines");
    CHECK(covered == sizeof(data), "and every byte was covered exactly once");
}

static void test_snprintf_inline(void) {
    printf("snprintf_inline: a formatted value where only an expression fits\n");
    char a[8], b[8];
    CHECK(strcmp(snprintf_inline(a, sizeof(a), "%04X", 0x5BF), "05BF") == 0, "formats and returns the buffer");
    /* two in one call is the case a shared static buffer would get wrong */
    char both[32];
    snprintf(both, sizeof(both), "%s/%s", snprintf_inline(a, sizeof(a), "%d", 1), snprintf_inline(b, sizeof(b), "%d", 2));
    CHECK(strcmp(both, "1/2") == 0, "two of them in one printf do not fight over a buffer");
    /* truncation is snprintf's, so it terminates */
    char t[4];
    CHECK(strcmp(snprintf_inline(t, sizeof(t), "%s", "abcdefg"), "abc") == 0, "truncates and terminates");
}

int main(void) {
    printf("d_format: text formatting into the caller's buffer\n\n");
    test_secs2str();
    test_hexdump_line();
    test_snprintf_inline();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
