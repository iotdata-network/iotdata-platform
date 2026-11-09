// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_module_buffers_locked.c - d_module_buffers.h built WITH locking, and actually run threaded.
//
// The unlocked tests next door prove the accounting. This one proves the guard: that the free map,
// the reference counts and the counters survive concurrent acquire/unref. It is a separate TU
// because the locking is a compile-time choice -- the caller defines the four macros or gets
// nothing -- so the two builds cannot coexist in one file.
//
// The exclusivity check is the point. A race in the free map does not corrupt a counter, it hands
// ONE buffer to TWO threads, and on a relay that surfaces as a garbled frame on air that looks
// exactly like a radio problem. So each thread stamps its whole buffer with its own id, waits, and
// reads it back: if another thread were handed the same buffer, the stamp would not survive.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#define BUFFER_LOCK_TYPE       pthread_mutex_t
#define BUFFER_LOCK_INIT(l)    pthread_mutex_init((l), NULL)
#define BUFFER_LOCK_ACQUIRE(l) pthread_mutex_lock(l)
#define BUFFER_LOCK_RELEASE(l) pthread_mutex_unlock(l)
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

#define TEST_STRIDE 64u
#define TEST_COUNT  8u
#define THREADS     4
#define ITERATIONS  20000

BUFFER_POOL_DECLARE(g_pool, TEST_COUNT, TEST_STRIDE);

typedef struct {
    uint8_t id;
    uint32_t got;     /* acquires that succeeded */
    uint32_t missed;  /* pool was empty -- expected under contention, not an error */
    uint32_t stomped; /* somebody else wrote our buffer: the failure this test exists for */
} worker_t;

static void *worker(void *arg) {
    worker_t *const w = (worker_t *)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        const buffer_handle_t h = buffer_acquire(&g_pool);
        if (h == (buffer_handle_t)BUFFER_NONE) {
            w->missed++;
            continue;
        }
        w->got++;
        uint8_t *const p = buffer_data(&g_pool, h);
        const uint16_t room = buffer_room(&g_pool, h);
        memset(p, w->id, room);
        /* give any other thread holding the same buffer a chance to overwrite the stamp */
        sched_yield();
        for (uint16_t k = 0; k < room; k++)
            if (p[k] != w->id) {
                w->stomped++;
                break;
            }
        buffer_unref(&g_pool, h);
    }
    return NULL;
}

/* A buffer crossing a thread boundary through a shared QUEUE, which is the gateway's real shape:
   the mosquitto callback builds a command and queues it, the main loop takes it and releases it.
   There is deliberately NO mutex of our own here -- the queue carries its own lock, and the point
   of the test is that this is enough. If it were not, the accounting below would not balance and
   the payload check would not hold.

   Note what is NOT claimed: the queue guards each CALL, so a compound sequence still belongs to
   the caller. This producer needs no such sequence, which is why it needs no mutex. */
#define HANDOFFS    5000
#define QUEUE_DEPTH 4
#define TAG_COMMAND 1

BUFFER_QUEUE_DECLARE(g_queue, QUEUE_DEPTH);

static uint32_t g_queued = 0, g_refused = 0;   /* producer side */
static uint32_t g_received = 0, g_corrupt = 0; /* consumer side */
static uint32_t g_peek_lied = 0;               /* consumer side: peek saw a frame that take then missed */
static volatile bool g_producer_done = false;
static bool g_consumer_gave_up = false;

/* The consumer's exit condition reads queue state, so a broken guard can corrupt the very thing
   that ends the loop -- removing the queue's lock makes this test HANG rather than fail. A hang
   reads as a broken build, not as a caught regression, so the wait is bounded: past the bound it
   is a FAIL with the reason named. */
#define IDLE_SPINS_MAX 10000000

static void *producer(__attribute__((unused)) void *arg) {
    for (int i = 0; i < HANDOFFS; i++) {
        const buffer_handle_t h = buffer_acquire(&g_pool);
        if (h == (buffer_handle_t)BUFFER_NONE) {
            g_refused++; /* pool empty: the consumer is behind, which is a real condition */
            sched_yield();
            continue;
        }
        uint8_t *const p = buffer_data(&g_pool, h);
        p[0] = 0xA5;
        p[1] = (uint8_t)(i & 0xFF);
        p[2] = (uint8_t)~(i & 0xFF); /* a checkable relationship, not just a constant */
        buffer_set_len(&g_pool, h, 3);
        if (buffer_queue_add(&g_queue, h, 0, 0, TAG_COMMAND, (uint32_t)i))
            g_queued++;
        else
            g_refused++;          /* queue full: told, not silently superseded */
        buffer_unref(&g_pool, h); /* the queue took its own reference */
    }
    g_producer_done = true;
    return NULL;
}

static void *consumer(__attribute__((unused)) void *arg) {
    uint32_t idle = 0;
    for (;;) {
        uint32_t key = 0;
        uint8_t tag = 0;
        /* PEEK LOCKS, so this drives that path with the producer contending for the same lock.
           The invariant is sharp on this queue: nothing expires (the producer queues with expiry 0)
           and the consumer is the only remover, so a frame peek just saw cannot have vanished by
           the take below. On a queue two threads TOOK from it could, which is the staleness the
           lock does not fix and the header says so. */
        const bool peeked = buffer_queue_peek(&g_queue, 0, NULL, NULL) != (buffer_handle_t)BUFFER_NONE;
        const buffer_handle_t h = buffer_queue_take(&g_queue, 0, &tag, &key);
        if (peeked && h == (buffer_handle_t)BUFFER_NONE)
            g_peek_lied++;
        if (h == (buffer_handle_t)BUFFER_NONE) {
            if (g_producer_done && buffer_queue_count(&g_queue) == 0)
                break;
            if (++idle > IDLE_SPINS_MAX) {
                g_consumer_gave_up = true;
                break;
            }
            sched_yield();
            continue;
        }
        idle = 0;
        const uint8_t *const p = buffer_data(&g_pool, h);
        if (tag != TAG_COMMAND || buffer_len(&g_pool, h) != 3 || p[0] != 0xA5 || p[1] != (uint8_t)(key & 0xFF) || p[2] != (uint8_t)~(key & 0xFF))
            g_corrupt++; /* a torn handoff: the frame is not the one that was queued */
        else
            g_received++;
        buffer_unref(&g_pool, h); /* the reference the queue handed over */
    }
    return NULL;
}

static void test_concurrent_acquire_release(void) {
    printf("concurrent acquire/release: one buffer is never handed to two threads\n");
    pthread_t t[THREADS];
    worker_t w[THREADS];
    for (int i = 0; i < THREADS; i++) {
        memset(&w[i], 0, sizeof(w[i]));
        w[i].id = (uint8_t)(0x10 + i);
        CHECK(pthread_create(&t[i], NULL, worker, &w[i]) == 0, "thread started");
    }
    uint32_t got = 0, missed = 0, stomped = 0;
    for (int i = 0; i < THREADS; i++) {
        (void)pthread_join(t[i], NULL);
        got += w[i].got;
        missed += w[i].missed;
        stomped += w[i].stomped;
    }
    printf("  %" PRIu32 " acquired, %" PRIu32 " missed (pool empty), high_water=%u\n", got, missed, (unsigned)buffer_pool_high_water(&g_pool));
    CHECK(stomped == 0, "no buffer was held by two threads at once");
    CHECK(buffer_pool_used(&g_pool) == 0, "every buffer came back");
    CHECK(got + missed == (uint32_t)(THREADS * ITERATIONS), "every attempt accounted for");
    CHECK(buffer_pool_acquires(&g_pool) == got, "the pool counted exactly the successes");
    CHECK(buffer_pool_fails(&g_pool) == missed, "and exactly the empty-pool failures");
    CHECK(buffer_pool_high_water(&g_pool) <= TEST_COUNT, "high water never exceeds the pool");
}

static void test_cross_thread_queue(void) {
    printf("cross-thread queue, no lock of our own: producer queues, consumer takes\n");
    BUFFER_QUEUE_INIT(g_queue, QUEUE_DEPTH, &g_pool);
    pthread_t tp, tc;
    CHECK(pthread_create(&tp, NULL, producer, NULL) == 0, "producer started");
    CHECK(pthread_create(&tc, NULL, consumer, NULL) == 0, "consumer started");
    (void)pthread_join(tp, NULL);
    (void)pthread_join(tc, NULL);
    printf("  %" PRIu32 " queued, %" PRIu32 " refused (full), %" PRIu32 " received intact, queue high_water=%u\n", g_queued, g_refused, g_received, (unsigned)buffer_queue_high_water(&g_queue));
    CHECK(!g_consumer_gave_up, "the consumer drained the queue rather than giving up waiting");
    CHECK(g_corrupt == 0, "every frame taken was the frame that was queued");
    CHECK(g_peek_lied == 0, "a frame peek saw was still there to take, under contention");
    CHECK(g_queued + g_refused == HANDOFFS, "every request either queued or was refused");
    CHECK(g_received == g_queued, "and everything queued came back out exactly once");
    CHECK(g_queued > 0 && g_received > 0, "the pair actually did some work");
    CHECK(buffer_queue_count(&g_queue) == 0, "queue drained");
    CHECK(buffer_queue_high_water(&g_queue) <= QUEUE_DEPTH, "never deeper than its capacity");
    CHECK(buffer_queue_added(&g_queue) == g_queued, "the queue counted the same adds we did");
    CHECK(buffer_pool_used(&g_pool) == 0, "no buffer left behind: not by the queue, not by either thread");
}

int main(void) {
    printf("d_module_buffers: locked build, threaded\n\n");
    BUFFER_POOL_INIT(g_pool, TEST_COUNT, TEST_STRIDE, 0);
    test_concurrent_acquire_release();
    test_cross_thread_queue();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
