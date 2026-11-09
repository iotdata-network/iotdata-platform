//
// test_node_content_manifest.c - the OFFERS table: what a node knows is on offer, as opposed to
// what it is holding.
//
// Still no radio. The MANIFEST codec is the library's and is round-tripped there; what is under
// test here is the half that cannot be checked by encoding and decoding -- the rules §3 states in
// prose and nothing else enforces:
//
//   a manifest an operator AUTHORS and one that ARRIVES are the same apply(), so they cannot drift
//   size and hash come from the STORE when authoring, so there is nothing to mistype that matters
//   a differing size or hash DESTROYS the bytes, which is what makes id reuse safe
//   revocation destroys; expiry merely forgets -- the one asymmetry in the lifecycle
//   only the advertiser originates, so a heard offer cannot be re-advertised from a cache
//   every origination bumps the generation, because a byte-identical refresh would be deduped away
//
// The last two are the ones that would otherwise be discovered on a bench, days later, as "the
// sensors stopped hearing about the image".
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
#include "iotdata_node_content.h"
#include "d_module_filestore_ram.h"
#include "iotdata_node_content_manifest.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

#define ID_A       0x001003u /* the gateway is station 1, so its ids are 0x001xxx */
#define ID_B       0x001004u
#define ID_SIG     0x001009u
#define ID_T       0x601001u /* another station's content, which is what a HEARD offer must name: */
#define ID_T2      0x601002u /* §4 gives an id's top twelve bits to its originator, and the offers  */
#define ID_TSIG    0x601009u /* table refuses anything off the air that claims one of ours.         */
#define MY_STATION 0x001u    /* the gateway is station 1, so ID_A..ID_SIG are ours and 0x6xxxxx are not */

static filestore_ram_t store;
static const d_filestore_t *backend;
static iotdata_node_content_t content;
static iotdata_node_content_manifests_t offers;

/* A file of `size` bytes whose content is a function of the offset, so two files of the same length
   differ -- which is what the supersede rule has to be able to see. */
static uint8_t fixture[1024];

static uint64_t put_file(const uint32_t id, const uint32_t size, const uint8_t seed) {
    if (size > sizeof(fixture))
        return 0u;
    for (uint32_t i = 0; i < size; i++)
        fixture[i] = (uint8_t)(seed + i);
    if (!filestore_ram_plant(&store, id, fixture, size))
        return 0u;
    return _iotdata_crc64(fixture, size, _IOTDATA_CRC64_INIT);
}

static void setup(void) {
    backend = filestore_ram_init(&store);
    iotdata_node_content_init(&content, backend);
    (void)iotdata_node_content_begin(&content, 1000u);
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
}

static bool holds(const uint32_t id) {
    uint32_t total = 0u, held = 0u;
    return iotdata_node_content_stat(&content, id, &total, &held);
}

/* ------------------------------------------------------------------------------------------- */

static void test_authoring_reads_the_store(void) {
    printf("\nauthoring a manifest takes the size and the hash from the bytes, not from the operator\n");
    setup();
    const uint64_t crc = put_file(ID_A, 448u, 0x10u);
    CHECK(crc != 0u, "the fixture file was written");

    const iotdata_node_content_status_t rc = iotdata_node_content_manifest_author(&offers, &content, ID_A, "sensor-esp32c3-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BINARY, IOTDATA_CONTENT_COMPRESS_NONE), 1440u, ID_SIG, 2000u);
    CHECK(rc == IOTDATA_NODE_CONTENT_OK, "it was authored");

    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL, "and the table has it");
    if (m != NULL) {
        CHECK(m->size == 448u, "the size came off the store");
        CHECK(m->hash == crc, "and so did the hash, agreeing with what the bytes sum to");
        CHECK(m->local, "it is ours: we are the advertiser");
        CHECK(m->generation == 0u, "a first offer has had no origination yet");
        CHECK(strcmp(m->name, "sensor-esp32c3-0.9.11") == 0, "the name is the operator's");
        CHECK(iotdata_node_content_manifest_remaining_m(m, 2000u) == 1440u, "the whole expiry remains, at the moment it was stated");
    }
    CHECK(offers.stat_authored == 1u, "counted as authored");
    CHECK(iotdata_node_content_manifests_count(&offers) == 1u, "one offer");

    /* WHAT THE BYTES CANNOT SAY IS ALL THE OPERATOR SAYS, so nothing here needs a size argument --
       and an id the store does not have is refused rather than described from nothing. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "absent", 0u, 60u, 0u, 2000u) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "a manifest for a file we do not hold is refused");
    CHECK(iotdata_node_content_manifests_count(&offers) == 1u, "and leaves no row behind");

    /* A PARTIAL FILE HAS NO HASH WORTH ADVERTISING. checksum() sums what is there and says it is
       short, and authoring has to take that as a refusal rather than as a number. */
    CHECK(backend->create(backend->ctx, ID_B, 448u), "a file declared at 448");
    CHECK(backend->write(backend->ctx, ID_B, 0u, "half", 4u) == 4, "with four bytes in it");
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "partial", 0u, 60u, 0u, 2000u) == IOTDATA_NODE_CONTENT_ERR_SIZE, "a manifest for half a file is refused");

    /* A SCRATCH ID IS MALFORMED BY CONSTRUCTION and can never be advertised, requested or answered,
       so it cannot be authored either. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, 0x000001u, NULL, 0u, 60u, 0u, 2000u) == IOTDATA_NODE_CONTENT_ERR_ID, "a scratch id is not a content id");
    CHECK(iotdata_node_content_manifest_author(&offers, &content, 0xFFF001u, NULL, 0u, 60u, 0u, 2000u) == IOTDATA_NODE_CONTENT_ERR_ID, "nor is the broadcast range");
}

static void test_the_wire_and_the_console_are_one_path(void) {
    printf("\na manifest that arrives and one that is authored land in the same table the same way\n");
    setup();

    /* Build one as an advertiser would, then feed it in as a receiver. */
    uint8_t frame[128];
    const iotdata_content_manifest_t w = {
        .id = ID_T,
        .generation = 3u,
        .size = 448u,
        .hash = 0xA1B2C3D4E5F60718ull,
        .sig_id = ID_TSIG,
        .expiry_m = 120u,
        .encoding = IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BSDIFF, IOTDATA_CONTENT_COMPRESS_HEATSHRINK),
        .name = "relay-esp32c3-0.9.10-to-0.9.11",
    };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &w);
    CHECK(n > 0u, "the advertisement packed");

    uint32_t id = 0u;
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 5000u, &id) == IOTDATA_NODE_CONTENT_OK, "and was taken");
    CHECK(id == ID_T, "the id is reported back, so a refusal can name the offer");

    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, ID_T);
    CHECK(m != NULL, "the table has it");
    if (m != NULL) {
        CHECK(m->size == 448u && m->hash == w.hash && m->sig_id == ID_TSIG, "every field survived");
        CHECK(m->generation == 3u, "including the generation it was originated with");
        CHECK(m->encoding == w.encoding, "and the encoding nibbles");
        CHECK(!m->local, "it is heard, not ours");
        CHECK(strcmp(m->name, w.name) == 0, "and the name was copied out of the frame");
    }
    CHECK(offers.stat_heard == 1u, "counted as heard");

    /* A NAME LONGER THAN A ROW IS TRUNCATED AND THE OFFER STANDS: the name is descriptive, and
       losing its tail is a worse listing rather than a wrong offer. */
    char longname[IOTDATA_NODE_CONTENT_NAME_MAX + 40];
    memset(longname, 'z', sizeof(longname) - 1u);
    longname[sizeof(longname) - 1u] = '\0';
    const iotdata_content_manifest_t big = { .id = ID_T2, .size = 64u, .sig_id = ID_TSIG, .expiry_m = 60u, .name = longname };
    const size_t bn = iotdata_content_manifest_pack(frame, sizeof(frame), &big);
    CHECK(bn > 0u && iotdata_node_content_manifest_on_wire(&offers, &content, frame, bn, 5000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a long name is taken");
    m = iotdata_node_content_manifest_find(&offers, ID_T2);
    CHECK(m != NULL && strlen(m->name) == (size_t)IOTDATA_NODE_CONTENT_NAME_MAX, "truncated to what a row holds");

    /* NOT A MANIFEST AT ALL. A value the codec refuses must not make a row, and must be counted:
       a node being told nonsense is a fact worth having. */
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, 4u, 5000u, NULL) != IOTDATA_NODE_CONTENT_OK, "a value too short is refused");
    CHECK(offers.stat_refused >= 1u, "and counted");
    CHECK(iotdata_node_content_manifests_count(&offers) == 2u, "no row was made");
}

static void test_supersede_destroys_the_bytes(void) {
    printf("\nan offer that disagrees about the bytes destroys them -- which is what makes id reuse safe\n");
    uint8_t frame[128];

    /* OUR OWN OFFER COMING BACK IS NOT A FRESH STATEMENT, and §4 is what says so: the id's top
       twelve bits are our station, so nothing off the air may name it whatever the table holds.
       An advertisement floods and returns off every relay that heard it, and a down frame carries
       no sender, so this is indistinguishable from somebody else claiming our content. */
    setup();
    const uint64_t crc = put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "first", 0u, 600u, ID_SIG, 2000u) == IOTDATA_NODE_CONTENT_OK, "authored over the file");
    CHECK(holds(ID_A), "and the file is there");
    const iotdata_content_manifest_t same = { .id = ID_A, .generation = 1u, .size = 448u, .hash = crc, .sig_id = ID_SIG, .expiry_m = 600u, .name = "first" };
    size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &same);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 3000u, NULL) == IOTDATA_NODE_CONTENT_ERR_STATION, "our own offer coming back is refused");
    CHECK(offers.stat_foreign == 1u, "and counted as an id claimed by the wrong station");
    CHECK(holds(ID_A), "and it certainly does not destroy the file");
    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && m->local, "the row is still ours");

    /* AND IT IS REFUSED WITH NO ROW AT ALL, which the table-state guard could not manage: an offer
       of ours that had expired or been crowded out left our own content reachable from the air,
       and a revocation naming it would have deleted the bytes. */
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 3100u, NULL) == IOTDATA_NODE_CONTENT_ERR_STATION, "still refused with an empty table");
    CHECK(holds(ID_A), "and the bytes are still ours");

    /* A DIFFERENT SIZE UNDER AN ID WE HEARD ABOUT: the bytes we fetched are not the bytes now
       being offered, so they go. This is the case the rule exists for -- another station re-using
       its own counter, which §4's allocation wraps after 4096. */
    setup();
    const uint64_t theirs = put_file(ID_T, 448u, 0x10u);
    const iotdata_content_manifest_t first_offer = { .id = ID_T, .size = 448u, .hash = theirs, .sig_id = ID_TSIG, .expiry_m = 600u, .name = "first" };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &first_offer);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 2000u, NULL) == IOTDATA_NODE_CONTENT_OK, "their offer is known, and we hold the bytes");
    const iotdata_content_manifest_t bigger = { .id = ID_T, .size = 960u, .hash = 0xDEADBEEFull, .sig_id = ID_TSIG, .expiry_m = 600u, .name = "second" };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &bigger);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 3000u, NULL) == IOTDATA_NODE_CONTENT_OK, "the new offer is taken");
    CHECK(!holds(ID_T), "and the bytes that were under that id are gone");
    CHECK(offers.stat_superseded == 1u, "counted as superseded");
    m = iotdata_node_content_manifest_find(&offers, ID_T);
    CHECK(m != NULL && m->size == 960u, "the row describes the new object");

    /* SAME SIZE, DIFFERENT HASH -- which only the manifest already in hand can detect, since the
       store can only say how long a file is. */
    setup();
    const uint64_t one = put_file(ID_T, 448u, 0x10u);
    const iotdata_content_manifest_t as_heard = { .id = ID_T, .size = 448u, .hash = one, .sig_id = ID_TSIG, .expiry_m = 600u, .name = "first" };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &as_heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 3000u, NULL) == IOTDATA_NODE_CONTENT_OK, "the first offer is known");
    CHECK(holds(ID_T), "the bytes are still here");
    const iotdata_content_manifest_t rehash = { .id = ID_T, .size = 448u, .hash = one ^ 1u, .sig_id = ID_TSIG, .expiry_m = 600u, .name = "same length, other bytes" };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &rehash);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 4000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and the second arrives");
    CHECK(!holds(ID_T), "a hash that disagrees destroys them too, at the same length");
    CHECK(offers.stat_superseded == 1u, "counted");
}

/* §4'S ALLOCATION, HELD UP AT BOTH ENDS. The top twelve bits of a content id are its originating
   station, and the architecture leans on that everywhere -- so the offers table enforces it in
   both directions rather than trusting it. */
static void test_only_our_own_station_may_name_our_ids(void) {
    printf("\nonly the station in an id's top bits may originate it -- in both directions\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    (void)put_file(ID_T, 64u, 0x20u);

    /* WE MAY NOT DESCRIBE CONTENT UNDER SOMEBODY ELSE'S ID. Without this a node could author an
       offer for a neighbour's id -- a plausible slip when content arrives by cable -- and would
       then refuse that neighbour's own updates AND its revocations while serving the bytes on.
       Immovable, and the sort of thing that surfaces months later. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_T, "not ours to offer", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_ERR_STATION, "authoring a foreign id is refused");
    CHECK(offers.stat_foreign == 1u, "and counted");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_T) == NULL, "with no row made");
    CHECK(holds(ID_T), "and the bytes left alone: holding content is not claiming it");

    /* WE MAY DESCRIBE OUR OWN, which is the ordinary path -- including content put there by hand
       or by a sidecar device, which is exactly what a station's own id range is for. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ours", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "and our own is authored");

    /* NOBODY ELSE MAY ADVERTISE ONE OF OURS, table or no table -- tested above in supersede. And
       a station that has not been told who it is enforces neither, falling back to the weaker
       table-state guard: a host harness, or a node whose station is not settled at mount time. */
    iotdata_node_content_manifests_init(&offers, true, 0u);
    uint8_t frame[128];
    const iotdata_content_manifest_t mine = { .id = ID_A, .size = 448u, .hash = 7u, .sig_id = ID_SIG, .expiry_m = 600u, .name = "whoever" };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &mine);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an unconfigured station cannot tell, and says so by taking it");
    CHECK(offers.stat_foreign == 0u, "without pretending it checked");
}

static void test_revocation_destroys_and_expiry_forgets(void) {
    printf("\nrevocation evicts the copy; expiry only stops the offer\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "doomed", 0u, 10u, ID_SIG, 2000u) == IOTDATA_NODE_CONTENT_OK, "authored with ten minutes on it");

    /* EXPIRY: the row goes, the bytes stay. A node stops offering it and keeps what it has. */
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 2000u + 9u * 60u * 1000u) == 0, "nothing expires early");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) != NULL, "still offered at nine minutes");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 2000u + 10u * 60u * 1000u) == 1, "and goes at ten");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "the offer is forgotten");
    CHECK(holds(ID_A), "AND THE BYTES ARE STILL HERE: expiring is not revoking");
    CHECK(offers.stat_expired == 1u, "counted as expired");

    /* REVOCATION: both go, and nothing is left to say it was revoked -- a stored flag is what
       would come back after a reboot still serving the bytes.
     *
     * THE ADVERTISER'S REVOCATION, OFF THE AIR, FOR AN OFFER WE HEARD. This is the case the
       mechanism exists for: somebody told us about content, we may have fetched it, and now they
       say it is withdrawn. */
    uint8_t frame[64];
    const uint32_t ID_X = 0x601001u;
    const uint64_t hx = put_file(ID_X, 64u, 0x30u);
    const iotdata_content_manifest_t theirs = { .id = ID_X, .size = 64u, .hash = hx, .sig_id = ID_SIG, .expiry_m = 600u, .name = "theirs" };
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &theirs), 9000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an offer we heard, whose bytes we hold");
    const size_t n = iotdata_node_content_manifest_advertise_revoke(ID_X, frame, sizeof(frame));
    CHECK(n == (size_t)IOTDATA_CONTENT_MANIFEST_SIZE, "a revocation is the smallest manifest there is");
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 10000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and it is accepted");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_X) == NULL, "the offer is gone");
    CHECK(!holds(ID_X), "and so are the bytes");
    CHECK(offers.stat_revoked == 1u, "counted as revoked");
    CHECK(iotdata_node_content_manifests_count(&offers) == 0u, "with no trace left to come back as a live flag");

    /*
     * BUT NOT FOR AN OFFER OF OURS, and this is the rule that has to be stated here because the
     * earlier version of this test asserted the opposite and passed.
     *
     * §8 gives a manifest no authenticity, so a revocation is a frame anyone can send. While it
     * was honoured for our own ids, one 38-byte frame naming an id a gateway was distributing
     * destroyed the image on it. The ordinary case is just as wrong and far more likely: our own
     * revocation floods, comes back off a relay, and lands as somebody telling us to delete bytes
     * we had already decided about.
     *
     * AN OPERATOR STILL REVOKES, through the local verb -- which is the whole point of having the
     * two paths meet at apply() with `local` as the only difference between them.
     */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ours", 0u, 600u, ID_SIG, 9000u) == IOTDATA_NODE_CONTENT_OK, "an offer of our own");
    const size_t rn = iotdata_node_content_manifest_advertise_revoke(ID_A, frame, sizeof(frame));
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, rn, 10000u, NULL) != IOTDATA_NODE_CONTENT_OK, "a revocation off the air is refused for it");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) != NULL, "the offer stands");
    CHECK(holds(ID_A), "AND THE BYTES ARE STILL HERE: no frame may destroy what we stand behind");

    CHECK(iotdata_node_content_manifest_revoke(&offers, &content, ID_A, 11000u) == IOTDATA_NODE_CONTENT_OK, "and the operator's own revocation is honoured");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL && !holds(ID_A), "which does take both");

    /* A REVOCATION FOR SOMETHING WE NEVER HEARD OF is not an error. It is the common case for a
       node that was asleep when the offer was made, and refusing it would be refusing to be told. */
    const size_t un = iotdata_node_content_manifest_advertise_revoke(0x602001u, frame, sizeof(frame));
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, un, 11000u, NULL) == IOTDATA_NODE_CONTENT_OK, "revoking an unknown id is fine");

    /* THE REMAINDER NEVER REACHES 0 WHILE THE OFFER LIVES, because 0 in that field is the
       revocation sentinel -- and §13 has a holder re-report its observed remainder in the same
       field an advertisement states one in. A row with seconds left that answered 0 would be
       passing on "destroy your copy" to whoever asked it. */
    setup();
    (void)put_file(ID_A, 64u, 0x30u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "nearly up", 0u, 2u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored with two minutes on it");
    const iotdata_node_content_manifest_t *const nearly = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(nearly != NULL, "the row is there");
    if (nearly != NULL) {
        CHECK(iotdata_node_content_manifest_remaining_m(nearly, 1000u + 119u * 1000u) == 1u, "one second short of expiry still reports a minute");
        CHECK(iotdata_node_content_manifest_remaining_m(nearly, 1000u + 120u * 1000u) == 0u, "and 0 arrives exactly when the offer does expire");
    }
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + 119u * 1000u) == 0, "so nothing is dropped a second early");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + 120u * 1000u) == 1, "and it goes on the tick after");

    /* INDEFINITE NEVER COUNTS DOWN, which is the common case rather than an edge one: an offer
       should stand until it is replaced. */
    setup();
    (void)put_file(ID_B, 64u, 0x20u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "forever", 0u, IOTDATA_CONTENT_EXPIRY_INDEFINITE, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored indefinitely");
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&offers, ID_B);
    CHECK(m != NULL && iotdata_node_content_manifest_remaining_m(m, 1000u + 40u * 24u * 60u * 60u * 1000u) == IOTDATA_CONTENT_EXPIRY_INDEFINITE, "forty days later it is still indefinite");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + 40u * 24u * 60u * 60u * 1000u) == 0, "and nothing expires it");
}

static void test_only_the_advertiser_originates(void) {
    printf("\nonly the node that authored an offer may advertise it, and every origination differs\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ours", 0u, 1440u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored here");

    /* ORIGINATIONS ARE SPACED PAST THE FLOOR, because they are in life: §3's cadence is "a few
       frames a week" and IOTDATA_NODE_CONTENT_READVERTISE_MIN_S refuses anything faster. The floor
       itself is tested below; here it is simply respected. */
#define STEP ((uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u + 1000u)
    uint32_t clock_ms = 2000u;
    uint8_t a[128], b[128];
    const size_t na = iotdata_node_content_manifest_advertise(&offers, &content, ID_A, a, sizeof(a), clock_ms);
    CHECK(na > 0u, "and can be advertised");

    /* THE GENERATION IS WHAT MAKES THE FRAME DIFFER. Without it a refresh is byte-identical to the
       one before, lands inside the dedup lifetime by construction, and is thrown by every relay
       that still remembers it -- guaranteed suppression of the one message whose job is to keep
       arriving. */
    clock_ms += STEP;
    const size_t nb = iotdata_node_content_manifest_advertise(&offers, &content, ID_A, b, sizeof(b), clock_ms);
    CHECK(nb == na, "a refresh is the same size");
    CHECK(memcmp(a, b, na) != 0, "AND NOT THE SAME BYTES");
    uint8_t ga = 0, gb = 0;
    CHECK(iotdata_content_header_parse(a, na, NULL, &ga, NULL) && iotdata_content_header_parse(b, nb, NULL, &gb, NULL), "both headers parse");
    CHECK(ga == 1u && gb == 2u, "the generation advanced, from a first offer that had none");

    /* FOUR BITS WRAP AT SIXTEEN, which is ample when originations are hours apart -- what matters
       is that consecutive ones differ, not that they never repeat. */
    for (int i = 0; i < 14; i++) {
        clock_ms += STEP;
        (void)iotdata_node_content_manifest_advertise(&offers, &content, ID_A, a, sizeof(a), clock_ms);
    }
    clock_ms += STEP;
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, a, sizeof(a), clock_ms) > 0u, "still advertises after a wrap");
    uint8_t gw = 0xFF;
    CHECK(iotdata_content_header_parse(a, na, NULL, &gw, NULL) && gw == 1u, "and comes round to 1 again, not to a refused 16");

    /* RE-STATING THE OFFER RESETS ITS OWN REMAINDER, because an advertisement states an expiry
       FROM RECEIPT and we have just re-stated it. */
    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && iotdata_node_content_manifest_remaining_m(m, clock_ms) == 1440u, "the whole expiry again");

    /* A HEARD OFFER CANNOT BE RE-ORIGINATED. The expiry is not decremented when forwarded, so a
       cache that re-stated one would extend its life indefinitely. A relay repeats what it heard
       by flooding the FRAME, which is the down path's job and not this module's. */
    uint8_t frame[128];
    const iotdata_content_manifest_t heard = { .id = ID_T, .size = 64u, .hash = 7u, .sig_id = ID_TSIG, .expiry_m = 60u, .name = "somebody else's" };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, clock_ms, NULL) == IOTDATA_NODE_CONTENT_OK, "we know about it");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_T, a, sizeof(a), clock_ms) == 0u, "and must not re-originate it");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, 0x001FFFu, a, sizeof(a), clock_ms) == 0u, "nor advertise an id we know nothing about");

    /* A BUMP IS NOT SPENT ON A FRAME THAT WAS NEVER BUILT: a buffer too small must leave the
       generation where it was, or the next origination looks like a refresh of something nobody
       ever heard. */
    m = iotdata_node_content_manifest_find(&offers, ID_A);
    const uint8_t before = (m != NULL) ? m->generation : 0xFFu;
    clock_ms += STEP;
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, a, 8u, clock_ms) == 0u, "a buffer too small writes nothing");
    m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && m->generation == before, "and costs no generation");
}

static void test_the_table_is_bounded_and_says_so(void) {
    printf("\nthe offers table is bounded, and a full one is counted rather than quietly dropping an offer\n");
    setup();
    uint8_t frame[64];
    /* ONE OFFER EACH FROM DISTINCT ORIGINATORS, because filling the table from one of them is
       now what the per-origin cap is for and is tested on its own below. §4 puts the station in
       the top twelve bits, so a different originator is a different high nibble. */
    for (uint32_t i = 0; i < (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++) {
        const iotdata_content_manifest_t w = { .id = ((0x002u + i) << 12) | 0x001u, .size = 64u, .hash = i, .sig_id = ID_SIG, .expiry_m = 600u };
        const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &w);
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an offer fits");
    }
    CHECK(iotdata_node_content_manifests_count(&offers) == (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX, "the table is full");

    const iotdata_content_manifest_t one_more = { .id = 0x003000u, .size = 64u, .sig_id = ID_SIG, .expiry_m = 600u };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &one_more);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_ERR_FULL, "and the next one is refused");
    CHECK(offers.stat_full == 1u, "counted, because it means this node is misdimensioned");

    /* A ROW FREED BY EXPIRY IS REUSABLE: the bound is on what is live, not on how many have ever
       been heard. */
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + 600u * 60u * 1000u) == (int)IOTDATA_NODE_CONTENT_MANIFESTS_MAX, "they all expire together");
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u + 600u * 60u * 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and the table takes offers again");
}

/* The check a transfer is checked AGAINST, which is the other half of what an offer is for. The
   four answers are distinct because they call for different things, and conflating two of them
   either throws away a transfer that was merely unfinished or accepts one that was wrong. */
static void test_verify_against_the_offer(void) {
    printf("\nthe manifest is what a pull is checked against, and its four answers mean four things\n");
    setup();
    const uint64_t crc = put_file(ID_A, 448u, 0x10u);

    /* NO MANIFEST: there is nothing to be right or wrong about, and a node that verified against
       nothing would be reporting an opinion as a fact. */
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_ERR_STATE, "with no offer, there is nothing to verify against");

    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "image", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored over the file");
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_OK, "and what we hold is what we offered");

    /* NO BYTES: not started, or destroyed under us. Distinct from a bad sum, because nothing is
       being condemned. */
    CHECK(iotdata_node_content_delete(&content, ID_A) == IOTDATA_NODE_CONTENT_OK, "the file goes");
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "and verify says absent, not corrupt");

    /* FEWER BYTES THAN THE OFFER STATES: fetch the rest. A fragment sums to something, and calling
       that a checksum failure would tell a node to abandon hours of airtime that were fine. */
    CHECK(backend->create(backend->ctx, ID_A, 448u), "a file declared at the right size");
    CHECK(backend->write(backend->ctx, ID_A, 0u, fixture, 224u) == 224, "with half of it present");
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_ERR_SIZE, "verify says there are bytes missing");

    /* A COMPLETE FILE OF THE WRONG LENGTH, which is not the same thing and is reachable the one way
       round that apply() cannot prevent: the offer arrives FIRST and the bytes are put there
       afterwards, by cable or by a factory image, so no apply ever ran to compare them. Checking
       the length against the OFFER rather than only asking the store whether the file is whole is
       what catches it -- and it has to answer SIZE rather than CHECKSUM, because the remedy is a
       different file and not a re-fetch of this one. */
    setup();
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, NULL, 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "an offer for nothing is refused");
    uint8_t frame[64];
    const iotdata_content_manifest_t heard = { .id = ID_T, .size = 448u, .hash = 0x1234u, .sig_id = ID_TSIG, .expiry_m = 600u };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "but we are told about a 448-byte object");
    (void)put_file(ID_T, 960u, 0x40u); /* and then handed a different one, whole, under that id */
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_T) == IOTDATA_NODE_CONTENT_ERR_SIZE, "a whole file of the wrong length is a SIZE answer");

    /* THE RIGHT NUMBER OF THE WRONG BYTES: start again. The one answer that condemns what is held,
       because no amount of fetching more will fix it. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "image", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored");
    CHECK(backend->write(backend->ctx, ID_A, 100u, "xxxx", 4u) == 4, "four bytes are quietly changed under it");
    CHECK(iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_ERR_CHECKSUM, "and the offer condemns the file");
    uint64_t got = 0u;
    uint32_t len = 0u;
    CHECK(iotdata_node_content_checksum(&content, ID_A, &got, &len) == IOTDATA_NODE_CONTENT_OK && len == 448u && got != crc, "which the store cannot tell you on its own: it still has 448 whole bytes");
}

/* AN AUTHORED OFFER OUTLIVES A RESTART, which is the half of §3's provisioning problem that is
   ours: a gateway that loses what an operator told it makes them say it again every reboot, and
   that cost the first bench run its offer within minutes. A HEARD one does not outlive anything --
   its expiry is an offset on a monotonic clock, and the advertiser re-originates. */
static void test_an_authored_offer_survives_a_restart(void) {
    printf("\nwhat a person said is kept beside the bytes; what was merely heard is not\n");
    setup();
    const uint64_t crc = put_file(ID_A, 448u, 0x10u);
    (void)put_file(ID_B, 64u, 0x20u); /* a file nobody describes */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "sensor-esp32c3-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BSDIFF, IOTDATA_CONTENT_COMPRESS_HEATSHRINK), 1440u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK,
          "authored");
    /* and originated twice, so the generation to be preserved is not the initial one */
    uint8_t frame[128];
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 2000u) > 0u, "advertised");
    const uint32_t later_ms = 2000u + (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u + 1000u;
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), later_ms) > 0u, "and again, a floor's worth later");

    /* and one offer we only HEARD, for an id we hold nothing of */
    const iotdata_content_manifest_t heard = { .id = 0x601001u, .size = 96u, .hash = 7u, .sig_id = 0x601002u, .expiry_m = 600u, .name = "somebody else's" };
    const size_t hn = iotdata_content_manifest_pack(frame, sizeof(frame), &heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, hn, 3000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and one heard");
    CHECK(iotdata_node_content_manifests_count(&offers) == 2u, "two offers before the restart");

    /* THE RESTART. The store survives -- it is flash, or a directory -- and the table does not. */
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_count(&offers) == 0u, "the table came back empty");
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 9000u) == 1, "and exactly one offer came back");

    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL, "the authored one");
    if (m != NULL) {
        CHECK(strcmp(m->name, "sensor-esp32c3-0.9.11") == 0, "with the name a person typed");
        CHECK(m->encoding == IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BSDIFF, IOTDATA_CONTENT_COMPRESS_HEATSHRINK), "the encoding they chose");
        CHECK(m->expiry_m == 1440u && m->sig_id == ID_SIG, "the expiry and the signature stream");
        CHECK(m->local, "still ours to advertise");
        /* THE GENERATION EXACTLY, because a table starting from zero would re-use one that relays
           still remember, and a byte-identical advertisement is thrown by every one of them. */
        CHECK(m->generation == 2u, "AND the generation it had reached");
        /* size and hash came from the STORE, not from the sidecar -- which is why they are right */
        CHECK(m->size == 448u && m->hash == crc, "with the size and hash re-read from the bytes");
        /* the expiry runs from the restore, not from whenever it was first stated */
        CHECK(iotdata_node_content_manifest_remaining_m(m, 9000u) == 1440u, "and the offer is stated afresh");
    }
    CHECK(iotdata_node_content_manifest_find(&offers, 0x601001u) == NULL, "the heard one did NOT come back");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_B) == NULL, "nor did the file nobody described");

    /* AND IT KEEPS WORKING: the restored row advertises, with the generation moving on from 2. */
    /* ALLOWED IMMEDIATELY after a restore: the row came back from a sidecar with no origination
       behind it in this lifetime, and a node that has just come up should re-announce. */
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 9000u) > 0u, "the restored offer advertises at once");
    uint8_t gen = 0xFFu;
    CHECK(iotdata_content_header_parse(frame, sizeof(frame), NULL, &gen, NULL) && gen == 3u, "and carries on from where it was");
}

static void test_a_sidecar_never_outlives_its_bytes(void) {
    printf("\na description cannot outlive what it describes, in any of the three ways it could try\n");
    setup();
    const uint64_t crc = put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored");

    /* 1. DELETED. The store takes the sidecar with the content, or the next id to reuse that number
          -- §4's allocation wraps after 4096 -- would inherit somebody else's description. */
    CHECK(iotdata_node_content_delete(&content, ID_A) == IOTDATA_NODE_CONTENT_OK, "the file is deleted");
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 2000u) == 0, "and nothing is restored");
    /* and the number, reused for something else, comes back undescribed */
    (void)put_file(ID_A, 64u, 0x55u);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 2000u) == 0, "a reused id inherits no description");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "none at all");

    /* 2. REVOKED. Same destruction, reached the other way. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored again");
    CHECK(iotdata_node_content_manifest_revoke(&offers, &content, ID_A, 2000u) == IOTDATA_NODE_CONTENT_OK, "and revoked");
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 3000u) == 0, "a revoked offer does not come back");

    /* 3. REPLACED, which is create() over an existing id. The description goes with the old bytes:
          size and hash could be re-derived from the new ones, but the NAME cannot, and calling a
          bundle somebody just dropped there by the old one's name is the mistake worth making
          impossible. Whoever replaces the bytes describes them again. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored");
    const uint64_t other = put_file(ID_A, 960u, 0x66u); /* a different object under the same id */
    CHECK(other != crc, "the bytes changed underneath it");
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 4000u) == 0, "and the old description did not come with them");
    /* described afresh, it comes back -- and now it describes what is actually there */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img-0.9.12", 0u, 600u, ID_SIG, 4000u) == IOTDATA_NODE_CONTENT_OK, "re-authored");
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 5000u) == 1, "the new offer comes back");
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && m->size == 960u && m->hash == other, "describing what is there, re-read from the bytes");
    CHECK(m != NULL && strcmp(m->name, "img-0.9.12") == 0, "by the name somebody actually gave it");
    CHECK(m != NULL && iotdata_node_content_manifest_verify(&offers, &content, ID_A) == IOTDATA_NODE_CONTENT_OK, "and it verifies");

    /* 4. HALF A FILE. author() refuses an incomplete one, so a restore of a truncated file yields
          no offer rather than one advertising a fragment's hash. */
    setup();
    CHECK(backend->create(backend->ctx, ID_A, 448u), "a file declared at 448");
    CHECK(backend->write(backend->ctx, ID_A, 0u, fixture, 224u) == 224, "with half of it");
    uint8_t side[64];
    const iotdata_content_manifest_t forged = { .id = ID_A, .size = 448u, .hash = crc, .sig_id = ID_SIG, .expiry_m = 600u, .name = "img" };
    const size_t fn = iotdata_content_manifest_pack(side, sizeof(side), &forged);
    CHECK(backend->meta_write(backend->ctx, ID_A, side, fn) == (int)fn, "and a sidecar put there by hand");
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 5000u) == 0, "a fragment is not offered");
}

/* SAYING IT AGAIN WITHOUT BEING ASKED. §3 makes this a requirement: an advertisement's reach in
   time is the down path's broadcast slot expiry, not the manifest's own, and an advertisement is
   not requestable -- so a node that joined or rebooted after the frame aged out has no way at all
   to learn about content sitting right there. */
static void test_what_is_due_to_be_said_again(void) {
    printf("\nan advertiser is told which of its own offers to say again, one at a time\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    (void)put_file(ID_B, 64u, 0x20u);
    const uint32_t cadence_ms = (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_S * 1000u;

    /* NEVER ORIGINATED IS DUE AT ONCE, which is what makes an offer restored from a sidecar
       announce itself after a restart with nobody asking -- the whole point of persisting it. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "a", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u) == ID_A, "a brand new offer is due immediately");

    uint8_t frame[128];
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u) > 0u, "so it is said");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u) == 0u, "and is not due again at once");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u + cadence_ms - 1u) == 0u, "nor a millisecond early");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u + cadence_ms) == ID_A, "and is due on the cadence");

    /* ONE AT A TIME, OLDEST FIRST: four offers coming due together would otherwise go back to back,
       which at 2.4kbps is seconds of self-collision. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "b", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "a second offer");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u) == ID_B, "which has never been said, so it goes first");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_B, frame, sizeof(frame), 1000u) > 0u, "said");
    /* now both have been said once; A is the older, so A is due first */
    const uint32_t later = 1000u + cadence_ms + 5000u;
    CHECK(iotdata_node_content_manifest_due(&offers, later) == ID_A, "then the one said longest ago");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), later) > 0u, "said");
    CHECK(iotdata_node_content_manifest_due(&offers, later) == ID_B, "and then the other");

    /* ONLY OUR OWN. §3: only the advertiser originates, so an offer we merely heard is never due --
       re-stating it from a cache would extend its life indefinitely. */
    setup();
    const iotdata_content_manifest_t heard = { .id = 0x601001u, .size = 64u, .hash = 3u, .sig_id = 0x601002u, .expiry_m = 600u, .name = "theirs" };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &heard);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an offer we heard");
    CHECK(iotdata_node_content_manifest_due(&offers, 1000u + cadence_ms * 10u) == 0u, "is never due, however long it sits");
}

/* THE SAFETY CAPS. §8 is explicit that the protocol has no authenticity -- "anyone can advertise a
   content-id and serve garbage" -- so every one of these is about what an INCORRECT advertiser
   cannot be allowed to do to this node, and not one of them is needed to make a correct one work. */
static void test_the_caps_bound_an_errant_advertiser(void) {
    printf("\nthe three caps: how often we originate, how long we honour, and whose offers fill the table\n");

    /* 1. THE FLOOR UNDER RE-ORIGINATION. Four bits of generation exhaust in sixteen, and the
          seventeenth reuses a value a relay may still hold -- after which the offer is deduped
          away and stops arriving, which is the one thing it exists to do. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored");
    uint8_t frame[128];
    /* the FIRST origination is always allowed: an operator who describes an offer and advertises
       it in the same breath must not be told to wait */
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u) > 0u, "the first origination goes at once");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1001u) == 0u, "a second a millisecond later does not");
    CHECK(offers.stat_tooSoon == 1u, "and is counted, so a runaway is visible rather than silent");
    const uint32_t floor_ms = (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u;
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u + floor_ms - 1u) == 0u, "nor one a millisecond short of the floor");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u + floor_ms) > 0u, "and exactly a floor later does");
    /* AND RE-DESCRIBING IT IS NOT A FRESH LICENCE, or a loop that re-authored between
       originations would walk straight past the floor. */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img-again", 0u, 600u, ID_SIG, 1000u + floor_ms) == IOTDATA_NODE_CONTENT_OK, "re-authored");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u + floor_ms + 1u) == 0u, "re-authoring does not reset the floor");

    /* 2. THE LONGEST EXPIRY WE WILL HONOUR, on what arrives and never on what we author. */
    setup();
    (void)put_file(ID_A, 64u, 0x20u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ours", 0u, IOTDATA_CONTENT_EXPIRY_INDEFINITE, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "our own indefinite offer");
    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && m->expiry_m == IOTDATA_CONTENT_EXPIRY_INDEFINITE, "stands indefinitely: we are not our own attacker");
    CHECK(offers.stat_capped == 0u, "and nothing was capped");

    const iotdata_content_manifest_t forever = { .id = 0x601001u, .size = 64u, .hash = 3u, .sig_id = 0x601002u, .expiry_m = IOTDATA_CONTENT_EXPIRY_INDEFINITE, .name = "somebody else's forever" };
    size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &forever);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and theirs is taken");
    m = iotdata_node_content_manifest_find(&offers, 0x601001u);
    CHECK(m != NULL && m->expiry_m == (uint16_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M, "but clamped to what we will honour");
    CHECK(offers.stat_capped == 1u, "counted");
    /* SO IT AGES OUT, which an indefinite entry never would -- and an advertiser that is still
       alive re-states it long before this, which is what §3 requires of it anyway. */
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + (uint32_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M * 60u * 1000u) >= 1, "and an advertiser that went quiet loses its row");
    CHECK(iotdata_node_content_manifest_find(&offers, 0x601001u) == NULL, "which is the whole point of the cap");

    /* an over-long but finite expiry is clamped the same way */
    setup();
    const iotdata_content_manifest_t toolong = { .id = 0x601003u, .size = 64u, .hash = 4u, .sig_id = 0x601002u, .expiry_m = (uint16_t)(IOTDATA_NODE_CONTENT_EXPIRY_MAX_M + 100u) };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &toolong);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an over-long expiry is taken");
    m = iotdata_node_content_manifest_find(&offers, 0x601003u);
    CHECK(m != NULL && m->expiry_m == (uint16_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M, "and clamped, not refused");
    /* and one inside the cap is left exactly alone */
    const iotdata_content_manifest_t fine = { .id = 0x601004u, .size = 64u, .hash = 5u, .sig_id = 0x601002u, .expiry_m = 60u };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &fine);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a reasonable one is taken");
    m = iotdata_node_content_manifest_find(&offers, 0x601004u);
    CHECK(m != NULL && m->expiry_m == 60u, "untouched");

    /* 3. ONE ORIGINATOR'S SHARE OF THE TABLE. §4 puts the station in the top twelve bits, so this
          is free to work out -- and without it one broken node mints ids until nothing else fits. */
    setup();
    uint32_t taken = 0u;
    for (uint32_t i = 0; i < (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX + 2u; i++) {
        const iotdata_content_manifest_t w = { .id = 0x601000u + i, .size = 64u, .hash = i, .sig_id = 0x601002u, .expiry_m = 600u };
        n = iotdata_content_manifest_pack(frame, sizeof(frame), &w);
        if (iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK)
            taken++;
    }
    CHECK(taken == (uint32_t)IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX, "one originator gets its share and no more");
    CHECK(offers.stat_crowded > 0u, "and the refusals are counted");
/* THE REST OF THE SHARE'S PROPERTIES ONLY EXIST WHEN IT IS A SHARE. The tight build sets the
   table to two rows, where the floor of two makes the cap equal to the whole table -- there is no
   "somebody else" left to make room for, and nothing to author past. */
#if IOTDATA_NODE_CONTENT_MANIFESTS_MAX > IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX
    /* AND SOMEBODY ELSE CAN STILL GET IN, which is the whole purpose: the table was not filled. */
    const iotdata_content_manifest_t other = { .id = 0x003003u, .size = 64u, .hash = 9u, .sig_id = 0x003004u, .expiry_m = 600u };
    n = iotdata_content_manifest_pack(frame, sizeof(frame), &other);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a third station's offer still fits");
    /* OUR OWN ROWS ARE EXEMPT from the share, because the table is ours and a node is not its own
       attacker -- so a gateway may describe more of its own content than it would let a neighbour
       describe of theirs. */
    setup();
    for (uint32_t i = 1; i <= (uint32_t)IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX + 1u; i++) {
        (void)put_file(0x001000u + i, 64u, (uint8_t)i);
        CHECK(iotdata_node_content_manifest_author(&offers, &content, 0x001000u + i, "ours", 0u, 600u, 0x001002u, 1000u) == IOTDATA_NODE_CONTENT_OK, "we may author past the share");
    }
#endif
}

/* The 13-byte STATUS row is how a manager sees any of this over the air, and it is the whole of the
   observability for the bring-up: an advertisement that landed shows up as an offer with nothing
   held, which is exactly the state that proves the frame arrived. */
static void test_the_status_table_spans_both_sources(void) {
    printf("\nthe status table is the union of what is held and what is offered, in id order\n");
    setup();

    /* one file with no offer, one offer with no file, one of each */
    (void)put_file(0x001005u, 448u, 0x10u);
    (void)put_file(0x001002u, 64u, 0x20u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, 0x001002u, "both", 0u, 90u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "authored over one of them");
    uint8_t frame[64];
    const iotdata_content_manifest_t w = { .id = 0x601009u, .size = 448u, .hash = 9u, .sig_id = 0u, .expiry_m = IOTDATA_CONTENT_EXPIRY_INDEFINITE };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &w);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and heard about one we hold nothing of");

    CHECK(iotdata_node_content_table_count(&content, &offers) == 3u, "three ids between the two sources");

    uint8_t row[IOTDATA_NODE_STATUS_CONTENT_ENTRY_SIZE];
    /* IN ID ORDER, so an index means the same thing twice running: 001002, 001005, 601009 -- two
       of ours and one of theirs, which is what "both sources" means here. */
    CHECK(iotdata_node_content_table_row(&content, &offers, 0, row, 2000u), "row 0");
    CHECK(iotdata_node_entry_get_u24(row, 0) == 0x001002u, "the lowest id first");
    CHECK(row[3] == IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING, "complete, so it can be served");
    CHECK((row[4] & IOTDATA_NODE_STATUS_CONTENT_FLAG_PERSISTENT) != 0u, "and this store was declared persistent");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 2u, "64 bytes is two chunks held");
    CHECK(iotdata_node_entry_get_u16(row, 7) == 2u, "and the offer says two");
    CHECK(iotdata_node_entry_get_u16(row, 9) == 90u, "with its 90 minutes still showing a second in");

    CHECK(iotdata_node_content_table_row(&content, &offers, 1, row, 2000u), "row 1");
    CHECK(iotdata_node_entry_get_u24(row, 0) == 0x001005u, "then the next");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 14u, "448 bytes is 14 chunks, the last one short");
    CHECK(iotdata_node_entry_get_u16(row, 7) == 0u, "NO MANIFEST SEEN, which is what 0 means here");
    CHECK(iotdata_node_entry_get_u16(row, 9) == 0u, "and no expiry to report");

    CHECK(iotdata_node_content_table_row(&content, &offers, 2, row, 2000u), "row 2");
    CHECK(iotdata_node_entry_get_u24(row, 0) == 0x601009u, "and the offer we hold nothing of");
    CHECK(iotdata_node_entry_get_u16(row, 5) == 0u, "nothing held");
    CHECK(iotdata_node_entry_get_u16(row, 7) == 14u, "but we know how big it should be");
    /* CLAMPED, because we only HEARD this one: §3 lets an advertiser say indefinite and this node
       will honour it for as long as it is willing to honour one, after which the advertiser keeps
       it alive by saying so again. See THE SAFETY CAPS. */
    CHECK(iotdata_node_entry_get_u16(row, 9) == IOTDATA_NODE_CONTENT_EXPIRY_MAX_M, "and that the offer stands, for as long as we will honour one");
    CHECK(iotdata_node_entry_get_u16(row, 11) == 1u, "a second since we were told, which is what shows a stall");

    CHECK(!iotdata_node_content_table_row(&content, &offers, 3, row, 2000u), "and there is no fourth row");

    /* A SCRATCH FILE IS NOBODY'S BUSINESS: a half-built upload is not content and does not appear. */
    CHECK(backend->create(backend->ctx, 0x000001u, 32u), "a scratch file exists");
    CHECK(iotdata_node_content_table_count(&content, &offers) == 3u, "and is not in the table");
}

/* The words, so that four applications report an offer the same way. */
/* ============================================================================================
   WHAT THE AUDIT OF 2026-11-09 ADDED. The suite above walks each rule on its own, one offer at a
   time, from a clean table -- which is how the two bugs below survived it. These exercise the
   dimensions a single-offer test cannot reach: several lifecycles running at once, a restart with
   a tableful rather than one row, the bytes moving under an offer, and a clock that has been
   running for weeks.
   ============================================================================================ */

/* A WITHDRAWN OFFER HAS TO STAY WITHDRAWN. Found by asking what the sidecar does when a LOCAL
   offer expires, and the answer was: nothing, so the next boot restored the offer that had just
   lapsed. The same offer lived for ten minutes or for ever depending on whether anybody
   power-cycled the node, which is not a lifetime at all. */
static void test_a_withdrawn_offer_stays_withdrawn(void) {
    printf("\nour own offer expiring is a withdrawal, and a withdrawal survives a restart\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ten-minutes", 0u, 10u, ID_SIG, 2000u) == IOTDATA_NODE_CONTENT_OK, "offered for ten minutes");
    uint8_t side[D_FILESTORE_META_MAX];
    CHECK(backend->meta_read(backend->ctx, ID_A, side, sizeof(side)) > 0, "and the sidecar describes it");

    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 2000u + 10u * 60u * 1000u) == 1, "ten minutes later it lapses");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "the row is gone");
    CHECK(backend->meta_read(backend->ctx, ID_A, side, sizeof(side)) <= 0, "AND SO IS THE SIDECAR: this is what the restart used to undo");
    CHECK(holds(ID_A), "the bytes stay -- expiring is not revoking");

    /* THE PROOF IS THE RESTART, because that is where it went wrong. */
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    CHECK(iotdata_node_content_manifests_restore(&offers, &content, 50000u) == 0, "and a reboot does not resurrect it");
    CHECK(iotdata_node_content_manifests_count(&offers) == 0u, "nothing is on offer");

    /* A HEARD offer never had a sidecar to clear -- only the advertiser writes one -- so expiry
       there is the ordinary case and the bytes are equally untouched. */
    setup();
    (void)put_file(ID_B, 64u, 0x20u);
    uint8_t frame[128];
    const iotdata_content_manifest_t theirs = { .id = ID_T, .size = 64u, .hash = put_file(ID_T, 64u, 0x20u), .sig_id = ID_TSIG, .expiry_m = 5u, .name = "theirs" };
    const size_t tn = iotdata_content_manifest_pack(frame, sizeof(frame), &theirs);
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, tn, 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "an offer we merely heard");
    CHECK(backend->meta_read(backend->ctx, ID_B, side, sizeof(side)) <= 0, "writes no sidecar: we are not its advertiser");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 1000u + 5u * 60u * 1000u) == 1, "and lapses on its own clock");
    CHECK(holds(ID_B), "with the bytes kept");
}

/* THE BYTES CAN BE REPLACED UNDER AN OFFER, and until this was fixed the node went on advertising
   the OLD size and hash -- so every requester fetched, checked the advertised hash against what
   arrived, and threw the transfer away. A radio-shaped failure that is not a radio problem. The
   bench sequence that does it is ordinary: insert a new blob over an id that is already offered. */
static void test_an_offer_whose_bytes_changed_is_dropped(void) {
    printf("\nan offer is dropped when the bytes under it are replaced, not advertised stale\n");
    uint8_t frame[128];

    /* DIFFERENT BYTES, SAME LENGTH -- the case a size check alone would miss. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img-0.9.11", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "offered");
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 1000u) > 0u, "and advertised while it is true");
    (void)put_file(ID_A, 448u, 0x99u); /* somebody drops a different build under the same id */
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 900000u) == 0u, "once the bytes differ it will not go out");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "the offer is dropped, not quietly corrected: the NAME cannot be re-derived");
    CHECK(offers.stat_stale == 1u, "and counted, so it is visible rather than silent");
    CHECK(holds(ID_A), "the new bytes are untouched -- we stopped describing them, not holding them");
    uint8_t side[D_FILESTORE_META_MAX];
    CHECK(backend->meta_read(backend->ctx, ID_A, side, sizeof(side)) <= 0, "and the stale description does not survive a restart either");

    /* A DIFFERENT LENGTH, and the bytes going away entirely. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "offered");
    (void)put_file(ID_A, 96u, 0x10u);
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 900000u) == 0u, "a shorter object is a different object");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "dropped");

    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "img", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "offered");
    iotdata_node_content_delete(&content, ID_A);
    CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 900000u) == 0u, "and content we no longer hold is never offered");
    CHECK(iotdata_node_content_manifest_find(&offers, ID_A) == NULL, "dropped");

    /* AND AUTHORING REFUSES CONTENT THAT IS STILL ARRIVING, which is the same invariant on the way
       in: a file created but not filled sums to something, and offering that would advertise a
       hash nobody can ever match. */
    setup();
    CHECK(backend->create(backend->ctx, ID_B, 448u), "a file is created for a transfer");
    CHECK(backend->write(backend->ctx, ID_B, 0u, fixture, 64u) == 64, "and partly filled");
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "half", 0u, 600u, ID_SIG, 1000u) != IOTDATA_NODE_CONTENT_OK, "it cannot be offered yet");
    CHECK(iotdata_node_content_manifests_count(&offers) == 0u, "and leaves no row behind");
}

/* SEVERAL LIFECYCLES AT ONCE, which is the shape of any real node: a blob of our own, two offers
   heard from different places, and an image being replaced, all in the table together. Every rule
   above was checked one offer at a time from a clean table -- so nothing checked that an operation
   lands on the row it names and leaves its neighbours exactly as they were. That is the wrong-slot
   bug class, and the table is keyed by id with eight slots, so it is a live possibility. */
/* NEEDS ROOM FOR SEVERAL AT ONCE, which is the whole subject -- so it is compiled out of the
   tight build, whose job is the opposite: proving the table behaves when it has barely any. */
#if IOTDATA_NODE_CONTENT_MANIFESTS_MAX >= 4
static void test_one_lifecycle_does_not_disturb_another(void) {
    printf("\nfour offers at different stages: an operation on one leaves the others untouched\n");
    setup();
    const uint32_t ID_P1 = 0x601001u, ID_P2 = 0x602001u; /* two other stations' ids */
    uint8_t frame[128];

    (void)put_file(ID_A, 448u, 0x10u);
    (void)put_file(ID_B, 96u, 0x20u);
    const uint64_t h1 = put_file(ID_P1, 64u, 0x30u), h2 = put_file(ID_P2, 128u, 0x40u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "ours-a", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "one of ours");
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_B, "ours-b", 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "and a second");
    const iotdata_content_manifest_t m1 = { .id = ID_P1, .size = 64u, .hash = h1, .sig_id = ID_SIG, .expiry_m = 30u, .name = "theirs-1" };
    const iotdata_content_manifest_t m2 = { .id = ID_P2, .size = 128u, .hash = h2, .sig_id = ID_SIG, .expiry_m = 600u, .name = "theirs-2" };
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &m1), 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "one heard");
    CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &m2), 1000u, NULL) == IOTDATA_NODE_CONTENT_OK, "and another, from elsewhere");
    CHECK(iotdata_node_content_manifests_count(&offers) == 4u, "four offers, four lifecycles");

/* COPIES A ROW, AND SAYS SO IF THERE IS NOT ONE. The first version dereferenced whatever find()
   returned, so in the tight build -- two slots, four offers -- it segfaulted after the suite had
   printed "all ok", which is a confusing way to learn about a dimension. */
#define SNAP(v, id) \
    iotdata_node_content_manifest_t v = { 0 }; \
    do { \
        const iotdata_node_content_manifest_t *const _r = iotdata_node_content_manifest_find(&offers, id); \
        CHECK(_r != NULL, "the row this block is about exists"); \
        if (_r != NULL) \
            v = *_r; \
    } while (0)
#define SAME(v, id, msg) CHECK(iotdata_node_content_manifest_find(&offers, id) != NULL && memcmp(&(v), iotdata_node_content_manifest_find(&offers, id), sizeof(v)) == 0, msg)

    /* REVOKING ONE OF THEIRS touches that row and that file, and nothing else. */
    {
        SNAP(a, ID_A);
        SNAP(b, ID_B);
        SNAP(t2, ID_P2);
        const iotdata_content_manifest_t rev = { .id = ID_P1, .size = 0u, .expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_REVOKED };
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &rev), 2000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a revocation arrives for one of them");
        CHECK(iotdata_node_content_manifest_find(&offers, ID_P1) == NULL && !holds(ID_P1), "it goes, bytes and all");
        SAME(a, ID_A, "our first offer is byte-for-byte as it was");
        SAME(b, ID_B, "and our second");
        SAME(t2, ID_P2, "and the other station's");
        CHECK(holds(ID_A) && holds(ID_B) && holds(ID_P2), "and no other file was destroyed");
    }

    /* EXPIRING ONE drops exactly one row. The remaining heard offer has 600 minutes on it and ours
       have 600 too, so a tick that takes one must take only one. */
    {
        SNAP(a, ID_A);
        SNAP(b, ID_B);
        const iotdata_content_manifest_t soon = { .id = ID_P2, .size = 128u, .hash = h2, .sig_id = ID_SIG, .expiry_m = 1u, .name = "theirs-2" };
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &soon), 3000u, NULL) == IOTDATA_NODE_CONTENT_OK, "re-stated with a minute left");
        CHECK(iotdata_node_content_manifests_tick(&offers, &content, 3000u + 60u * 1000u) == 1, "exactly one row expires");
        CHECK(iotdata_node_content_manifest_find(&offers, ID_P2) == NULL, "that one");
        SAME(a, ID_A, "ours is untouched by somebody else's clock");
        SAME(b, ID_B, "and so is the other");
    }

    /* ADVERTISING ONE bumps that row's generation and stamps that row's clock. The neighbour's
       generation is the thing a shared cursor would corrupt, and a relay throws a byte-identical
       advertisement away, so a bump landing on the wrong row loses an offer silently. */
    {
        SNAP(b, ID_B);
        const uint8_t gen_a = iotdata_node_content_manifest_find(&offers, ID_A)->generation;
        CHECK(iotdata_node_content_manifest_advertise(&offers, &content, ID_A, frame, sizeof(frame), 900000u) > 0u, "one of ours goes out");
        CHECK(iotdata_node_content_manifest_find(&offers, ID_A)->generation == (uint8_t)((gen_a + 1u) & IOTDATA_CONTENT_TAG_MAX), "its generation advanced");
        SAME(b, ID_B, "and the other kept its generation, which is what a relay dedups on");
    }

    /* SUPERSEDING ONE destroys that object's bytes and leaves every other file alone -- the rule
       that makes id reuse safe, checked here against a table that has other files in it.
     *
     * IT HAS TO BE ONE OF THEIRS. A heard manifest for an id WE advertise is refused outright,
       because the only thing it can be is our own advertisement coming back off a relay -- which
       is a rule this block confirmed by failing when it was first written against ID_B. */
    {
        SNAP(a, ID_A);
        SNAP(b, ID_B);
        const uint32_t ID_P3 = 0x603001u;
        const uint64_t h3 = put_file(ID_P3, 64u, 0x50u);
        const iotdata_content_manifest_t first = { .id = ID_P3, .size = 64u, .hash = h3, .sig_id = ID_SIG, .expiry_m = 600u, .name = "theirs-3" };
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &first), 4000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a third station's offer, and we hold its bytes");
        const iotdata_content_manifest_t fresh = { .id = ID_P3, .size = 64u, .hash = h3 ^ 0xFFu, .sig_id = ID_SIG, .expiry_m = 600u, .name = "theirs-3b" };
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &fresh), 5000u, NULL) == IOTDATA_NODE_CONTENT_OK, "a different object under that used id");
        CHECK(!holds(ID_P3), "the bytes that were there are destroyed");
        SAME(a, ID_A, "and our own offers are exactly as they were");
        SAME(b, ID_B, "both of them");
        CHECK(holds(ID_A) && holds(ID_B), "with their bytes still here");

        /* and the echo rule itself, stated once where it is easy to find */
        const iotdata_content_manifest_t echo = { .id = ID_B, .size = 96u, .hash = 1u, .sig_id = ID_SIG, .expiry_m = 600u, .name = "not-ours" };
        CHECK(iotdata_node_content_manifest_on_wire(&offers, &content, frame, iotdata_content_manifest_pack(frame, sizeof(frame), &echo), 6000u, NULL) != IOTDATA_NODE_CONTENT_OK, "nobody else may re-describe an offer of ours");
        SAME(b, ID_B, "so it is untouched");
        CHECK(holds(ID_B), "and its bytes are never at risk from a frame off the air");
    }
#undef SNAP
#undef SAME
}
#endif

/* A RESTART WITH A TABLEFUL, not the one offer the restore test used. The walk reads a sidecar per
   file, so everything that can go wrong needs more than one file to go wrong with: a description
   that will not parse sitting among good ones, a file with no description at all, and more
   described files than the table can hold. */
/* NEEDS ROOM FOR SEVERAL AT ONCE, which is the whole subject -- so it is compiled out of the
   tight build, whose job is the opposite: proving the table behaves when it has barely any. */
#if IOTDATA_NODE_CONTENT_MANIFESTS_MAX >= 4
static void test_a_restart_with_a_tableful(void) {
    printf("\na restart restores a tableful, skips what it cannot read, and does not overflow\n");
    setup();
    const uint32_t n_good = 3u;
    for (uint32_t i = 0; i < n_good; i++) {
        const uint32_t id = 0x001010u + i;
        (void)put_file(id, 64u + i * 32u, (uint8_t)(0x10u + i));
        char name[16];
        snprintf(name, sizeof(name), "blob-%u", (unsigned)i);
        CHECK(iotdata_node_content_manifest_author(&offers, &content, id, name, 0u, 600u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK, "described");
    }
    /* a file nobody described: legitimate, and must not become a phantom offer */
    (void)put_file(0x001020u, 32u, 0x77u);
    /* a description that will not parse, which a partly-written sidecar would look like */
    (void)put_file(0x001021u, 32u, 0x78u);
    const uint8_t rubbish[3] = { 0xFF, 0xFF, 0xFF };
    CHECK(backend->meta_write(backend->ctx, 0x001021u, rubbish, sizeof(rubbish)) > 0, "and one unreadable description");

    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    const int back = iotdata_node_content_manifests_restore(&offers, &content, 50000u);
    CHECK(back == (int)n_good, "every good description comes back, and only those");
    CHECK((uint32_t)iotdata_node_content_manifests_count(&offers) == n_good, "one row each");
    for (uint32_t i = 0; i < n_good; i++) {
        const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(&offers, 0x001010u + i);
        CHECK(m != NULL && m->local && m->size == 64u + i * 32u, "with its own size, re-read from its own bytes");
    }
    CHECK(iotdata_node_content_manifest_find(&offers, 0x001020u) == NULL, "an undescribed file is not an offer");
    CHECK(iotdata_node_content_manifest_find(&offers, 0x001021u) == NULL, "and an unreadable one is skipped, not fatal");
    CHECK(holds(0x001021u), "its bytes are left alone: we could not read the description, which says nothing about them");

    /* MORE DESCRIBED FILES THAN THE TABLE HOLDS. A node can be handed a store from a bigger one,
       or be rebuilt smaller; either way the walk must stop at the edge and say so. */
    setup();
    const uint32_t want = (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX + 3u;
    uint32_t over = 0u;
    for (uint32_t i = 0; i < want; i++) {
        const uint32_t id = 0x001030u + i;
        const uint64_t h = put_file(id, 64u, (uint8_t)i);
        if (!holds(id))
            break; /* the backing store ran out of room first, which is not what is under test */
        over++;
        /* STRAIGHT INTO THE STORE, because author() refuses once the table is full and so can
           never leave more descriptions behind than the table holds -- the first version of this
           test proved only that. A sidecar IS a MANIFEST TLV, which is what makes this the same
           bytes the authoring path would have written. */
        const iotdata_content_manifest_t w = { .id = id, .size = 64u, .hash = h, .sig_id = ID_SIG, .expiry_m = 600u, .name = "x" };
        uint8_t side[D_FILESTORE_META_MAX];
        const size_t sn = iotdata_content_manifest_pack(side, sizeof(side), &w);
        CHECK(backend->meta_write(backend->ctx, id, side, sn) > 0, "described in the store");
    }
    iotdata_node_content_manifests_init(&offers, true, MY_STATION);
    const uint32_t expect = (over < (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX) ? over : (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX;
    const int capped = iotdata_node_content_manifests_restore(&offers, &content, 50000u);
    CHECK(capped == (int)expect, "it restores what it can hold and stops there");
    CHECK((uint32_t)iotdata_node_content_manifests_count(&offers) == expect, "exactly full, never over");
    if (over > (uint32_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX)
        CHECK(offers.stat_full > 0u, "and says it ran out of room");
}
#endif

/* A CLOCK THAT HAS BEEN RUNNING FOR WEEKS. hw_time_ms() is 32 bits of milliseconds, so it comes
   round every 49.7 days and a relay is expected to run for months. Everything here does its
   arithmetic as a difference, which is correct across the wrap -- this pins it, because the
   failure mode is an offer that either never expires or expires the instant it is made. */
static void test_a_clock_that_runs_for_months(void) {
    printf("\nages are differences, so they survive the 49.7-day wrap and a little skew\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    const uint32_t near_wrap = 0xFFFFF000u; /* about four seconds before it comes round */
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "wrapped", 0u, 600u, ID_SIG, near_wrap) == IOTDATA_NODE_CONTENT_OK, "described just before the wrap");
    const iotdata_node_content_manifest_t *m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && iotdata_node_content_manifest_remaining_m(m, near_wrap) == 600u, "all of its life ahead of it");
    /* ten minutes later, which is on the other side of zero */
    const uint32_t after = near_wrap + 10u * 60u * 1000u;
    CHECK(after < near_wrap, "the clock really did come round");
    CHECK(iotdata_node_content_manifest_remaining_m(m, after) == 590u, "and ten minutes of it have gone, not forty-nine days");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, after) == 0, "so nothing expires at the wrap");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, near_wrap + 600u * 60u * 1000u) == 1, "and it still expires when it should");

    /* A STAMP IN THE FUTURE, which is what a node sees when something re-stamps a row from a clock
       read a moment later than the pass's. Unsigned, the difference would be enormous and a
       24-hour offer would lapse on the spot. */
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "skewed", 0u, 1440u, ID_SIG, 100000u) == IOTDATA_NODE_CONTENT_OK, "described");
    m = iotdata_node_content_manifest_find(&offers, ID_A);
    CHECK(m != NULL && iotdata_node_content_manifest_remaining_m(m, 99000u) == 1440u, "a tick from a moment earlier does not age it");
    CHECK(iotdata_node_content_manifests_tick(&offers, &content, 99000u) == 0, "and certainly does not expire it");
    CHECK(iotdata_node_content_manifest_due(&offers, 99000u) == ID_A, "a brand new offer is still due to be said");
}

static void test_the_wording(void) {
    printf("\none formatter, so four applications cannot each invent their own listing\n");
    setup();
    (void)put_file(ID_A, 448u, 0x10u);
    CHECK(iotdata_node_content_manifest_author(&offers, &content, ID_A, "sensor-esp32c3-0.9.11", IOTDATA_CONTENT_ENCODING(IOTDATA_CONTENT_TYPE_BINARY, IOTDATA_CONTENT_COMPRESS_NONE), 1440u, ID_SIG, 1000u) == IOTDATA_NODE_CONTENT_OK,
          "authored");

    char buf[IOTDATA_NODE_CONTENT_MANIFEST_STR_MAX + 1];
    const char *const s = iotdata_node_content_manifest_str(buf, sizeof(buf), iotdata_node_content_manifest_find(&offers, ID_A), 1000u);
    printf("  %s\n", s);
    CHECK(strstr(s, "001003") != NULL, "the id, in the six hex digits the store names files by");
    CHECK(strstr(s, "448 bytes") != NULL, "the size");
    CHECK(strstr(s, "14 chunks") != NULL, "what that is in chunks, which is what a transfer counts");
    CHECK(strstr(s, "1440m") != NULL, "what is left of the offer");
    CHECK(strstr(s, "ours") != NULL, "whether we may advertise it");
    CHECK(strstr(s, "sensor-esp32c3-0.9.11") != NULL, "and the name");

    const char *const none = iotdata_node_content_manifest_str(buf, sizeof(buf), NULL, 1000u);
    CHECK(strcmp(none, "no offer") == 0, "and no offer says so rather than printing a zeroed row");

    uint8_t frame[64];
    const iotdata_content_manifest_t w = { .id = ID_T, .size = 64u, .hash = 1u, .sig_id = 0u, .expiry_m = IOTDATA_CONTENT_EXPIRY_INDEFINITE };
    const size_t n = iotdata_content_manifest_pack(frame, sizeof(frame), &w);
    (void)iotdata_node_content_manifest_on_wire(&offers, &content, frame, n, 1000u, NULL);
    const char *const h = iotdata_node_content_manifest_str(buf, sizeof(buf), iotdata_node_content_manifest_find(&offers, ID_T), 1000u);
    printf("  %s\n", h);
    /* AN INDEFINITE OFFER WE HEARD reads as the cap we clamped it to, in minutes -- and that is
       the honest number, because it is what this node will actually do. Our OWN indefinite offer
       is not clamped and does print "indefinite": tested above. */
    CHECK(strstr(h, "indefinite") == NULL, "a heard indefinite offer is not reported as indefinite");
    char want[16];
    (void)snprintf(want, sizeof(want), "%um", (unsigned)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M);
    CHECK(strstr(h, want) != NULL, "but as the cap it was held to");
    CHECK(strstr(h, "heard") != NULL, "and one we merely heard says so");
}

int main(void) {
    printf("test_node_content_manifest\n");
    test_authoring_reads_the_store();
    test_the_wire_and_the_console_are_one_path();
    test_supersede_destroys_the_bytes();
    test_only_our_own_station_may_name_our_ids();
    test_revocation_destroys_and_expiry_forgets();
    test_only_the_advertiser_originates();
    test_the_table_is_bounded_and_says_so();
    test_verify_against_the_offer();
    test_an_authored_offer_survives_a_restart();
    test_a_sidecar_never_outlives_its_bytes();
    test_what_is_due_to_be_said_again();
    test_the_caps_bound_an_errant_advertiser();
    test_the_status_table_spans_both_sources();
    test_a_withdrawn_offer_stays_withdrawn();
    test_an_offer_whose_bytes_changed_is_dropped();
#if IOTDATA_NODE_CONTENT_MANIFESTS_MAX >= 4
    test_one_lifecycle_does_not_disturb_another();
#endif
#if IOTDATA_NODE_CONTENT_MANIFESTS_MAX >= 4
    test_a_restart_with_a_tableful();
#endif
    test_a_clock_that_runs_for_months();
    test_the_wording();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
