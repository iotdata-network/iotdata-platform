// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_state.c - host tests for iotdata_node_state.h over the linux datastore.
//
// The failure modes are all invisible in a log and all show up days later on somebody's roof: a
// struct that grew between firmware versions and is read back through its old layout; a module
// added or removed taking its neighbours' state with it; a store that accepts every write and
// loses the lot on reboot. Each one is a test below.
//
// What is NOT here is anything about sequence numbers going backwards. Ordering the advance
// against the persist is the APPLICATION's problem -- the sensor flushes before the frame goes
// out, the gateway writes behind a tick -- and this module only promises that what was stored
// comes back.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_config.h"
#include "iotdata_variant.h"
#include "iotdata.h"
#include "device/d_module_datastore_linux.h"
#include "iotdata_node_state.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define TAG_A 0x41414141UL
#define TAG_B 0x42424242UL
#define TAG_C 0x43434343UL

typedef struct {
    uint32_t cycles;
    uint16_t battery_mv;
} block_a_t;
typedef struct {
    uint8_t flags;
} block_b_t;

static char g_dir[128];

static void store_reset(datastore_t *const ds) {
    CHECK(datastore_open(ds, g_dir), "store opens");
}

// ------------------------------------------------------------------------------------------------------------------------

static void test_first_boot_defaults(void) {
    printf("\nfirst boot: every block defaulted, nothing restored\n");
    datastore_t ds;
    store_reset(&ds);
    (void)datastore_erase(&ds, "state");

    static const block_a_t A_DEFAULTS = { .cycles = 7u, .battery_mv = 4200u };
    block_a_t a;
    block_b_t b;
    iotdata_node_state_t s;
    iotdata_node_state_init(&s, &ds, "state");
    CHECK(iotdata_node_state_insert(&s, TAG_A, 1u, &a, sizeof(a), &A_DEFAULTS), "insert a");
    CHECK(iotdata_node_state_insert(&s, TAG_B, 1u, &b, sizeof(b), NULL), "insert b");
    CHECK(!iotdata_node_state_load(&s), "nothing to restore");
    CHECK(a.cycles == 7u && a.battery_mv == 4200u, "a took its defaults");
    CHECK(b.flags == 0u, "b zero-filled with no defaults given");
    CHECK(!iotdata_node_state_insert(&s, TAG_C, 1u, &b, sizeof(b), NULL), "insert after load is refused");
    datastore_close(&ds);
}

static void test_round_trip(void) {
    printf("\nsaved and restored, by tag\n");
    datastore_t ds;
    store_reset(&ds);
    (void)datastore_erase(&ds, "state");

    block_a_t a = { 0 };
    block_b_t b = { 0 };
    iotdata_node_state_t s;
    iotdata_node_state_init(&s, &ds, "state");
    (void)iotdata_node_state_insert(&s, TAG_A, 1u, &a, sizeof(a), NULL);
    (void)iotdata_node_state_insert(&s, TAG_B, 1u, &b, sizeof(b), NULL);
    (void)iotdata_node_state_load(&s);
    a.cycles = 1234u;
    a.battery_mv = 3900u;
    b.flags = 0xA5u;
    CHECK(iotdata_node_state_flush(&s), "flushed");

    /* a restart: same registrations, different memory */
    block_a_t a2 = { 0 };
    block_b_t b2 = { 0 };
    iotdata_node_state_t s2;
    iotdata_node_state_init(&s2, &ds, "state");
    (void)iotdata_node_state_insert(&s2, TAG_A, 1u, &a2, sizeof(a2), NULL);
    (void)iotdata_node_state_insert(&s2, TAG_B, 1u, &b2, sizeof(b2), NULL);
    CHECK(iotdata_node_state_load(&s2), "restored something");
    CHECK(a2.cycles == 1234u && a2.battery_mv == 3900u, "a came back");
    CHECK(b2.flags == 0xA5u, "b came back");
    datastore_close(&ds);
}

static void test_registration_changes(void) {
    printf("\na firmware change: blocks added, removed and resized\n");
    datastore_t ds;
    store_reset(&ds);
    (void)datastore_erase(&ds, "state");

    block_a_t a;
    block_b_t b;
    iotdata_node_state_t s;
    iotdata_node_state_init(&s, &ds, "state");
    (void)iotdata_node_state_insert(&s, TAG_A, 1u, &a, sizeof(a), NULL);
    (void)iotdata_node_state_insert(&s, TAG_B, 1u, &b, sizeof(b), NULL);
    (void)iotdata_node_state_load(&s); /* defaults everything: values have to be set AFTER it */
    a.cycles = 99u;
    a.battery_mv = 3300u;
    b.flags = 0x11u;
    CHECK(iotdata_node_state_flush(&s), "flushed both");

    /* the new firmware: A unchanged, B GREW, C is new and was never persisted */
    typedef struct {
        uint8_t flags;
        uint32_t added_field;
    } block_b_grown_t;
    block_a_t a2 = { 0 };
    block_b_grown_t b2 = { .flags = 0xEE, .added_field = 0xDEADBEEF };
    uint8_t c2 = 0x77u;
    iotdata_node_state_t s2;
    iotdata_node_state_init(&s2, &ds, "state");
    (void)iotdata_node_state_insert(&s2, TAG_A, 1u, &a2, sizeof(a2), NULL);
    (void)iotdata_node_state_insert(&s2, TAG_B, 1u, &b2, sizeof(b2), NULL);
    (void)iotdata_node_state_insert(&s2, TAG_C, 1u, &c2, sizeof(c2), NULL);
    (void)iotdata_node_state_load(&s2);

    CHECK(a2.cycles == 99u && a2.battery_mv == 3300u, "the UNCHANGED block still came back");
    CHECK(b2.flags == 0u && b2.added_field == 0u, "the RESIZED block defaulted, not reinterpreted");
    CHECK(c2 == 0u, "the NEW block defaulted");
    CHECK(s2.stat_restored == 1u && s2.stat_defaulted == 2u, "one restored, two defaulted");

    /* and back again: a firmware that DROPS B must not disturb A */
    block_a_t a3 = { 0 };
    iotdata_node_state_t s3;
    iotdata_node_state_init(&s3, &ds, "state");
    (void)iotdata_node_state_insert(&s3, TAG_A, 1u, &a3, sizeof(a3), NULL);
    (void)iotdata_node_state_load(&s3);
    CHECK(a3.cycles == 99u, "an unknown tag in the image is skipped, not fatal");
    datastore_close(&ds);
}

/* THE CASE THE RESIZE TEST DOES NOT COVER, and the one a unit change needs: same tag, same size,
   different VERSION. A field that changed from milliseconds to seconds does not move a byte, so
   size tells the loader nothing and only the version stands between a node and a value a thousand
   times too large. */
static void test_version_change(void) {
    printf("\na field that changed MEANING without changing size: the version is the only guard\n");
    datastore_t ds;
    store_reset(&ds);
    (void)datastore_erase(&ds, "state");

    block_a_t a;
    iotdata_node_state_t s;
    iotdata_node_state_init(&s, &ds, "state");
    (void)iotdata_node_state_insert(&s, TAG_A, 1u, &a, sizeof(a), NULL);
    (void)iotdata_node_state_load(&s);
    a.cycles = 5000u; /* stand in for the millisecond window that used to live here */
    a.battery_mv = 4100u;
    CHECK(iotdata_node_state_flush(&s), "written by the old firmware");

    /* the same block, same width, read by a build that bumped its version */
    static const block_a_t A_DEFAULTS = { .cycles = 5u, .battery_mv = 0u };
    block_a_t a2 = { 0 };
    iotdata_node_state_t s2;
    iotdata_node_state_init(&s2, &ds, "state");
    (void)iotdata_node_state_insert(&s2, TAG_A, 2u, &a2, sizeof(a2), &A_DEFAULTS);
    CHECK(!iotdata_node_state_load(&s2), "nothing restored: the stored version is not this one");
    CHECK(a2.cycles == 5u, "the block DEFAULTED rather than being reinterpreted");
    CHECK(s2.stat_defaulted == 1u && s2.stat_restored == 0u, "and said so in the counters");

    /* and the version that WROTE it still reads it, so the bump is a one-way step and not a wipe */
    block_a_t a3 = { 0 };
    iotdata_node_state_t s3;
    iotdata_node_state_init(&s3, &ds, "state");
    (void)iotdata_node_state_insert(&s3, TAG_A, 1u, &a3, sizeof(a3), NULL);
    CHECK(iotdata_node_state_load(&s3), "the original version still matches");
    CHECK(a3.cycles == 5000u, "and gets its bytes back unchanged");
    datastore_close(&ds);
}

static void test_tick_gating(void) {
    printf("\ntick: write-behind is gated, write-through is not\n");
    datastore_t ds;
    store_reset(&ds);
    (void)datastore_erase(&ds, "state");

    block_a_t a = { 0 };
    iotdata_node_state_t s;
    iotdata_node_state_init(&s, &ds, "state");
    (void)iotdata_node_state_insert(&s, TAG_A, 1u, &a, sizeof(a), NULL);
    (void)iotdata_node_state_load(&s);

    CHECK(!iotdata_node_state_tick(&s, 1000u), "nothing dirty, nothing written");
    iotdata_node_state_touch(&s);
    CHECK(iotdata_node_state_tick(&s, 1000u), "first dirty tick writes");
    iotdata_node_state_touch(&s);
    CHECK(!iotdata_node_state_tick(&s, 1000u + IOTDATA_NODE_STATE_SAVE_MS - 1u), "too soon: held back");
    CHECK(iotdata_node_state_tick(&s, 1000u + IOTDATA_NODE_STATE_SAVE_MS), "interval elapsed: written");
    CHECK(!s.dirty, "clean after a write");
    datastore_close(&ds);
}

static void test_persistence_detection(void) {
    printf("\npersistence: a store that cannot survive a reboot says so\n");

    /* The negative case is the one that matters -- it is the silent data loss this guards. */
    datastore_t shm;
    if (datastore_open(&shm, "/dev/shm/iotdata-state-test")) {
        CHECK(!datastore_persistent(&shm), "tmpfs is detected and reported as NOT persistent");
        (void)datastore_erase(&shm, "state");
        datastore_close(&shm);
        (void)rmdir("/dev/shm/iotdata-state-test");
        CHECK(!datastore_persistent(&shm), "a closed store is never persistent");
    } else
        printf("  (no /dev/shm here, tmpfs check skipped)\n");

    /* The positive case needs a path on a real filesystem, and the build directory is the one we
       know is on disk -- /tmp is tmpfs on a great many hosts, this one included. */
    datastore_t disk;
    if (datastore_open(&disk, "build-test/persist-probe")) {
        CHECK(datastore_persistent(&disk), "a disk-backed filesystem reports persistent");
        datastore_close(&disk);
        (void)rmdir("build-test/persist-probe");
    }
}

// ------------------------------------------------------------------------------------------------------------------------

int main(void) {
    printf("iotdata_node_state: blocks by tag, and sequence numbers that never go backwards\n");
    snprintf(g_dir, sizeof(g_dir), "/tmp/iotdata-state-test-%d", (int)getpid());

    test_first_boot_defaults();
    test_round_trip();
    test_registration_changes();
    test_version_change();
    test_tick_gating();
    test_persistence_detection();

    datastore_t ds;
    if (datastore_open(&ds, g_dir)) {
        (void)datastore_erase(&ds, "state");
        datastore_close(&ds);
    }
    (void)rmdir(g_dir);

    printf("\n%s\n", fails == 0 ? "all ok" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
