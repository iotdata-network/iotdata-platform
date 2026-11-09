//
// test_node_crc64.c - CRC-64/XZ, against the reference implementation rather than against a
// remembered constant.
//
// THE POINT OF THIS SUITE. §7 chose a CRC partly because "it has published test vectors, so a
// gateway in C on Linux and a sensor on an 8-bit part can be PROVEN to agree". That promise is only
// worth something if the vectors are right, and a check value quoted from memory is exactly how it
// goes wrong: the first draft of this work carried 0x995DC9BBDEFB9D7A, which is not the CRC-64/XZ
// check value at all. So three independent anchors instead:
//
//   1. the POLYNOMIAL, anchored on CRC-64/ECMA-182's published check value in its NORMAL form --
//      a completely different code path to the one under test, which pins the polynomial even if
//      the reflected implementation is wrong
//   2. the TABLE, regenerated here bitwise from that polynomial, so a typo in a literal cannot live
//   3. the VECTORS, compared against liblzma's own lzma_crc64() when it can be dlopened -- the
//      implementation XZ itself ships. Skipped, loudly, where it cannot be found.
//
// Built twice by the Makefile, with the table and without it, because an untested alternative
// implementation is worse than no alternative.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <dlfcn.h>

/* CRC-64 moved into d_common.h, which wants a platform under it for its timer and delay helpers.
   This suite calls neither -- the host platform is here only so the checksum compiles. */
#include "d_platform_linux.h"
#include "d_common.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

/* Every vector here came out of lzma_crc64(). None was typed from memory. */
static const struct {
    const char *what;
    const uint8_t *data;
    size_t len;
    uint64_t crc;
} VECTORS[] = {
#define V(what, lit, crc) { what, (const uint8_t *)(lit), sizeof(lit) - 1u, crc }
    V("nothing at all", "", 0x0000000000000000ULL),
    V("\"a\"", "a", 0x330284772E652B05ULL),
    V("\"123456789\" -- the catalogue's check string", "123456789", 0x995DC9BBDF1939FAULL),
    V("the quick brown fox", "The quick brown fox jumps over the lazy dog", 0x5B5EB8C2E54AA1C4ULL),
#undef V
};

/* ------------------------------------------------------------------------------------------- */

static void test_vectors(void) {
    printf("\nthe vectors liblzma produced\n");
    for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++)
        CHECK(_iotdata_crc64(VECTORS[i].data, VECTORS[i].len, _IOTDATA_CRC64_INIT) == VECTORS[i].crc, VECTORS[i].what);

    /* Two that a string literal cannot carry: every byte value, which is the only way a bad table
       entry in the top half shows up, and a long run of zeros -- whose CRC is emphatically NOT zero,
       which is the property that makes a CRC worth running over a padded object at all. */
    uint8_t all[256];
    for (size_t i = 0; i < sizeof(all); i++)
        all[i] = (uint8_t)i;
    CHECK(_iotdata_crc64(all, sizeof(all), _IOTDATA_CRC64_INIT) == 0x72414B2F65DB3AB0ULL, "every byte 0x00..0xFF");

    uint8_t zeros[1000];
    memset(zeros, 0, sizeof(zeros));
    CHECK(_iotdata_crc64(zeros, sizeof(zeros), _IOTDATA_CRC64_INIT) == 0x372AE22BEC8A254FULL, "1000 zero bytes");
    CHECK(_iotdata_crc64(zeros, sizeof(zeros), _IOTDATA_CRC64_INIT) != 0u, "and zeros do not hash to zero, which is the point");

    uint8_t ones[1024];
    memset(ones, 0xFF, sizeof(ones));
    CHECK(_iotdata_crc64(ones, sizeof(ones), _IOTDATA_CRC64_INIT) == 0x389A86ECA26C62DFULL, "1024 x 0xFF");
}

/* ------------------------------------------------------------------------------------------- */

static void test_incremental(void) {
    printf("\na run at a time gives the same answer as all of it at once\n");
    uint8_t all[256];
    for (size_t i = 0; i < sizeof(all); i++)
        all[i] = (uint8_t)i;
    const uint64_t whole = _iotdata_crc64(all, sizeof(all), _IOTDATA_CRC64_INIT);

    /* EVERY split, not a chosen one: the convention's whole claim is that the running value is the
       answer so far, and a boundary that only works at a multiple of something would still pass a
       single hand-picked split. */
    bool every = true;
    for (size_t cut = 0; cut <= sizeof(all); cut++) {
        const uint64_t partial = _iotdata_crc64(all, cut, _IOTDATA_CRC64_INIT);
        if (_iotdata_crc64(all + cut, sizeof(all) - cut, partial) != whole)
            every = false;
    }
    CHECK(every, "all 257 split points agree with the one-shot value");

    /* Byte at a time, which is what a console sink does when a line boundary lands badly. */
    uint64_t crc = _IOTDATA_CRC64_INIT;
    for (size_t i = 0; i < sizeof(all); i++)
        crc = _iotdata_crc64(&all[i], 1u, crc);
    CHECK(crc == whole, "one byte at a time agrees too");

    CHECK(_iotdata_crc64(NULL, 0u, 0x1234ULL) == 0x1234ULL, "a NULL buffer leaves the value alone");
    CHECK(_iotdata_crc64(all, 0u, 0x1234ULL) == 0x1234ULL, "and so does a zero length -- a short final batch needs no special case");
    CHECK(_iotdata_crc64(NULL, 0u, _IOTDATA_CRC64_INIT) == 0u, "no bytes at all is 0, which liblzma agrees with");
}

/* ------------------------------------------------------------------------------------------- */

/* ANCHOR 1: the polynomial, through a path that shares no code with the thing under test. The
   normal (unreflected, init 0, no xorout) form of the same polynomial is CRC-64/ECMA-182, whose
   published check value for "123456789" is 0x6C40DF5F0B497347. If this passes, the polynomial is
   right -- whatever the reflected implementation happens to be doing. */
static void test_polynomial(void) {
    printf("\nthe polynomial itself, anchored on ECMA-182 rather than on our own code\n");
    const uint64_t normal = 0x42F0E1EBA9EA3693ULL;

    uint64_t crc = 0u;
    const char *s = "123456789";
    for (const char *p = s; *p != '\0'; p++) {
        crc ^= (uint64_t)(uint8_t)*p << 56;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000000000000000ULL) ? ((crc << 1) ^ normal) : (crc << 1);
    }
    CHECK(crc == 0x6C40DF5F0B497347ULL, "normal form reproduces ECMA-182's published check value");

    /* And that _IOTDATA_CRC64_POLY really is that polynomial reversed, rather than a constant
       copied from somewhere that happened to look right. */
    uint64_t reversed = 0u;
    for (int i = 0; i < 64; i++)
        if (normal & (1ULL << i))
            reversed |= 1ULL << (63 - i);
    CHECK(reversed == _IOTDATA_CRC64_POLY, "_IOTDATA_CRC64_POLY is that polynomial, bit-reversed");
}

/* ------------------------------------------------------------------------------------------- */

/* ANCHOR 2: the table. Four bit-steps per nibble, straight from the polynomial, compared with
   whatever the header has written down. Only meaningful in a table build; in a bitwise build there
   is nothing to compare and the loop under test IS the generator, which anchors 1 and 3 cover. */
static void test_table(void) {
#if _IOTDATA_CRC64_TABLE
    printf("\nthe nibble table regenerated from the polynomial, entry by entry\n");
    bool all_agree = true;
    for (unsigned i = 0; i < 16u; i++) {
        uint64_t t = i;
        for (int step = 0; step < 4; step++)
            t = (t & 1u) ? ((t >> 1) ^ _IOTDATA_CRC64_POLY) : (t >> 1);
        if (t != _iotdata_crc64_table[i]) {
            printf("  FAIL: table[%u] is 0x%016llX, the polynomial says 0x%016llX\n", i, (unsigned long long)_iotdata_crc64_table[i], (unsigned long long)t);
            all_agree = false;
            fails++;
        }
    }
    CHECK(all_agree, "all 16 entries are what the polynomial generates");
    CHECK(_iotdata_crc64_table[0] == 0u, "and entry 0 is zero, as it must be");
#else
    printf("\nbitwise build: no table to regenerate\n");
#endif
}

/* ------------------------------------------------------------------------------------------- */

/* ANCHOR 3: liblzma, which is the implementation XZ ships. dlopen rather than a link dependency, so
   a host without it SKIPS loudly instead of failing to build -- the same bargain the OTA suite
   strikes with ota-bundle's source. */
static void test_against_liblzma(void) {
    printf("\nand agreement with liblzma's own lzma_crc64, where it can be found\n");
    void *so = dlopen("liblzma.so.5", RTLD_LAZY);
    if (so == NULL)
        so = dlopen("liblzma.so", RTLD_LAZY);
    if (so == NULL) {
        printf("  SKIP: liblzma not available here\n");
        return;
    }
    /* The cast through a union of function pointers that a strict build wants: dlsym returns an
       object pointer, and assigning it to a function pointer is not something ISO C blesses. */
    union {
        void *obj;
        uint64_t (*fn)(const uint8_t *, size_t, uint64_t);
    } sym;
    sym.obj = dlsym(so, "lzma_crc64");
    if (sym.obj == NULL) {
        printf("  SKIP: liblzma has no lzma_crc64\n");
        (void)dlclose(so);
        return;
    }

    uint8_t buf[517]; /* a deliberately awkward length: prime, and not a multiple of anything */
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 7u + 11u);

    CHECK(_iotdata_crc64(buf, sizeof(buf), _IOTDATA_CRC64_INIT) == sym.fn(buf, sizeof(buf), 0u), "517 awkward bytes agree with the reference");

    bool prefixes = true, splits = true;
    for (size_t n = 0; n <= sizeof(buf); n++)
        if (_iotdata_crc64(buf, n, _IOTDATA_CRC64_INIT) != sym.fn(buf, n, 0u))
            prefixes = false;
    CHECK(prefixes, "and so does every prefix of them, all 518");

    /* The convention claim, against the reference: feeding a run into a value the reference
       produced must land where the reference lands. If the entry/exit complementing were wrong,
       one-shot could still agree while this did not. */
    for (size_t cut = 0; cut <= sizeof(buf); cut++)
        if (_iotdata_crc64(buf + cut, sizeof(buf) - cut, sym.fn(buf, cut, 0u)) != sym.fn(buf, sizeof(buf), 0u))
            splits = false;
    CHECK(splits, "a run fed into the reference's own running value continues it correctly");

    for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++)
        CHECK(sym.fn(VECTORS[i].data, VECTORS[i].len, 0u) == VECTORS[i].crc, "the written-down vector matches the reference");

    (void)dlclose(so);
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_crc64 (table=%d)\n", _IOTDATA_CRC64_TABLE);
    test_polynomial();
    test_table();
    test_vectors();
    test_incremental();
    test_against_liblzma();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
