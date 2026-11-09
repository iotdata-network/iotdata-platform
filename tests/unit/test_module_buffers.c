
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_module_buffers.c - host tests for d_module_buffers.h (the frame pool and the timed queue).
//
// The failure modes here are the reason this exists and the reason they are worth testing hard:
// a leaked reference exhausts the pool days later, a double unref hands a live frame to another
// holder, and exhaustion drops frames silently. None of the three is visible in a log.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "device/d_format.h" /* snprintf_inline, reached by buffer_queue_tag_name */
#include "device/d_module_buffers.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* dimensioned as a relay would: max frame + rssi + the largest header it prepends */
#define TEST_FRAME_MAX 240u
#define TEST_HDR_MAX   6u
#define TEST_STRIDE    (TEST_FRAME_MAX + 1u + TEST_HDR_MAX)
#define TEST_COUNT     12u

BUFFER_POOL_DECLARE(g_pool, TEST_COUNT, TEST_STRIDE);
BUFFER_QUEUE_DECLARE(g_queue, 8);

/* A POOL WIDER THAN ONE MAP WORD, which is the only thing that exercises the start-word hint in
   buffer_acquire: with count <= 32 there is one word, the hint is always 0, and the scan is what it
   always was -- and g_pool above is 12. 70 is three words, and deliberately NOT a multiple of 32,
   so the last word carries 26 padding bits that are not buffers and must never be handed out.
   The stride is tiny because nothing here writes bytes; it is the bookkeeping under test. */
#define WIDE_COUNT 70u
BUFFER_POOL_DECLARE(g_wide, WIDE_COUNT, 16u);

/* a caller's tag vocabulary, as the relay has one */
static const char *test_tag_name(const uint8_t tag) {
    return tag == 1 ? "BEACON" : tag == 2 ? "FORWARD" : NULL;
}

static void setup(void) {
    BUFFER_POOL_INIT(g_pool, TEST_COUNT, TEST_STRIDE, TEST_HDR_MAX);
    BUFFER_QUEUE_INIT(g_queue, 8, &g_pool);
}

// ------------------------------------------------------------------------------------------------------------------------

static void test_acquire_release(void) {
    printf("acquire / release accounting\n");
    setup();
    CHECK(buffer_pool_total(&g_pool) == TEST_COUNT, "total is what was dimensioned");
    CHECK(buffer_pool_used(&g_pool) == 0, "nothing in use");

    const buffer_handle_t a = buffer_acquire(&g_pool);
    CHECK(a != BUFFER_NONE, "acquired");
    CHECK(buffer_refs(&g_pool, a) == 1, "one reference");
    CHECK(buffer_pool_used(&g_pool) == 1, "one in use");
    buffer_unref(&g_pool, a);
    CHECK(buffer_pool_used(&g_pool) == 0, "returned");
    CHECK(buffer_refs(&g_pool, a) == 0, "and is no longer valid");

    /* every buffer, then one too many: exhaustion is a counted condition, not a crash */
    setup();
    buffer_handle_t all[TEST_COUNT];
    for (unsigned i = 0; i < TEST_COUNT; i++) {
        all[i] = buffer_acquire(&g_pool);
        CHECK(all[i] != BUFFER_NONE, "acquired one of the full set");
    }
    CHECK(buffer_pool_used(&g_pool) == TEST_COUNT, "full");
    CHECK(buffer_acquire(&g_pool) == BUFFER_NONE, "empty pool refuses");
    CHECK(buffer_pool_fails(&g_pool) == 1, "and counts the refusal");
    CHECK(buffer_pool_high_water(&g_pool) == TEST_COUNT, "high water recorded");
    for (unsigned i = 0; i < TEST_COUNT; i++)
        buffer_unref(&g_pool, all[i]);
    CHECK(buffer_pool_used(&g_pool) == 0, "all returned");
    CHECK(buffer_pool_high_water(&g_pool) == TEST_COUNT, "high water is a watermark, not a gauge");

    /* every handle must be distinct, or two holders share bytes */
    setup();
    bool seen[TEST_COUNT] = { false };
    for (unsigned i = 0; i < TEST_COUNT; i++) {
        const buffer_handle_t h = buffer_acquire(&g_pool);
        CHECK(h < TEST_COUNT && !seen[h], "a fresh buffer each time");
        seen[h] = true;
    }
}

static void test_refcounting(void) {
    printf("reference counting: several holders, one buffer\n");
    setup();
    const buffer_handle_t h = buffer_acquire(&g_pool);
    (void)buffer_ref(&g_pool, h); /* a second holder -- the queue */
    (void)buffer_ref(&g_pool, h); /* a third -- the ack tracker */
    CHECK(buffer_refs(&g_pool, h) == 3, "three holders");
    CHECK(buffer_pool_used(&g_pool) == 1, "still one buffer");

    buffer_unref(&g_pool, h);
    CHECK(buffer_pool_used(&g_pool) == 1, "one holder leaving does not free it");
    buffer_unref(&g_pool, h);
    CHECK(buffer_pool_used(&g_pool) == 1, "nor two");
    buffer_unref(&g_pool, h);
    CHECK(buffer_pool_used(&g_pool) == 0, "the last one does");

    /* an unref past zero must not corrupt the accounting: it is a bug at the call site, and the
       pool's job is to not compound it by handing the same buffer out twice */
    buffer_unref(&g_pool, h);
    CHECK(buffer_pool_used(&g_pool) == 0, "an extra unref changes nothing");
    const buffer_handle_t a = buffer_acquire(&g_pool);
    const buffer_handle_t b = buffer_acquire(&g_pool);
    CHECK(a != b, "and the pool still hands out distinct buffers");

    /* operations on an invalid handle are refused, not undefined */
    CHECK(buffer_data(&g_pool, BUFFER_NONE) == NULL, "no data for a non-handle");
    CHECK(buffer_len(&g_pool, BUFFER_NONE) == 0, "no length either");
    CHECK(buffer_refs(&g_pool, BUFFER_NONE) == 0, "no references");
    CHECK(!buffer_prepend(&g_pool, BUFFER_NONE, "xx", 2), "cannot prepend to nothing");
}

static void test_reserved_prefix(void) {
    printf("the reserved prefix: a header goes on without moving the payload\n");
    setup();
    const buffer_handle_t h = buffer_acquire(&g_pool);

    /* a receive lands past the prefix, so there is room in front of it */
    uint8_t *const rx = buffer_data(&g_pool, h);
    CHECK(rx != NULL, "have somewhere to receive");
    CHECK(buffer_room(&g_pool, h) == TEST_STRIDE - TEST_HDR_MAX, "room is the stride less the prefix");
    const uint8_t payload[9] = { 0x35, 0x38, 0x00, 0x01, 0x2C, 0xC1, 0x68, 0x97, 0x3E };
    memcpy(rx, payload, sizeof(payload));
    buffer_set_len(&g_pool, h, sizeof(payload));
    CHECK(buffer_len(&g_pool, h) == sizeof(payload), "length set");

    /* prepend a forwarding header: the payload must NOT have moved */
    const uint8_t hdr[TEST_HDR_MAX] = { 0xFF, 0x1B, 0x00, 0xE6, 0x10, 0x70 };
    CHECK(buffer_prepend(&g_pool, h, hdr, TEST_HDR_MAX), "prepended");
    CHECK(buffer_len(&g_pool, h) == sizeof(payload) + TEST_HDR_MAX, "length grew by the header");
    const uint8_t *const framed = buffer_data(&g_pool, h);
    CHECK(framed == rx - TEST_HDR_MAX, "the data start moved back, the payload did not move");
    CHECK(memcmp(framed, hdr, TEST_HDR_MAX) == 0, "the header is at the front");
    CHECK(memcmp(framed + TEST_HDR_MAX, payload, sizeof(payload)) == 0, "the payload follows it intact");

    /* more than the whole prefix, in one call: refused before anything is written. Checked as
       well as the spent-prefix case below because this is the bound that protects the pool -- `at`
       is unsigned, so an unguarded over-long prepend wraps and writes outside it. */
    const uint8_t toobig[TEST_HDR_MAX + 4] = { 0 };
    const uint16_t len_before = buffer_len(&g_pool, h);
    CHECK(!buffer_prepend(&g_pool, h, toobig, (uint16_t)sizeof(toobig)), "longer than the prefix is refused");
    CHECK(buffer_len(&g_pool, h) == len_before, "and nothing changed");

    /* the prefix is spent: a second header must be refused, not silently truncated */
    CHECK(!buffer_prepend(&g_pool, h, hdr, 1), "no prefix left, so refused");
    CHECK(buffer_len(&g_pool, h) == sizeof(payload) + TEST_HDR_MAX, "and nothing changed");

    /* a fresh buffer starts with its prefix again */
    buffer_unref(&g_pool, h);
    const buffer_handle_t h2 = buffer_acquire(&g_pool);
    CHECK(buffer_room(&g_pool, h2) == TEST_STRIDE - TEST_HDR_MAX, "prefix restored on acquire");
    CHECK(buffer_len(&g_pool, h2) == 0, "and it is empty");
    buffer_unref(&g_pool, h2);
}

/* Holding one buffer across cycles instead of acquiring and releasing one per cycle. The acquire
   is what restores the prefix and clears the length, so reuse has to do it explicitly -- a buffer
   prepended into once would otherwise stay shifted for the life of the run. */
static void test_reuse_without_reacquiring(void) {
    printf("reuse: a held buffer goes back to the state acquire would have given it\n");
    setup();
    const buffer_handle_t h = buffer_acquire(&g_pool);
    const uint8_t *const fresh = buffer_data(&g_pool, h);

    /* use it as the forward path does: receive, then prepend a header */
    buffer_set_len(&g_pool, h, 20);
    const uint8_t hdr[TEST_HDR_MAX] = { 1, 2, 3, 4, 5, 6 };
    CHECK(buffer_prepend(&g_pool, h, hdr, TEST_HDR_MAX), "prepended");
    CHECK(buffer_data(&g_pool, h) != fresh, "the data start has moved");
    CHECK(buffer_room(&g_pool, h) == TEST_STRIDE, "and the prefix is spent");

    /* reset, WITHOUT going back to the pool: same buffer, same state as a new one */
    CHECK(buffer_reset(&g_pool, h), "reset");
    CHECK(buffer_data(&g_pool, h) == fresh, "the payload start is back at the prefix");
    CHECK(buffer_room(&g_pool, h) == TEST_STRIDE - TEST_HDR_MAX, "so the prefix is available again");
    CHECK(buffer_len(&g_pool, h) == 0, "and it is empty");
    CHECK(buffer_prepend(&g_pool, h, hdr, TEST_HDR_MAX), "which means it can be prepended into again");
    CHECK(buffer_pool_acquires(&g_pool) == 1, "all of that on ONE acquire: the point of reuse");

    /* refused while anybody else holds it: moving the payload start under another holder is the
       corruption reference counting exists to prevent */
    buffer_reset(&g_pool, h);
    buffer_ref(&g_pool, h);
    CHECK(!buffer_reset(&g_pool, h), "refused with a second holder");
    buffer_unref(&g_pool, h);
    CHECK(buffer_reset(&g_pool, h), "and allowed again once it is ours alone");
    buffer_unref(&g_pool, h);
    CHECK(!buffer_reset(&g_pool, h), "and not on a buffer nobody holds");
    CHECK(buffer_pool_used(&g_pool) == 0, "released");
}

/*
 * WHAT IS ASSERTED IS THAT NOTHING IS STRANDED AND NOTHING IS HANDED OUT TWICE -- not which handle
 * comes back. The hint gives up "always the lowest free index" on purpose, so a test that pinned
 * the order would be testing the hint's current shape rather than the pool's contract. The one
 * ordering claim below is forced: with 69 of 70 held there is exactly one free bit anywhere, so any
 * correct scan must return it, wherever the hint happens to be sitting.
 */
static void test_pool_wider_than_one_word(void) {
    printf("a pool of %u, three map words: the scan wraps and the padding is not a buffer\n", (unsigned)WIDE_COUNT);
    BUFFER_POOL_INIT(g_wide, WIDE_COUNT, 16u, 0u);

    buffer_handle_t all[WIDE_COUNT];
    bool live[WIDE_COUNT];
    memset(live, 0, sizeof(live));
    unsigned unreachable = 0, padding = 0, twice = 0;
    for (unsigned i = 0; i < WIDE_COUNT; i++) {
        all[i] = buffer_acquire(&g_wide);
        if (all[i] == BUFFER_NONE) {
            unreachable++;
            continue;
        }
        if (all[i] >= WIDE_COUNT) {
            padding++;
            continue;
        }
        if (live[all[i]])
            twice++;
        live[all[i]] = true;
    }
    CHECK(unreachable == 0, "every one of the 70 is reachable");
    CHECK(padding == 0, "and is a real buffer, never a padding bit of the last word");
    CHECK(twice == 0, "and is handed out exactly once");
    CHECK(buffer_pool_used(&g_wide) == WIDE_COUNT, "the pool is now full");
    CHECK(buffer_acquire(&g_wide) == BUFFER_NONE, "a full pool refuses rather than minting a padding bit");

    /* THE WRAP. The hint is in the LAST word -- that is where the last acquire succeeded -- and the
       buffer freed here is in the FIRST. A scan that started at the hint and ran to the end without
       coming round would report an empty pool while a buffer sat free in it. */
    buffer_unref(&g_wide, all[3]);
    CHECK(buffer_pool_used(&g_wide) == WIDE_COUNT - 1, "one buffer free, in the first word");
    CHECK(buffer_acquire(&g_wide) == all[3], "and the scan comes round to it");

    /* churn across all three words: a hint left out of range, or one that stopped the scan short,
       shows up here as a refusal or a double hand-out rather than as a slow path */
    unsigned churn_fails = 0;
    for (unsigned round = 0; round < 500; round++) {
        const unsigned k = (round * 7u) % WIDE_COUNT; /* strides across the words rather than along one */
        buffer_unref(&g_wide, all[k]);
        live[all[k]] = false;
        all[k] = buffer_acquire(&g_wide);
        if (all[k] == BUFFER_NONE || all[k] >= WIDE_COUNT || live[all[k]])
            churn_fails++;
        else
            live[all[k]] = true;
    }
    CHECK(churn_fails == 0, "500 rounds of free-and-reacquire across the words, none refused or duplicated");
    for (unsigned i = 0; i < WIDE_COUNT; i++)
        buffer_unref(&g_wide, all[i]);
    CHECK(buffer_pool_used(&g_wide) == 0, "and it all comes back");
    CHECK(buffer_pool_fails(&g_wide) == 1, "the only refusal was the deliberate one");
}

// ------------------------------------------------------------------------------------------------------------------------

/* The three calls a polling loop makes on EVERY pass, on a queue with nothing in it: each answers
   nothing, and -- the part worth a test -- the early-out that lets them answer without walking the
   slots does not then swallow the work when there IS something in there. */
static void test_empty_queue(void) {
    printf("an empty queue answers every question, and one with a frame in it is still walked\n");
    setup();
    CHECK(buffer_queue_count(&g_queue) == 0, "empty to begin with");
    CHECK(buffer_queue_take(&g_queue, 1000, NULL, NULL) == BUFFER_NONE, "take finds nothing");
    CHECK(buffer_queue_peek(&g_queue, 1000, NULL, NULL) == BUFFER_NONE, "peek finds nothing");
    CHECK(buffer_queue_expire(&g_queue, 1000) == 0, "expire finds nothing");
    CHECK(buffer_queue_expired(&g_queue) == 0, "and counts nothing expired");

    const buffer_handle_t h = buffer_acquire(&g_pool);
    CHECK(buffer_queue_add(&g_queue, h, 0, 100, 1, 0x1234), "one frame queued, expiring at 100");
    buffer_unref(&g_pool, h);
    CHECK(buffer_queue_peek(&g_queue, 50, NULL, NULL) == h, "peek walks a queue that has something in it");
    CHECK(buffer_queue_expire(&g_queue, 200) == 1, "and expire still expires it");
    CHECK(buffer_pool_used(&g_pool) == 0, "the reference went with it");
    CHECK(buffer_queue_count(&g_queue) == 0, "leaving it empty again");
}

// ------------------------------------------------------------------------------------------------------------------------

static void test_queue(void) {
    printf("the queue: due, expiry, and who holds the reference\n");
    setup();
    const buffer_handle_t h = buffer_acquire(&g_pool);
    buffer_set_len(&g_pool, h, 10);

    /* queueing takes a reference; the caller still owns its own */
    CHECK(buffer_queue_add(&g_queue, h, 100, 5000, 1, 0xABCD), "queued");
    CHECK(buffer_refs(&g_pool, h) == 2, "the queue holds a reference of its own");
    buffer_unref(&g_pool, h); /* the caller is done with it */
    CHECK(buffer_pool_used(&g_pool) == 1, "the queue keeps it alive");

    /* not due yet */
    uint8_t tag = 0;
    uint32_t key = 0;
    CHECK(buffer_queue_take(&g_queue, 50, &tag, &key) == BUFFER_NONE, "nothing due before its time");
    CHECK(buffer_queue_count(&g_queue) == 1, "and it is still queued");

    /* peek answers the same question as take, and CHANGES NOTHING -- no removal, and no reference
       transferred, which is the part that would leak or double-free if it were got wrong */
    CHECK(buffer_queue_peek(&g_queue, 50, &tag, &key) == BUFFER_NONE, "peek respects the due time too");
    CHECK(buffer_queue_peek(&g_queue, 150, &tag, &key) == h, "peek sees what take would hand over");
    CHECK(tag == 1 && key == 0xABCD, "with its tag and key");
    CHECK(buffer_queue_count(&g_queue) == 1, "peek did not remove it");
    CHECK(buffer_refs(&g_pool, h) == 1, "and took no reference: still only the queue's");

    /* due: the reference travels to the caller */
    const buffer_handle_t got = buffer_queue_take(&g_queue, 150, &tag, &key);
    CHECK(got == h, "the frame came back");
    CHECK(tag == 1 && key == 0xABCD, "with its tag and key");
    CHECK(buffer_queue_count(&g_queue) == 0, "no longer queued");
    CHECK(buffer_refs(&g_pool, got) == 1, "and the queue's reference is now ours");
    buffer_unref(&g_pool, got);
    CHECK(buffer_pool_used(&g_pool) == 0, "released");

    /* expiry frees the frame, and only the stale one */
    setup();
    const buffer_handle_t a = buffer_acquire(&g_pool), b = buffer_acquire(&g_pool), c = buffer_acquire(&g_pool);
    CHECK(buffer_queue_add(&g_queue, a, 0, 100, 1, 1), "a queued");
    CHECK(buffer_queue_add(&g_queue, b, 0, 9000, 1, 2), "b queued");
    CHECK(buffer_queue_add(&g_queue, c, 0, 0, 1, 3), "c queued, never expires");
    buffer_unref(&g_pool, a);
    buffer_unref(&g_pool, b);
    buffer_unref(&g_pool, c);
    CHECK(buffer_pool_used(&g_pool) == 3, "the queue holds all three");
    CHECK(buffer_queue_expire(&g_queue, 50) == 0, "nothing stale yet");
    CHECK(buffer_queue_expire(&g_queue, 200) == 1, "a expired");
    CHECK(buffer_pool_used(&g_pool) == 2, "and its buffer went back");
    CHECK(buffer_queue_expire(&g_queue, 200) == 0, "not twice");
    CHECK(buffer_queue_expire(&g_queue, 100000) == 1, "b expired; c never does");
    CHECK(buffer_pool_used(&g_pool) == 1, "c alone remains");
    CHECK(buffer_queue_expired(&g_queue) == 2, "two expiries counted");

    /* remove by key, e.g. an ACK arriving */
    CHECK(buffer_queue_remove_key(&g_queue, 1, 3), "removed by key");
    CHECK(buffer_pool_used(&g_pool) == 0, "and released");
    CHECK(!buffer_queue_remove_key(&g_queue, 1, 3), "and not again");

    /* a full queue rejects and takes NO reference, so a failed add leaves ownership unchanged */
    setup();
    buffer_handle_t held[8];
    for (unsigned i = 0; i < 8; i++) {
        held[i] = buffer_acquire(&g_pool);
        CHECK(buffer_queue_add(&g_queue, held[i], 0, 0, 1, i), "filled a slot");
    }
    const buffer_handle_t extra = buffer_acquire(&g_pool);
    CHECK(!buffer_queue_add(&g_queue, extra, 0, 0, 1, 99), "a full queue rejects");
    CHECK(buffer_queue_rejected(&g_queue) == 1, "and counts it");
    CHECK(buffer_refs(&g_pool, extra) == 1, "taking no reference of its own");
    buffer_unref(&g_pool, extra);
    CHECK(buffer_pool_used(&g_pool) == 8, "so the rejected frame went back");

    /* the queue's own depth high-water: not inferable from the pool's, because the pool counts
       every holder and the queue is only one of them */
    CHECK(buffer_queue_high_water(&g_queue) == 8, "the queue reached its capacity");
    CHECK(buffer_pool_high_water(&g_pool) >= 9, "while the pool saw more than that");

    /* clearing must not leak what it was holding */
    buffer_queue_clear(&g_queue);
    for (unsigned i = 0; i < 8; i++)
        buffer_unref(&g_pool, held[i]);
    CHECK(buffer_pool_used(&g_pool) == 0, "clear released the queue's references");

    /* the tag resolver is generic and optional: named when set, the number when not */
    CHECK(strcmp(buffer_queue_tag_name(&g_queue, 3), "tag3") == 0, "unnamed tags render as numbers");
    buffer_queue_set_tag_name(&g_queue, test_tag_name);
    CHECK(strcmp(buffer_queue_tag_name(&g_queue, 1), "BEACON") == 0, "and by name once set");
    CHECK(strcmp(buffer_queue_tag_name(&g_queue, 99), "tag99") == 0, "a resolver may decline a tag");
    buffer_queue_set_tag_name(&g_queue, NULL);

    /* earliest-due first, not insertion order */
    setup();
    const buffer_handle_t late = buffer_acquire(&g_pool), soon = buffer_acquire(&g_pool);
    CHECK(buffer_queue_add(&g_queue, late, 900, 0, 2, 10), "late queued first");
    CHECK(buffer_queue_add(&g_queue, soon, 100, 0, 2, 20), "soon queued second");
    buffer_unref(&g_pool, late);
    buffer_unref(&g_pool, soon);
    CHECK(buffer_queue_peek(&g_queue, 1000, NULL, NULL) == soon, "peek agrees on which is next");
    CHECK(buffer_queue_take(&g_queue, 1000, &tag, &key) == soon, "the earliest due came out first");
    CHECK(key == 20, "with its key");
    CHECK(buffer_queue_peek(&g_queue, 1000, NULL, NULL) == late, "and then the later one is next");
}

static void test_no_leaks_over_churn(void) {
    printf("churn: a receive-forward-retransmit cycle must not leak\n");
    setup();
    for (unsigned cycle = 0; cycle < 1000; cycle++) {
        const buffer_handle_t h = buffer_acquire(&g_pool);
        CHECK(h != BUFFER_NONE, "acquired");
        buffer_set_len(&g_pool, h, 20);
        (void)buffer_prepend(&g_pool, h, "HDRHDR", 6);
        /* queued for transmit AND held for a possible retransmit: two holders */
        CHECK(buffer_queue_add(&g_queue, h, cycle, cycle + 100, 1, cycle), "queued");
        const buffer_handle_t keep = buffer_ref(&g_pool, h);
        buffer_unref(&g_pool, h); /* the receiver is done */
        uint8_t tag = 0;
        uint32_t key = 0;
        const buffer_handle_t tx = buffer_queue_take(&g_queue, cycle, &tag, &key);
        CHECK(tx == h, "transmitted the one we queued");
        buffer_unref(&g_pool, tx);   /* transmit done */
        buffer_unref(&g_pool, keep); /* ACK arrived */
        CHECK(buffer_pool_used(&g_pool) == 0, "nothing held at the end of a cycle");
    }
    CHECK(buffer_pool_high_water(&g_pool) <= 1, "one buffer was ever enough for this pattern");
    CHECK(buffer_pool_fails(&g_pool) == 0, "and it never ran dry");
    CHECK(buffer_pool_acquires(&g_pool) == 1000, "every cycle accounted for");
}

/* The receive loop as both the relay and the gateway now run it: ONE buffer held across cycles,
   reset before each read, given up only when a frame is handed on to somebody who keeps it. The
   invariant this pins down is that the held buffer is always receive-ready -- a cycle that
   prepended a forwarding header and then failed to queue it leaves the payload start moved, and
   without the reset the NEXT read lands in the wrong place and no header can be prepended again.
   Lives here rather than in either app because the rule belongs to the pool, and neither app's
   loop is reachable from a host test. */
static void test_reuse_across_receive_cycles(void) {
    printf("receive cycles: one held buffer, reset each time, released only on hand-off\n");
    setup();
    buffer_handle_t held = BUFFER_NONE;
    unsigned handed = 0, kept_holders = 0;
    for (unsigned cycle = 0; cycle < 1000; cycle++) {
        if (held == BUFFER_NONE)
            held = buffer_acquire(&g_pool);
        CHECK(held != BUFFER_NONE, "have a buffer to receive into");
        CHECK(buffer_reset(&g_pool, held), "reset before the read: we are its only holder");
        CHECK(buffer_room(&g_pool, held) == TEST_STRIDE - TEST_HDR_MAX, "so it is receive-ready, prefix and all");

        /* the read */
        uint8_t *const rx = buffer_data(&g_pool, held);
        memset(rx, (int)(cycle & 0xFF), 20);
        buffer_set_len(&g_pool, held, 20);

        /* every third frame is forwarded: prepend a header, then queue it -- and the queue is
           full every tenth time, which is the case that leaves a prepended buffer with no taker */
        if (cycle % 3 == 0) {
            CHECK(buffer_prepend(&g_pool, held, "HDRHDR", TEST_HDR_MAX), "forwarding header prepended");
            if (cycle % 10 != 0) {
                CHECK(buffer_queue_add(&g_queue, held, 0, 0, 1, cycle), "queued for transmit");
                handed++;
            }
        }

        /* the rule: keep it unless somebody else is holding this frame */
        if (buffer_refs(&g_pool, held) > 1) {
            buffer_unref(&g_pool, held);
            held = BUFFER_NONE;
            kept_holders++;
        }

        /* the transmit side drains what it was given */
        uint32_t key = 0;
        buffer_handle_t tx;
        while ((tx = buffer_queue_take(&g_queue, 0, NULL, &key)) != BUFFER_NONE) {
            CHECK(buffer_len(&g_pool, tx) == 20 + TEST_HDR_MAX, "a forwarded frame kept its header and payload");
            buffer_unref(&g_pool, tx);
        }
    }
    if (held != BUFFER_NONE)
        buffer_unref(&g_pool, held);
    printf("  %u cycles, %u frames handed on, %" PRIu32 " acquires\n", 1000u, handed, buffer_pool_acquires(&g_pool));
    CHECK(handed == kept_holders, "every frame handed on ended the reuse exactly once");
    /* Acquires track HAND-OFFS, not cycles: the first read needs a buffer, and thereafter only a
       frame somebody kept costs a new one. The +/-1 is just whether the last cycle happened to
       hand off (leaving nothing to re-acquire), so the relationship is what gets asserted. */
    CHECK(buffer_pool_acquires(&g_pool) >= handed && buffer_pool_acquires(&g_pool) <= handed + 1, "one acquire per hand-off, give or take the first");
    CHECK(buffer_pool_acquires(&g_pool) < 1000, "far fewer than one per cycle -- the point of the whole thing");
    CHECK(buffer_pool_used(&g_pool) == 0, "and nothing leaked across a thousand cycles");
    CHECK(buffer_pool_fails(&g_pool) == 0, "the pool never ran dry");
}

int main(void) {
    printf("d_module_buffers: pool + queue\n\n");
    test_acquire_release();
    test_refcounting();
    test_reserved_prefix();
    test_reuse_without_reacquiring();
    test_pool_wider_than_one_word();
    test_empty_queue();
    test_queue();
    test_no_leaks_over_churn();
    test_reuse_across_receive_cycles();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
