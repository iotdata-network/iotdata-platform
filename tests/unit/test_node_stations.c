//
// test_node_stations.c - the table of stations a node has heard.
//
// A skeleton over the parts that would quietly break: the LRU that decides who gets forgotten when
// the table is full, and the RSSI three-state, which exists because a station heard with no reading
// must not report as the strongest signal in a dataset about link strength.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "device/d_platform_linux.h" /* iotdata_node_utils.h asks it for the board identity */
#include "device/d_format.h"
#include "iotdata_node_utils.h"
#include "iotdata_node_stations.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static void test_empty(void) {
    printf("\nan empty table knows nothing and says so\n");
    stations_t t;
    stations_init(&t);
    CHECK(stations_count(&t) == 0, "count is zero");
    CHECK(stations_locate(&t, 0x123) == -1, "locate finds nothing");
}

static void test_seen_and_update(void) {
    printf("\nfirst sighting is new, later ones are not\n");
    stations_t t;
    stations_init(&t);
    CHECK(stations_seen(&t, 0x123, 7, -90, 1000) == true, "first time is new");
    CHECK(stations_seen(&t, 0x123, 7, -80, 2000) == false, "second time is not");
    CHECK(stations_count(&t) == 1, "and did not add a second row");

    const int i = stations_locate(&t, 0x123);
    CHECK(i == 0, "locate finds it");
    CHECK(t.s[i].rx_count == 2, "rx_count counts every sighting, not every station");
    CHECK(t.s[i].rssi_dbm == -80, "rssi is the LATEST, not the first");
    CHECK(t.s[i].last_ms == 2000, "and so is last_ms");

    printf("\na different station is a different row\n");
    CHECK(stations_seen(&t, 0x456, 7, -70, 3000) == true, "new station");
    CHECK(stations_count(&t) == 2, "two rows");
    CHECK(stations_locate(&t, 0x456) == 1, "in arrival order");
}

static void test_kind(void) {
    printf("\nkind can be learned later, and UNKNOWN never unlearns it\n");
    stations_t t;
    stations_init(&t);
    (void)stations_seen(&t, 0x123, 7, -90, 1000);
    CHECK(t.s[0].kind == STATION_KIND_UNKNOWN, "a new station starts unknown");
    stations_note_kind(&t, 0x123, STATION_KIND_RELAY);
    CHECK(t.s[0].kind == STATION_KIND_RELAY, "and can be told");
    /* a later frame that cannot tell must not erase what an earlier one established */
    stations_note_kind(&t, 0x123, STATION_KIND_UNKNOWN);
    CHECK(t.s[0].kind == STATION_KIND_RELAY, "UNKNOWN does not overwrite a known kind");
    stations_note_kind(&t, 0x999, STATION_KIND_GATEWAY); /* absent: must not crash or invent a row */
    CHECK(stations_count(&t) == 1, "noting an absent station adds nothing");
}

static void test_full_table_evicts_oldest(void) {
    printf("\nwhen full, the station heard longest ago is the one forgotten\n");
    stations_t t;
    stations_init(&t);
    /* fill it, then refresh everything EXCEPT 0x101, making that one the stalest */
    for (int i = 0; i < STATIONS_MAX; i++)
        (void)stations_seen(&t, (uint16_t)(0x100 + i), 7, -90, (uint32_t)(1000 + i * 10));
    CHECK(stations_count(&t) == STATIONS_MAX, "full");
    for (int i = 0; i < STATIONS_MAX; i++)
        if (i != 1)
            (void)stations_seen(&t, (uint16_t)(0x100 + i), 7, -90, 9000);

    CHECK(stations_seen(&t, 0x900, 7, -60, 9100) == true, "a new station past capacity is still new");
    CHECK(stations_count(&t) == STATIONS_MAX, "and the table does not grow");
    CHECK(stations_locate(&t, 0x101) == -1, "the stalest station was evicted");
    CHECK(stations_locate(&t, 0x900) >= 0, "the newcomer is in");
    CHECK(stations_locate(&t, 0x100) >= 0, "a refreshed station survived");

    /* an evicted slot must be reset, not inherit the evicted station's history */
    const int at = stations_locate(&t, 0x900);
    CHECK(at >= 0 && t.s[at].rx_count == 1, "the reused row starts its count again");
    CHECK(at >= 0 && t.s[at].kind == STATION_KIND_UNKNOWN, "and does not inherit a kind");
}

static void test_clear(void) {
    printf("\nclear forgets everyone\n");
    stations_t t;
    stations_init(&t);
    (void)stations_seen(&t, 0x123, 7, -90, 1000);
    stations_clear(&t);
    CHECK(stations_count(&t) == 0, "count is zero");
    CHECK(stations_locate(&t, 0x123) == -1, "and the station is gone");
    CHECK(stations_seen(&t, 0x123, 7, -90, 2000) == true, "so it reads as new again");
}

static void test_rssi_three_states(void) {
    printf("\nrssi is three states, because absence is not zero\n");
    char buf[STATIONS_RSSI_STR_MAX];
    CHECK(strcmp(stations_rssi_str(-91, buf, sizeof(buf)), "-91dBm") == 0, "a reading prints as dBm");
    CHECK(strcmp(stations_rssi_str(0, buf, sizeof(buf)), "sat") == 0, "zero is saturated, not silent");
    CHECK(strcmp(stations_rssi_str(1, buf, sizeof(buf)), "n/a") == 0, "positive means no reading at all");
    /* the whole point: none of the three can be mistaken for a strong signal */
    CHECK(strstr(stations_rssi_str(0, buf, sizeof(buf)), "0dBm") == NULL, "saturated never prints as 0dBm");
}

int main(void) {
    printf("test_node_stations -- capacity %d\n", STATIONS_MAX);
    test_empty();
    test_seen_and_update();
    test_kind();
    test_full_table_evicts_oldest();
    test_clear();
    test_rssi_three_states();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
