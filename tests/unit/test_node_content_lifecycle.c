//
// test_node_content_lifecycle.c - a manifest's whole life, across four nodes, over a fake air.
//
// WHAT THIS IS AND WHY IT IS NOT MORE UNIT TESTS. test_node_content_manifest.c drives one table
// directly and pins the rules. This one builds REAL FRAMES -- iotdata_encode_begin, a CONTENT TLV,
// iotdata_encode_end -- puts them on a bus with a reachability matrix and per-node station filters,
// and decodes them at the far end exactly as an application's receive branch will. So the receive
// branch below is not a stand-in for the one the apps will get: it IS that code, written once here
// where it can be driven, and the apps will call the same two functions in the same order.
//
// The topology is the bench: a gateway, a relay, and two sensors.
//
//     gateway(001) <-> relay(68E) <-> sensor1(57D)
//                                 <-> sensor2(153)
//
// and `hears[][]` is what each can hear, so "the gateway cannot reach that sensor directly" is a
// line of code rather than a box moved across a room.
//
// WHAT IS NOT HERE, DELIBERATELY: the down path's flood, hold and dedup. A frame on this bus
// arrives once, at everyone who can hear it. That is iotdata_node_down.h's job, it has its own
// suite in the relay, and modelling it twice would mean two models to keep honest. What this bus
// is for is who ACCEPTS a frame and what they do with it.
//
// THE TRANSFER IS SIMULATED, AND THAT IS THE POINT OF RUNNING THIS NOW. RANGES and CHUNKS do not
// exist yet, so a pull here writes the bytes through the insert the console already uses -- with
// the size and the CRC taken from the MANIFEST, which is exactly what the real requester will do.
// Everything either side of the bytes moving is therefore under test before a single chunk frame
// has been encoded: deciding to pull, the insert declaring what the manifest said, the checksum,
// the announce, and the verify afterwards.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

#include "d_platform_linux.h"
#include "d_common.h"
#include "d_format.h"
#include "d_module_filestore.h"
#include "iotdata_variant.h"
#include "iotdata_node.h"
#include "iotdata_node_stations.h"
#include "iotdata_node_content.h"
#include "d_module_filestore_ram.h"
#include "iotdata_node_content_manifest.h"
#include "iotdata_node_status.h"  /* control.h resolves status scopes, so it comes first */
#include "iotdata_node_control.h" /* the resolver: what a control result IS */
#include "d_module_otaslot.h"     /* content_control.h speaks OTA too, so its types have to be here */
#include "iotdata_node_content_ota.h"
#include "iotdata_node_content_control.h" /* a MANIFEST_SET that arrived over the air lands here */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

#define GW       0x001u /* the gateway's configured station, as the box on the bench has it */
#define RLY      0x68Eu
#define SEN1     0x57Du
#define SEN2     0x153u

/* §4's recommended allocation, (station << 12) | counter, so every node mints its own ids. */
#define GW_IMAGE 0x001003u /* the gateway's offer: a sensor image */
#define GW_DELTA 0x001004u /* and a patch flavour of the same thing */
#define GW_SIG   0x001005u
#define RLY_BLOB 0x68E001u /* the relay's own: a calibration blob loaded by cable */

#define NODES    4

typedef struct {
    const char *name;
    uint16_t station;
    filestore_ram_t fs;
    const d_filestore_t *be;
    iotdata_node_content_t content;
    iotdata_node_content_manifests_t offers;
    filter_t filter;
    /* what the bus did to it, which is the half a unit test cannot see */
    uint32_t heard;      /* frames that reached this node at all */
    uint32_t dropped;    /* ... and were refused before the module saw them */
    uint32_t taken;      /* MANIFEST TLVs applied */
    uint32_t refused;    /* ... and applied unsuccessfully */
    uint32_t commanded;  /* CONTROL TLVs carried out */
    uint32_t declined;   /* ... and refused */
    const char *refusal; /* the last reason given, which is what an operator is shown */
} node_t;

static node_t node[NODES];
static bool hears[NODES][NODES];
static uint32_t now_ms = 1000u;

/* ------------------------------------------------------------------------------------------- */

static uint8_t fixture[2048];

/* Plant a file as a cable or a factory image would, and hand back what it sums to -- which is what
   an operator reads off the build host and what a manifest will have to agree with. */
static uint64_t plant(node_t *const n, const uint32_t id, const uint32_t size, const uint8_t seed) {
    if (size > sizeof(fixture))
        return 0u;
    for (uint32_t i = 0; i < size; i++)
        fixture[i] = (uint8_t)(seed + i * 7u);
    if (!filestore_ram_plant(&n->fs, id, fixture, size))
        return 0u;
    return _iotdata_crc64(fixture, size, _IOTDATA_CRC64_INIT);
}

static bool holds(node_t *const n, const uint32_t id, uint32_t *const held) {
    uint32_t total = 0u, h = 0u;
    const bool ok = iotdata_node_content_stat(&n->content, id, &total, &h);
    if (held != NULL)
        *held = ok ? h : 0u;
    return ok;
}

/* THE CONTENT MODULE IS A SINGLETON, because a node is one node: content_control.h holds its store
   and its offers table in file statics, set once at boot. Four nodes in one process therefore have
   to say which of them is being spoken to before anything is dispatched, which is not a workaround
   -- it is the same call each app makes in its own startup, made here per frame instead. */
static void control_at(node_t *const n) {
    iotdata_node_content_control_init(&n->content, NULL);
    iotdata_node_content_control_offers_init(&n->offers, NULL, NULL); /* no originate: nothing here asks it to transmit */
}

/* ------------------------------------------------------------------------------------------- */
// THE RECEIVE BRANCH -- the code an application will call, written once, here
/* ------------------------------------------------------------------------------------------- */

/*
 * A down frame arrived. The three gates in front of the module are the ones the apps already apply
 * to every other TLV, in the order they already apply them:
 *
 *   is it a DOWN frame at all           an upstream frame is somebody's reading, not an offer
 *   is it addressed to us               our station, or broadcast
 *   does our filter allow that target   policy, and for a down frame the target IS the station field
 *
 * Only then does the CONTENT TLV reach the offers table. Several MANIFEST TLVs in one frame are
 * taken one at a time, because §3 puts one manifest in each and a frame offering several flavours
 * carries several.
 */
static int node_on_down(node_t *const n, const uint8_t *const buf, const size_t len) {
    uint8_t variant = 0;
    uint16_t station = 0, sequence = 0;
    if (iotdata_peek(buf, len, &variant, &station, &sequence) != IOTDATA_OK)
        return -1;
    n->heard++;
    if (!iotdata_node_is_down(sequence) || !iotdata_node_addressed_to(station, n->station)) {
        n->dropped++;
        return 0; /* not ours: on real hardware it would be flooded on instead */
    }
    if (!filter_allows(&n->filter, station)) {
        n->filter.stat_blocked++;
        n->dropped++;
        return 0;
    }
    static iotdata_decoder_t dec;
    if (iotdata_decode(buf, len, &dec) != IOTDATA_OK)
        return -1;
    int applied = 0;
    for (uint8_t i = 0; i < dec.tlv_count; i++) {
        const iotdata_decoder_tlv_t *const t = &dec.tlv[i];
        /* A COMMAND, which is a different thing arriving by the same door. A CONTENT TLV is a
           STATEMENT -- somebody telling us what they have -- and is applied to the offers table.
           A CONTROL TLV is an INSTRUCTION to this node about its own content, and the node carries
           it out on itself: §3's "set a manifest" is how a node with local files is told to start
           offering one of them, over the air, having never been near a console. The two are kept
           apart here exactly as the apps keep them apart, in one loop over one frame's TLVs. */
        if (t->format == IOTDATA_TLV_FMT_RAW && t->type == IOTDATA_NODE_TLV_CONTROL) {
            size_t cur = 0;
            uint8_t key, vlen;
            const uint8_t *val;
            while (iotdata_kvr_next(t->raw, t->length, &cur, &key, &val, &vlen)) {
                if (key != IOTDATA_NODE_CONTROL_CONTROL || vlen < 2u)
                    continue;
                const char *reason = NULL;
                control_at(n);
                const iotdata_node_control_result_t r = iotdata_node_content_control(val[0], val[1], (vlen > 2u) ? val + 2 : NULL, (uint8_t)(vlen - 2u), now_ms, &reason);
                if (r == IOTDATA_NODE_CONTROL_RESULT_DONE)
                    n->commanded++;
                else {
                    n->declined++;
                    n->refusal = reason;
                }
            }
            continue;
        }
        if (t->format != IOTDATA_TLV_FMT_RAW || t->type != IOTDATA_NODE_TLV_CONTENT)
            continue;
        uint8_t kind = 0;
        if (!iotdata_content_header_parse(t->raw, t->length, &kind, NULL, NULL))
            continue;
        if (kind != IOTDATA_CONTENT_MANIFEST)
            continue; /* RANGES and CHUNKS belong to the transfer, which is not built yet */
        uint32_t id = 0u;
        const iotdata_node_content_status_t rc = iotdata_node_content_manifest_on_wire(&n->offers, &n->content, t->raw, t->length, now_ms, &id);
        if (rc == IOTDATA_NODE_CONTENT_OK) {
            n->taken++;
            applied++;
        } else
            n->refused++;
    }
    return applied;
}

/* ------------------------------------------------------------------------------------------- */
// THE AIR
/* ------------------------------------------------------------------------------------------- */

/* One frame, built as a gateway builds one and put in front of everybody who can hear it. `tlvs`
   is an array of TLV values so a frame offering several flavours is one transmission, which is
   what §3 says it is. Returns how many nodes applied something. */
static int air_send(node_t *const from, const uint16_t target, const uint8_t *const *const tlvs, const size_t *const tlvlens, const size_t count) {
    static iotdata_encoder_t enc;
    static uint8_t frame[IOTDATA_MAX_PACKET_SIZE];
    size_t len = 0u;
    if (iotdata_encode_begin(&enc, frame, sizeof(frame), 0, target, IOTDATA_SEQUENCE_DOWN) != IOTDATA_OK)
        return -1;
    for (size_t k = 0; k < count; k++)
        if (iotdata_encode_tlv(&enc, IOTDATA_NODE_TLV_CONTENT, tlvs[k], (uint8_t)tlvlens[k]) != IOTDATA_OK)
            return -1;
    if (iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return -1;

    const int src = (int)(from - &node[0]);
    int applied = 0;
    for (int d = 0; d < NODES; d++)
        if (d != src && hears[src][d]) {
            const int n = node_on_down(&node[d], frame, len);
            if (n > 0)
                applied += n;
        }
    return applied;
}

/* A COMMAND PUT ON THE AIR, which is the gateway's job and the only way to reach a node that has
   no console: a down frame carrying a CONTROL TLV whose record is [subject][action][args...]. */
static int air_command(node_t *const from, const uint16_t target, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    static iotdata_encoder_t enc;
    static uint8_t frame[IOTDATA_MAX_PACKET_SIZE];
    uint8_t kvbuf[IOTDATA_MAX_PACKET_SIZE / 2], cmd[2 + 255];
    iotdata_kvr_t kv;
    size_t len = 0u;
    if ((size_t)arglen + 2u > sizeof(cmd))
        return -1;
    cmd[0] = subject;
    cmd[1] = action;
    if (args != NULL && arglen > 0u)
        memcpy(cmd + 2, args, arglen);
    iotdata_kvr_init(&kv, kvbuf, sizeof(kvbuf));
    if (!iotdata_kvr_add(&kv, IOTDATA_NODE_CONTROL_CONTROL, cmd, (uint8_t)(2u + arglen)))
        return -1;
    if (iotdata_encode_begin(&enc, frame, sizeof(frame), 0, target, IOTDATA_SEQUENCE_DOWN) != IOTDATA_OK || iotdata_encode_tlv(&enc, IOTDATA_NODE_TLV_CONTROL, kvbuf, (uint8_t)kv.len) != IOTDATA_OK ||
        iotdata_encode_end(&enc, &len) != IOTDATA_OK)
        return -1;
    const int src = (int)(from - &node[0]);
    int reached = 0;
    for (int d = 0; d < NODES; d++)
        if (d != src && hears[src][d])
            if (node_on_down(&node[d], frame, len) >= 0)
                reached++;
    return reached;
}

/* The arguments a MANIFEST_SET carries: an id, an encoding, an expiry, a signature id, and a name
   whose terminator has to fall inside the argument -- §3's rule, because there is no length to
   consult for a second opinion. */
static uint8_t set_args[3 + 1 + 2 + 3 + IOTDATA_NODE_CONTENT_NAME_MAX + 1];
static uint8_t build_set(const uint32_t id, const uint16_t expiry_m, const char *const name) {
    set_args[0] = (uint8_t)(id >> 16);
    set_args[1] = (uint8_t)(id >> 8);
    set_args[2] = (uint8_t)id;
    set_args[3] = 0u; /* encoding: the bytes as they are */
    set_args[4] = (uint8_t)(expiry_m >> 8);
    set_args[5] = (uint8_t)expiry_m;
    set_args[6] = set_args[7] = set_args[8] = 0u; /* no signature */
    const size_t n = strlen(name);
    memcpy(set_args + 9, name, n);
    set_args[9 + n] = '\0';
    return (uint8_t)(10u + n);
}

/* One of our own offers, re-originated: the generation advances and the frame differs. */
static int advertise(node_t *const from, const uint32_t id, const uint16_t target) {
    uint8_t val[128];
    const size_t n = iotdata_node_content_manifest_advertise(&from->offers, &from->content, id, val, sizeof(val), now_ms);
    if (n == 0u)
        return -1;
    const uint8_t *const tlvs[1] = { val };
    const size_t lens[1] = { n };
    return air_send(from, target, tlvs, lens, 1u);
}

/* ------------------------------------------------------------------------------------------- */

static void setup(void) {
    static const char *const names[NODES] = { "gateway", "relay", "sensor1", "sensor2" };
    static const uint16_t stations[NODES] = { GW, RLY, SEN1, SEN2 };
    memset(node, 0, sizeof(node));
    for (int i = 0; i < NODES; i++) {
        node[i].name = names[i];
        node[i].station = stations[i];
        node[i].be = filestore_ram_init(&node[i].fs);
        iotdata_node_content_init(&node[i].content, node[i].be);
        (void)iotdata_node_content_begin(&node[i].content, now_ms);
        /* The gateway's store is a directory and the two ESP32s' are SPIFFS, so all three are
           durable; a node staging into RAM would say false and its rows would say so. */
        iotdata_node_content_manifests_init(&node[i].offers, true, node[i].station);
        filter_init(&node[i].filter);
    }
    /* EVERYTHING HEARS EVERYTHING, which is the bench as it actually is -- the four boards are on
       one desk. Isolating a path is then a filter or a line of this matrix, and the tests below do
       both, because on the bench only one of the two is available. */
    for (int a = 0; a < NODES; a++)
        for (int b = 0; b < NODES; b++)
            hears[a][b] = (a != b);
}

/* ------------------------------------------------------------------------------------------- */

static void test_an_offer_reaches_everyone_who_can_hear_it(void) {
    printf("\nan advertisement is a broadcast down frame, and everybody who hears it keeps the offer\n");
    setup();
    node_t *const gw = &node[0];
    const uint64_t crc = plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(crc != 0u, "the gateway was handed an image");
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "sensor-esp32c3-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BINARY, IOTDATA_CONTENT_COMPRESS_NONE), 1440u, GW_SIG, now_ms) ==
              IOTDATA_NODE_CONTENT_OK,
          "and authored an offer over it");

    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "all three other nodes took it");
    for (int i = 1; i < NODES; i++) {
        const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&node[i].offers, GW_IMAGE);
        CHECK(m != NULL, "the offer is in the table");
        if (m != NULL) {
            CHECK(m->size == 448u && m->hash == crc, "with the size and the hash the gateway stated");
            CHECK(!m->local, "and it is heard, not theirs to re-originate");
            CHECK(strcmp(m->name, "sensor-esp32c3-0.9.11") == 0, "name and all");
        }
        /* NOTHING IS HELD, and that is the state the whole bring-up turns on: a node that knows
           about an object and has none of it is exactly what an arriving advertisement looks like. */
        CHECK(!holds(&node[i], GW_IMAGE, NULL), "and nothing of it is held yet");
    }

    /* THE ADVERTISER DOES NOT TAKE ITS OWN OFFER BACK. On real hardware the flood returns it off a
       relay; here it never left, but the rule is the same one. */
    const iotdata_node_content_manifest_t *const mine = iotdata_node_content_manifest_find(&gw->offers, GW_IMAGE);
    CHECK(mine != NULL && mine->local, "the gateway's row is still its own");
    CHECK(mine != NULL && mine->generation == 1u, "and one origination has been spent");
}

static void test_several_flavours_in_one_frame(void) {
    printf("\na frame offering several flavours carries several MANIFEST TLVs, and all of them land\n");
    setup();
    node_t *const gw = &node[0];
    const uint64_t cfull = plant(gw, GW_IMAGE, 448u, 0x11u);
    const uint64_t cdelta = plant(gw, GW_DELTA, 96u, 0x22u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "sensor-esp32c3-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BINARY, IOTDATA_CONTENT_COMPRESS_NONE), 1440u, GW_SIG, now_ms) ==
              IOTDATA_NODE_CONTENT_OK,
          "the whole image");
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_DELTA, "sensor-esp32c3-0.9.10-to-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BSDIFF, IOTDATA_CONTENT_COMPRESS_HEATSHRINK), 1440u, GW_SIG, now_ms) ==
              IOTDATA_NODE_CONTENT_OK,
          "and a patch against it");

    uint8_t a[128], b[128];
    const size_t na = iotdata_node_content_manifest_advertise(&gw->offers, &gw->content, GW_IMAGE, a, sizeof(a), now_ms);
    const size_t nb = iotdata_node_content_manifest_advertise(&gw->offers, &gw->content, GW_DELTA, b, sizeof(b), now_ms);
    CHECK(na > 0u && nb > 0u, "both packed");
    const uint8_t *const tlvs[2] = { a, b };
    const size_t lens[2] = { na, nb };
    CHECK(air_send(gw, IOTDATA_STATION_BROADCAST, tlvs, lens, 2u) == 6, "two offers times three listeners");

    for (int i = 1; i < NODES; i++) {
        const iotdata_node_content_manifest_t *const f = iotdata_node_content_manifest_find(&node[i].offers, GW_IMAGE);
        const iotdata_node_content_manifest_t *const d = iotdata_node_content_manifest_find(&node[i].offers, GW_DELTA);
        CHECK(f != NULL && d != NULL, "both flavours are known");
        if (f != NULL && d != NULL) {
            CHECK(f->hash == cfull && d->hash == cdelta, "each with its own hash");
            /* THE ENDPOINT PICKS, and these two nibbles are what it picks on -- a node that cannot
               inflate heatshrink can see so without parsing a single string. */
            CHECK(iotdata_content_encoding_compression(d->encoding) == IOTDATA_CONTENT_COMPRESS_HEATSHRINK, "the patch says how it is wrapped");
            CHECK(iotdata_content_encoding_compression(f->encoding) == IOTDATA_CONTENT_COMPRESS_NONE, "and the whole image says it is not");
            CHECK(d->size < f->size, "and the patch is the smaller airtime, which is the whole reason for offering both");
        }
    }
}

static void test_offers_from_several_places_coexist(void) {
    printf("\noffers come from wherever content does: a gateway's image and a relay's blob, side by side\n");
    setup();
    node_t *const gw = &node[0];
    node_t *const rly = &node[1];

    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "the gateway offers an image");

    /* THE RELAY MINTS ITS OWN IDS, out of its own station's 4096, which is what lets the upward
       and sideways cases work with no second mechanism. The blob got there by cable. */
    const uint64_t blob = plant(rly, RLY_BLOB, 64u, 0x33u);
    CHECK(iotdata_node_content_manifest_author(&rly->offers, &rly->content, RLY_BLOB, "calibration", 0u, IOTDATA_CONTENT_EXPIRY_INDEFINITE, 0u, now_ms) == IOTDATA_NODE_CONTENT_OK, "and the relay offers a blob of its own");
    /* ITS SIG-ID IS 0, which is the overloaded value: §3 spends it on "this IS a signature stream",
       and until signature streams exist it is also what a manifest with no stream behind it
       carries. See the note by IOTDATA_CONTENT_SIG_NONE -- the integrity of a 64-byte blob is its
       manifest's gross hash either way, so nothing here turns on it yet. */
    const iotdata_node_content_manifest_t *const own = iotdata_node_content_manifest_find(&rly->offers, RLY_BLOB);
    CHECK(own != NULL && own->hash == blob && own->sig_id == IOTDATA_CONTENT_SIG_NONE, "summed off its own store, and naming no signature stream");

    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "the image reaches everyone");
    CHECK(advertise(rly, RLY_BLOB, IOTDATA_STATION_BROADCAST) == 3, "and so does the blob");

    for (int i = 2; i < NODES; i++) {
        CHECK(iotdata_node_content_manifests_count(&node[i].offers) == 2u, "a sensor knows about both");
        const iotdata_node_content_manifest_t *const g = iotdata_node_content_manifest_find(&node[i].offers, GW_IMAGE);
        const iotdata_node_content_manifest_t *const r = iotdata_node_content_manifest_find(&node[i].offers, RLY_BLOB);
        CHECK(g != NULL && r != NULL, "under the two ids their originators minted");
        CHECK(g != NULL && (g->id >> 12) == GW, "and the top twelve bits say who minted each");
        CHECK(r != NULL && (r->id >> 12) == RLY, "which is the free validity check the allocation buys");
    }
    /* The gateway heard the relay's offer too: a gateway is a node like any other and may well
       want the blob. */
    CHECK(iotdata_node_content_manifest_find(&gw->offers, RLY_BLOB) != NULL, "the gateway heard the relay's offer");
    /* NEITHER MAY RE-ORIGINATE THE OTHER'S. That is what stops an offer's life being extended by
       whoever happens to be holding it. */
    uint8_t val[128];
    CHECK(iotdata_node_content_manifest_advertise(&gw->offers, &gw->content, RLY_BLOB, val, sizeof(val), now_ms) == 0u, "and must not re-advertise it");
    CHECK(iotdata_node_content_manifest_advertise(&rly->offers, &rly->content, GW_IMAGE, val, sizeof(val), now_ms) == 0u, "in either direction");
}

static void test_who_ignores_an_offer_and_why(void) {
    printf("\nthe four ways an offer is ignored, each for its own reason\n");
    setup();
    node_t *const gw = &node[0];
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an offer exists");

    /* 1. ADDRESSED TO SOMEBODY ELSE. A unicast advertisement is how a path is isolated on a bench:
          broadcast cannot be, because a relay's repeat of a broadcast is indistinguishable from the
          original. */
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 1, "a unicast offer reaches the one it names");
    CHECK(iotdata_node_content_manifest_find(&node[2].offers, GW_IMAGE) != NULL, "sensor1 has it");
    CHECK(iotdata_node_content_manifest_find(&node[3].offers, GW_IMAGE) == NULL, "and sensor2, which heard the frame, does not");
    CHECK(node[3].heard == 1u && node[3].dropped == 1u, "it heard it and dropped it, which is different from never hearing it");

    /* 2. OUT OF RANGE. Nothing to decide: the frame never arrives. */
    setup();
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an offer exists");
    hears[0][2] = false; /* the gateway cannot reach sensor1 directly */
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 2, "the relay and sensor2 take it");
    CHECK(node[2].heard == 0u, "sensor1 heard nothing at all");
    CHECK(iotdata_node_content_manifest_find(&node[2].offers, GW_IMAGE) == NULL, "so it knows nothing -- which is what a relay's flood is for");

    /* 3. FILTERED. The node could hear it and chose not to carry it. For a down frame the filter's
          subject is the TARGET, so this is "I have nothing to do with that station" said by a node
          about an offer aimed at it. */
    setup();
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an offer exists");
    CHECK(filter_insert(&node[2].filter, SEN1, FILTER_BLOCK, FILTER_MANUAL), "sensor1 blocks its own id, as a test rig would");
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 0, "the unicast offer is refused");
    CHECK(node[2].filter.stat_blocked == 1u, "and counted as blocked, so it is visible rather than silent");

    /* AND A WHITELIST DOES NOT SILENCE BROADCAST, which is the default the build define picks: a
       node narrowed to one station still hears what is addressed to everybody. */
    setup();
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an offer exists");
    CHECK(filter_insert(&node[2].filter, RLY, FILTER_ALLOW, FILTER_MANUAL), "sensor1 whitelists the relay alone");
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "and still takes a broadcast offer");

    /* 4. NONSENSE. A value that is not a manifest makes no row and is counted: a node being told
          rubbish is a fact worth having, and it is the only one of the four that is a fault. */
    setup();
    uint8_t junk[IOTDATA_CONTENT_MANIFEST_SIZE];
    memset(junk, 0xA5, sizeof(junk)); /* sub-kind 0xA: not one of the three */
    const uint8_t *const tlvs[1] = { junk };
    const size_t lens[1] = { sizeof(junk) };
    CHECK(air_send(&node[0], IOTDATA_STATION_BROADCAST, tlvs, lens, 1u) == 0, "nobody applies it");
    CHECK(node[2].heard == 1u, "it was heard");
    CHECK(node[2].taken == 0u && node[2].refused == 0u, "and discarded on the sub-kind, before the table was troubled");

    junk[0] = (uint8_t)(IOTDATA_CONTENT_MANIFEST << 4); /* now it claims to be a manifest */
    iotdata_node_entry_put_u24(junk, 1, 0x000001u);     /* ... for a scratch id, which is unassignable */
    junk[21] = '\0';
    CHECK(air_send(&node[0], IOTDATA_STATION_BROADCAST, tlvs, lens, 1u) == 0, "an offer for an unassignable id is refused");
    CHECK(node[2].refused == 1u, "and that refusal is counted, because the table did have to judge it");
}

/* THE WHOLE POINT OF THE OFFER, simulated end to end: a sensor hears about an object, decides it
   wants it, pulls it, and then uses the manifest it was given to check what it got. */
static void test_hear_decide_pull_check(void) {
    printf("\nhear an offer, decide to pull, pull it, then check it against the manifest that offered it\n");
    setup();
    node_t *const gw = &node[0];
    node_t *const s1 = &node[2];
    const uint64_t crc = plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "sensor-esp32c3-0.9.11", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "the gateway offers an image");
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 1, "and sensor1 is told about it");

    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&s1->offers, GW_IMAGE);
    CHECK(m != NULL, "sensor1 has the offer");
    if (m == NULL)
        return;

    /* DECIDING IS THE APPLICATION'S, and what the module gives it to decide ON is this: can I
       decode this encoding, is there room, and how much of it have I already got. */
    CHECK(iotdata_content_encoding_compression(m->encoding) == IOTDATA_CONTENT_COMPRESS_NONE, "it is a plain image, so no codec is needed");
    CHECK(iotdata_node_content_space(&s1->content) >= m->size, "and there is room for it");
    uint32_t held = 0u;
    CHECK(!holds(s1, GW_IMAGE, &held) && held == 0u, "with none of it here yet");

    /* NOTHING TO VERIFY AGAINST YET, which is a distinct answer from a bad one: a node that
       verified an absent file against a manifest and said "checksum" would be condemning bytes
       that were never there. */
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "verify says the bytes are absent");

    /* THE PULL. The size and the CRC come FROM THE MANIFEST -- that is the whole of what an offer
       is for, and it is exactly what the requester will pass when CHUNKS frames replace this. */
    CHECK(iotdata_node_content_insert_begin(&s1->content, GW_IMAGE, m->size, m->hash, now_ms) == IOTDATA_NODE_CONTENT_OK, "the transfer is declared from the offer");

    /* ... arriving a frame's worth at a time: seven 32-byte chunks, which is what one 240-byte
       CHUNKS frame carries. Half of it first, so the mid-transfer state is observable. */
    const uint32_t per_frame = 7u * IOTDATA_CONTENT_CHUNK_BYTES;
    uint32_t off = 0u;
    for (uint32_t f = 0; f < 1u; f++) {
        CHECK(iotdata_node_content_insert_write(&s1->content, off, &fixture[off], per_frame, now_ms) == IOTDATA_NODE_CONTENT_OK, "a frame's worth of chunks lands");
        off += per_frame;
    }
    CHECK(iotdata_node_content_insert_active(&s1->content), "the transfer is still running");
    CHECK(iotdata_node_content_tick(&s1->content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_NONE, "and the tick leaves it alone");

    /* MID-TRANSFER OBSERVABILITY. This is the row a manager reads to see a transfer moving, and
       the state it reports is FETCHING with a part of the object. */
    uint8_t row[IOTDATA_NODE_STATUS_CONTENT_ENTRY_SIZE];
    CHECK(iotdata_node_content_table_row(&s1->content, &s1->offers, 0, row, now_ms), "the status row builds");
    CHECK(iotdata_node_entry_get_u24(row, 0) == GW_IMAGE, "for the id being fetched");
    CHECK(row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_FETCHING, "and says FETCHING");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 7u, "seven chunks in");
    CHECK(iotdata_node_entry_get_u16(row, 7) == 14u, "of the fourteen the offer states");
    CHECK(iotdata_node_entry_get_u16(row, 9) == 1440u, "with the offer still standing");

    /* AND VERIFY STILL SAYS ABSENT, because mid-transfer the bytes are in a SCRATCH file and
       nothing is under the real id yet -- an insert stages and renames on proof, so a file at a
       real id is always one that passed its checksum. That is why the row above reads its progress
       from the insert and not from the store. ERR_SIZE is the answer for a partial file that is
       NOT an active transfer: a fragment planted by cable, or one a store was left holding. */
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "verify says there is still nothing under that id");

    /* the rest of it */
    while (off < m->size) {
        const uint32_t want = ((m->size - off) < per_frame) ? (m->size - off) : per_frame;
        CHECK(iotdata_node_content_insert_write(&s1->content, off, &fixture[off], want, now_ms) == IOTDATA_NODE_CONTENT_OK, "the remaining chunks land");
        off += want;
    }
    CHECK(iotdata_node_content_tick(&s1->content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and the tick lands it: checksum, rename, announce");
    CHECK(holds(s1, GW_IMAGE, &held) && held == 448u, "sensor1 now holds the whole object");

    /* THE CHECK THE MANIFEST EXISTS FOR. The insert verified the TRANSFER against the CRC it was
       given; this verifies the STORE against the OFFER, which is a different claim and the one
       that still holds after a reboot. */
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_OK, "and it is what the gateway offered");

    uint64_t got = 0u;
    uint32_t len = 0u;
    CHECK(iotdata_node_content_checksum(&s1->content, GW_IMAGE, &got, &len) == IOTDATA_NODE_CONTENT_OK && got == crc, "byte for byte, by its own reading");

    /* AND THE ROW NOW SAYS HOLDING, which is what "this node can serve it" looks like. */
    CHECK(iotdata_node_content_table_row(&s1->content, &s1->offers, 0, row, now_ms), "the row builds");
    CHECK(row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING, "and says HOLDING");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 14u && iotdata_node_entry_get_u16(row, 7) == 14u, "fourteen of fourteen chunks");
    CHECK(iotdata_node_entry_get_u16(row, 11) == 0u, "with nothing stale about it");
}

static void test_a_pull_that_goes_wrong(void) {
    printf("\nthe three ways a pull fails, and what each leaves behind\n");
    setup();
    node_t *const gw = &node[0];
    node_t *const s1 = &node[2];
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an offer exists");
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 1, "sensor1 knows about it");
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&s1->offers, GW_IMAGE);
    CHECK(m != NULL, "with the offer in hand");
    if (m == NULL)
        return;

    /* 1. THE RIGHT NUMBER OF THE WRONG BYTES. The insert's own sum catches it, the scratch is
          destroyed, and NOTHING lands under the real id -- which is the whole reason an insert
          stages to a scratch and renames on proof. */
    CHECK(iotdata_node_content_insert_begin(&s1->content, GW_IMAGE, m->size, m->hash, now_ms) == IOTDATA_NODE_CONTENT_OK, "a transfer begins");
    for (uint32_t off = 0; off < m->size; off += 32u) {
        uint8_t bad[32];
        memset(bad, 0x5A, sizeof(bad));
        CHECK(iotdata_node_content_insert_write(&s1->content, off, bad, sizeof(bad), now_ms) == IOTDATA_NODE_CONTENT_OK, "the wrong bytes are taken without complaint");
    }
    CHECK(iotdata_node_content_tick(&s1->content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "and the tick refuses them");
    CHECK(iotdata_node_content_insert_why(&s1->content) == IOTDATA_NODE_CONTENT_ERR_CHECKSUM, "naming the checksum");
    CHECK(!holds(s1, GW_IMAGE, NULL), "nothing landed under the id");
    CHECK(iotdata_node_content_count(&s1->content) == 0u, "and no scratch was left behind either");
    /* THE OFFER SURVIVES A FAILED PULL, which is what makes retrying possible at all. */
    CHECK(iotdata_node_content_manifest_find(&s1->offers, GW_IMAGE) != NULL, "and the offer is still there to try again from");

    /* 2. A TRANSFER THAT STOPS. Nothing arrives, the insert times out, and again nothing lands.
          On the air this is the far end going quiet mid-object. */
    CHECK(iotdata_node_content_insert_begin(&s1->content, GW_IMAGE, m->size, m->hash, now_ms) == IOTDATA_NODE_CONTENT_OK, "another begins");
    CHECK(iotdata_node_content_insert_write(&s1->content, 0u, fixture, 32u, now_ms) == IOTDATA_NODE_CONTENT_OK, "one chunk arrives");
    const uint32_t later = now_ms + (uint32_t)IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S * 1000u;
    CHECK(iotdata_node_content_tick(&s1->content, later) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "and then silence");
    CHECK(iotdata_node_content_insert_why(&s1->content) == IOTDATA_NODE_CONTENT_ERR_TIMEOUT, "named as a timeout");
    CHECK(!holds(s1, GW_IMAGE, NULL), "nothing landed");

    /* 3. BYTES THAT PASS THEIR OWN SUM AND ARE STILL NOT THE OFFER'S. This is the case the insert
          cannot catch, because it was told the wrong CRC in the first place -- a stale manifest, a
          wrong id, an operator's typo. verify() against the OFFER is what catches it, and it is
          why the manifest's hash is kept rather than consumed. */
    uint64_t wrong_crc = 0u;
    for (uint32_t i = 0; i < m->size; i++)
        fixture[i] = (uint8_t)(0x77u + i);
    wrong_crc = _iotdata_crc64(fixture, m->size, _IOTDATA_CRC64_INIT);
    CHECK(wrong_crc != m->hash, "a different object of the same length");
    CHECK(iotdata_node_content_insert_begin(&s1->content, GW_IMAGE, m->size, wrong_crc, now_ms) == IOTDATA_NODE_CONTENT_OK, "pulled against the wrong sum");
    for (uint32_t off = 0; off < m->size; off += 32u)
        CHECK(iotdata_node_content_insert_write(&s1->content, off, &fixture[off], 32u, now_ms) == IOTDATA_NODE_CONTENT_OK, "and it all arrives");
    CHECK(iotdata_node_content_tick(&s1->content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "the transfer is self-consistent and lands");
    CHECK(holds(s1, GW_IMAGE, NULL), "so the bytes are in the store");
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_ERR_CHECKSUM, "AND THE OFFER CONDEMNS THEM");

    /* The row still reports HOLDING, and that is correct rather than a gap: the store holds a
       complete object. Whether it is the RIGHT object is the manifest's verdict, not the store's,
       and a node that conflated the two would have no way to say "complete but wrong". */
    uint8_t row[IOTDATA_NODE_STATUS_CONTENT_ENTRY_SIZE];
    CHECK(iotdata_node_content_table_row(&s1->content, &s1->offers, 0, row, now_ms) && row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING, "the store's row says complete");
}

static void test_a_replaced_offer_destroys_what_was_pulled(void) {
    printf("\nreplacing content in place: the new offer destroys the old bytes wherever they got to\n");
    setup();
    node_t *const gw = &node[0];
    node_t *const s1 = &node[2];
    const uint64_t first = plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "0.9.11", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "offered");
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 1, "and told to sensor1");

    /* sensor1 pulls it */
    CHECK(iotdata_node_content_insert_begin(&s1->content, GW_IMAGE, 448u, first, now_ms) == IOTDATA_NODE_CONTENT_OK, "pulled");
    for (uint32_t off = 0; off < 448u; off += 32u)
        (void)iotdata_node_content_insert_write(&s1->content, off, &fixture[off], 32u, now_ms);
    CHECK(iotdata_node_content_tick(&s1->content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and holds it");
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_OK, "and it checks out");

    /* THE SAME ID, NEW BYTES. §3 makes this safe deliberately -- the recommended allocation wraps
       after 4096 ids -- and it gives "content replaced in place" for nothing. */
    const uint64_t second = plant(gw, GW_IMAGE, 960u, 0x44u);
    CHECK(second != first, "the gateway now has something else under that id");
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "0.9.12", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "and re-authors the offer");
    CHECK(advertise(gw, GW_IMAGE, SEN1) == 1, "and says so");

    CHECK(!holds(s1, GW_IMAGE, NULL), "sensor1's copy is GONE, rather than failing verification forever");
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&s1->offers, GW_IMAGE);
    CHECK(m != NULL && m->size == 960u && m->hash == second, "and the offer it now has is the new one");
    CHECK(s1->offers.stat_superseded == 1u, "counted as superseded, which is what makes this visible");
    CHECK(iotdata_node_content_manifest_verify(&s1->offers, &s1->content, GW_IMAGE) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "so it is back to having nothing of it");
}

static void test_revocation_reaches_a_fleet(void) {
    printf("\na revocation travels the same way an offer does, and evicts every copy it reaches\n");
    setup();
    node_t *const gw = &node[0];
    const uint64_t crc = plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "bad build", 0u, 1440u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "offered");
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "and everyone hears");

    /* both sensors pull it, and the relay caches it as a passing transfer would */
    for (int i = 1; i < NODES; i++) {
        CHECK(iotdata_node_content_insert_begin(&node[i].content, GW_IMAGE, 448u, crc, now_ms) == IOTDATA_NODE_CONTENT_OK, "a node pulls it");
        for (uint32_t off = 0; off < 448u; off += 32u)
            (void)iotdata_node_content_insert_write(&node[i].content, off, &fixture[off], 32u, now_ms);
        CHECK(iotdata_node_content_tick(&node[i].content, now_ms) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and holds it");
    }

    /* THE BAD BUILD IS WITHDRAWN. One frame, and it must DESTROY rather than mark: a cache that
       survives a reboot having kept the revocation as a live flag comes back serving it. */
    uint8_t val[64];
    const size_t n = iotdata_node_content_manifest_advertise_revoke(GW_IMAGE, val, sizeof(val));
    const uint8_t *const tlvs[1] = { val };
    const size_t lens[1] = { n };
    CHECK(air_send(gw, IOTDATA_STATION_BROADCAST, tlvs, lens, 1u) == 3, "all three take the revocation");
    CHECK(iotdata_node_content_manifest_revoke(&gw->offers, &gw->content, GW_IMAGE, now_ms) == IOTDATA_NODE_CONTENT_OK, "and the gateway withdraws its own");

    for (int i = 0; i < NODES; i++) {
        CHECK(!holds(&node[i], GW_IMAGE, NULL), "the bytes are gone");
        CHECK(iotdata_node_content_manifest_find(&node[i].offers, GW_IMAGE) == NULL, "and so is the offer");
        CHECK(iotdata_node_content_table_count(&node[i].content, &node[i].offers) == 0u, "with nothing left in the table to report");
    }

    /* A NODE THAT WAS ASLEEP FOR THE REVOCATION STILL HAS IT, and that is the accepted cost of a
       flood: what saves it is that nothing re-advertises the offer, so the entry ages out on its
       own expiry. An INDEFINITE one would not, which is why eviction is its only backstop. */
    setup();
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "short-lived", 0u, 30u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "offered for thirty minutes");
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "everyone hears");
    hears[0][2] = hears[1][2] = hears[3][2] = false; /* sensor1 goes dark */
    CHECK(air_send(gw, IOTDATA_STATION_BROADCAST, tlvs, lens, 1u) == 2, "the revocation reaches only two of them");
    CHECK(iotdata_node_content_manifest_find(&node[2].offers, GW_IMAGE) != NULL, "sensor1 still believes the offer");
    CHECK(iotdata_node_content_manifests_tick(&node[2].offers, &node[2].content, now_ms + 30u * 60u * 1000u) == 1, "until it runs out on its own");
    CHECK(iotdata_node_content_manifest_find(&node[2].offers, GW_IMAGE) == NULL, "and then it does not");
}

static void test_re_origination_is_what_keeps_an_offer_discoverable(void) {
    printf("\nan offer has to be re-originated, and each origination differs -- or nothing new ever hears it\n");
    setup();
    node_t *const gw = &node[0];
    (void)plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, IOTDATA_CONTENT_EXPIRY_INDEFINITE, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "an indefinite offer");

    /* sensor2 is not there yet -- it has not been commissioned, or it is out of range */
    hears[0][3] = false;
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 2, "two of the three hear the first origination");
    CHECK(iotdata_node_content_manifest_find(&node[3].offers, GW_IMAGE) == NULL, "sensor2 knows nothing");

    /* AN ADVERTISEMENT IS NOT REQUESTABLE. There is no "send me your manifest", so a node that
       joins late has no way to ask -- which is exactly why re-origination is a requirement and not
       an optimisation. sensor2 arrives, and the next refresh is what reaches it. */
    hears[0][3] = true;
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "and the refresh reaches all three");
    const iotdata_node_content_manifest_t *const late = iotdata_node_content_manifest_find(&node[3].offers, GW_IMAGE);
    CHECK(late != NULL, "sensor2 now knows about an object nobody could have told it to ask for");
    CHECK(late != NULL && late->generation == 2u, "and the generation says which origination it heard");

    /* THE FRAMES DIFFER, which is the only reason the refresh is not deduped away by every relay
       that remembers the last one. Byte-compare two consecutive originations. */
    uint8_t a[128], b[128];
    const size_t na = iotdata_node_content_manifest_advertise(&gw->offers, &gw->content, GW_IMAGE, a, sizeof(a), now_ms);
    const size_t nb = iotdata_node_content_manifest_advertise(&gw->offers, &gw->content, GW_IMAGE, b, sizeof(b), now_ms);
    CHECK(na == nb && na > 0u, "same offer, same size");
    CHECK(memcmp(a, b, na) != 0, "and never the same bytes twice running");
}

/* The observability the bring-up is read through: a node's whole content picture as 13-byte rows,
   with every combination of held and offered present at once. */
static void test_the_fleet_as_status_rows(void) {
    printf("\nwhat a manager sees: held with an offer, held without one, offered and not held\n");
    setup();
    node_t *const gw = &node[0];
    node_t *const rly = &node[1];

    /* the relay: a blob of its own by cable (held, and offered by itself), the gateway's image
       half-pulled (offered, partly held), and a stray file nobody has described (held, no offer) */
    (void)plant(rly, RLY_BLOB, 64u, 0x33u);
    CHECK(iotdata_node_content_manifest_author(&rly->offers, &rly->content, RLY_BLOB, "calibration", 0u, IOTDATA_CONTENT_EXPIRY_INDEFINITE, 0u, now_ms) == IOTDATA_NODE_CONTENT_OK, "the relay offers its blob");
    (void)plant(rly, 0x68E009u, 96u, 0x55u); /* a stray: on the store, described by nothing */

    const uint64_t crc = plant(gw, GW_IMAGE, 448u, 0x11u);
    CHECK(iotdata_node_content_manifest_author(&gw->offers, &gw->content, GW_IMAGE, "image", 0u, 600u, GW_SIG, now_ms) == IOTDATA_NODE_CONTENT_OK, "the gateway offers an image");
    CHECK(advertise(gw, GW_IMAGE, IOTDATA_STATION_BROADCAST) == 3, "everyone hears it");

    /* the relay starts pulling and stops halfway -- a cache mid-fill, which is the state a relay
       spends most of its life in */
    CHECK(iotdata_node_content_insert_begin(&rly->content, GW_IMAGE, 448u, crc, now_ms) == IOTDATA_NODE_CONTENT_OK, "it starts");
    (void)iotdata_node_content_insert_write(&rly->content, 0u, fixture, 224u, now_ms);

    /* THREE IDS, IN ID ORDER: 001003 (offered, partly held), 68E001 (held and offered), 68E009
       (held, undescribed). The scratch the insert is writing to does not appear -- a half-built
       file is nobody's business. */
    CHECK(iotdata_node_content_table_count(&rly->content, &rly->offers) == 3u, "three rows");
    uint8_t row[IOTDATA_NODE_STATUS_CONTENT_ENTRY_SIZE];

    CHECK(iotdata_node_content_table_row(&rly->content, &rly->offers, 0, row, now_ms), "row 0");
    CHECK(iotdata_node_entry_get_u24(row, 0) == GW_IMAGE, "the gateway's image");
    CHECK(row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_FETCHING, "being fetched");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 7u && iotdata_node_entry_get_u16(row, 7) == 14u, "seven of fourteen chunks");
    CHECK(iotdata_node_entry_get_u16(row, 9) == 600u, "against an offer with ten hours on it");

    CHECK(iotdata_node_content_table_row(&rly->content, &rly->offers, 1, row, now_ms), "row 1");
    CHECK(iotdata_node_entry_get_u24(row, 0) == RLY_BLOB, "its own blob");
    CHECK(row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING, "held whole");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 2u && iotdata_node_entry_get_u16(row, 7) == 2u, "two of two chunks");
    CHECK(iotdata_node_entry_get_u16(row, 9) == IOTDATA_CONTENT_EXPIRY_INDEFINITE, "offered indefinitely");

    CHECK(iotdata_node_content_table_row(&rly->content, &rly->offers, 2, row, now_ms), "row 2");
    CHECK(iotdata_node_entry_get_u24(row, 0) == 0x68E009u, "and the stray");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 3u, "three chunks of it held");
    CHECK(iotdata_node_entry_get_u16(row, 7) == 0u, "described by nothing, which is what 0 means");
    CHECK(iotdata_node_entry_get_u16(row, 9) == 0u, "and so no expiry to report");

    /* A SENSOR THAT HAS ONLY BEEN TOLD. One row, nothing held -- and this is the row that proves
       an advertisement arrived, which is the whole of the first bring-up step. */
    CHECK(iotdata_node_content_table_count(&node[2].content, &node[2].offers) == 1u, "sensor1 has one row");
    CHECK(iotdata_node_content_table_row(&node[2].content, &node[2].offers, 0, row, now_ms), "and it builds");
    CHECK(iotdata_node_entry_get_u24(row, 0) == GW_IMAGE && iotdata_node_entry_get_u16(row, 5) == 0u && iotdata_node_entry_get_u16(row, 7) == 14u, "an object it knows the size of and has none of");

    /* THE SAME PICTURE IN WORDS, so an operator at a console and a manager over MQTT are looking
       at one thing described one way. */
    char line[IOTDATA_NODE_CONTENT_MANIFEST_STR_MAX + 1];
    uint8_t cursor = 0;
    const iotdata_node_content_manifest_t *m = NULL;
    int printed = 0;
    while (iotdata_node_content_manifest_iterate(&rly->offers, &cursor, &m)) {
        printf("  %s: %s\n", rly->name, iotdata_node_content_manifest_str(line, sizeof(line), m, now_ms));
        printed++;
    }
    CHECK(printed == 2, "the relay lists the two offers it knows, and not the stray it merely holds");
}

/* ------------------------------------------------------------------------------------------- */

/*
 * A NODE IS TOLD, OVER THE AIR, TO OFFER A FILE IT ALREADY HAS.
 *
 * The case this is for: a node holds local files for its own reasons -- planted at the factory,
 * written by a cable, produced by the thing it does -- and somebody decides the fleet should be
 * able to fetch one of them. There is no console to type at; a sensor built from the simple app
 * template has none at all. So the instruction goes over the air as a CONTROL frame, and what
 * comes out the other side is the node ADVERTISING its own content, as the originator, because
 * that is what it is.
 *
 * This is the one path into the offers table that nothing else here covered: the console has its
 * own suite, MQTT reaches a gateway that executes locally, and every other test in this file puts
 * a MANIFEST on the air -- a STATEMENT about somebody else's content. A command is the other
 * direction entirely, and the two arrive through the same door.
 */
static void test_a_node_is_told_over_the_air_to_offer_what_it_holds(void) {
    printf("a node is told over the air to offer a file it already holds\n");
    setup();
    node_t *const gw = &node[0], *const sen = &node[2];

    /* the local file, there for its own reasons and described by nobody */
    const uint32_t LOCAL = 0x57D00Au;
    const uint64_t sum = plant(sen, LOCAL, 320u, 0x31u);
    CHECK(sum != 0u, "the sensor holds a file of its own, by cable or by factory");
    CHECK(iotdata_node_content_manifest_find(&sen->offers, LOCAL) == NULL, "and nothing offers it: holding is not offering");

    /* the gateway tells it to start offering that file */
    uint8_t n = build_set(LOCAL, 5u, "logs-0.1");
    CHECK(air_command(gw, SEN1, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set_args, n) > 0, "a MANIFEST_SET goes out addressed to the sensor");
    CHECK(sen->commanded == 1u && sen->declined == 0u, "the sensor carries it out");

    const iotdata_node_content_manifest_t *row = iotdata_node_content_manifest_find(&sen->offers, LOCAL);
    CHECK(row != NULL, "and now there is an offer for it");
    if (row != NULL) {
        CHECK(row->local, "WHICH IS OURS TO ADVERTISE -- the node is the originator, not a node repeating what it heard");
        CHECK(row->size == 320u && row->hash == sum, "describing the bytes that are actually there, read off the store");
        CHECK(row->expiry_m == 5u, "with the life the command gave it");
        CHECK(strcmp(row->name, "logs-0.1") == 0, "and the name, which only the command carried");
    }

    /* AND THE POINT OF ALL THAT: it can now be fetched by somebody who was never told anything. */
    now_ms += 1000u;
    CHECK(advertise(sen, LOCAL, (uint16_t)IOTDATA_STATION_BROADCAST) > 0, "the sensor advertises it, unprompted, as its own");
    const iotdata_node_content_manifest_t *far = iotdata_node_content_manifest_find(&node[3].offers, LOCAL);
    CHECK(far != NULL && !far->local, "and the far sensor hears it as somebody else's offer");
    CHECK(iotdata_node_content_manifest_find(&gw->offers, LOCAL) != NULL, "including the gateway that asked for it, which learns of it the same way as anyone");
}

/*
 * THE SAME COMMAND, REFUSED, AND FOR THE RIGHT REASONS.
 *
 * Two of them, and both are about the node protecting what it says about itself. A manifest is a
 * claim with no authenticity behind it (§8), so the only thing keeping the claims honest is each
 * node refusing to make one it cannot stand behind.
 */
static void test_a_manifest_set_over_the_air_is_refused_for_the_right_reasons(void) {
    printf("\na manifest set over the air is refused for the right reasons\n");
    setup();
    node_t *const gw = &node[0], *const sen = &node[2];

    /* ONE: AN ID THAT IS NOT OURS TO MINT. §4 puts the originating station in the top twelve bits,
       so an id is a statement about who authored the content. A node asked to author one of
       somebody else's refuses -- and this is the check at the point the command is processed,
       which is the only place it can be made: the command is legitimate, the id is not. */
    uint8_t n = build_set(0x001ABCu, 5u, "not-ours");
    CHECK(air_command(gw, SEN1, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set_args, n) > 0, "the gateway names an id out of its OWN range");
    CHECK(sen->commanded == 0u && sen->declined == 1u, "the sensor refuses it");
    CHECK(sen->refusal != NULL && strstr(sen->refusal, "another station") != NULL, "saying that the id belongs to another station, which is the useful answer");
    CHECK(iotdata_node_content_manifest_find(&sen->offers, 0x001ABCu) == NULL, "and no offer is made");

    /* TWO: NOTHING TO DESCRIBE. An offer whose bytes are absent is an invitation to fetch nothing,
       and the requester would find out only after asking. The store is consulted first. */
    sen->declined = 0u;
    sen->refusal = NULL;
    n = build_set(0x57D0FFu, 5u, "nothing-here");
    CHECK(air_command(gw, SEN1, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set_args, n) > 0, "an id in the sensor's own range, but holding nothing");
    CHECK(sen->commanded == 0u && sen->declined == 1u, "refused as well");
    CHECK(iotdata_node_content_manifest_find(&sen->offers, 0x57D0FFu) == NULL, "with nothing left behind");

    /* THREE: AND A REVOKE IS THE SAME COMMAND WITH NOTHING LEFT TO LIVE. §3 makes expiry 0 the
       withdrawal, so the verb an operator needs over the air is one they already have. */
    const uint32_t LOCAL = 0x57D00Bu;
    CHECK(plant(sen, LOCAL, 96u, 0x44u) != 0u, "a file the sensor holds");
    n = build_set(LOCAL, 5u, "briefly-0.1");
    (void)air_command(gw, SEN1, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set_args, n);
    CHECK(iotdata_node_content_manifest_find(&sen->offers, LOCAL) != NULL, "offered, when told to be");
    n = build_set(LOCAL, 0u, "briefly-0.1");
    (void)air_command(gw, SEN1, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, set_args, n);
    const iotdata_node_content_manifest_t *const row = iotdata_node_content_manifest_find(&sen->offers, LOCAL);
    CHECK(row == NULL || row->expiry_m == 0u, "and withdrawn by an expiry of zero, told the same way");
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_content_lifecycle\n");
    test_an_offer_reaches_everyone_who_can_hear_it();
    test_several_flavours_in_one_frame();
    test_offers_from_several_places_coexist();
    test_who_ignores_an_offer_and_why();
    test_hear_decide_pull_check();
    test_a_pull_that_goes_wrong();
    test_a_replaced_offer_destroys_what_was_pulled();
    test_revocation_reaches_a_fleet();
    test_re_origination_is_what_keeps_an_offer_discoverable();
    test_the_fleet_as_status_rows();
    test_a_node_is_told_over_the_air_to_offer_what_it_holds();
    test_a_manifest_set_over_the_air_is_refused_for_the_right_reasons();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
