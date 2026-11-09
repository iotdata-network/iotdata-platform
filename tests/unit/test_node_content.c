//
// test_node_content.c - the content store: the seam, the claim loop, and the console insert.
//
// There is no radio in any of this, which is the point: NOTES_CONTENT §9 says to build the RAM
// backend first precisely so everything above the seam can be driven from a host. What is under
// test is the half the wire path will REUSE -- one announce entry point, ownership derived at boot
// and persisted nowhere, scratch ids that die at startup, and an insert that only becomes a real
// file once its checksum agrees.
//
// The consumers here are scripted fakes. A real one (the OTA module) would read a header and decide;
// these just return whatever the test told them to, which is what makes the module's reaction to
// each of the CONTENT_R combinations separately observable.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* CRC-64 LIVES IN d_common.h, which needs a platform under it for its timer and delay helpers --
   so the host platform goes in first, exactly as the gateway does it. This suite calls neither;
   it is here for the checksum iotdata_node_content.h sums an upload with. */
#include "d_platform_linux.h"
#include "d_common.h"
#include "d_module_filestore.h" /* the seam: content.h is written against it */
#include "iotdata_node_content.h"
#include "d_module_filestore_ram.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

#define R_MINE   IOTDATA_NODE_CONTENT_R_MINE
#define R_DELETE IOTDATA_NODE_CONTENT_R_DELETE
#define R_WORK   IOTDATA_NODE_CONTENT_R_WORK
#define R_REBOOT IOTDATA_NODE_CONTENT_R_REBOOT

/* ------------------------------------------------------------------------------------------- */

typedef struct {
    const char *tag;
    iotdata_node_content_result_t on_claim;
    iotdata_node_content_result_t work_script[8];
    int work_n, work_i;
    int claims, works, describes;
    uint32_t last_id, last_size;
    bool describe_yes;
    bool claim_scratch; /* an "assembler": will take a scratch id back at boot */
} fake_t;

static iotdata_node_content_result_t fake_claim(void *ctx, uint32_t id, uint32_t size) {
    fake_t *f = (fake_t *)ctx;
    f->claims++;
    f->last_id = id;
    f->last_size = size;
    if (iotdata_node_content_id_is_scratch(id) && !f->claim_scratch)
        return IOTDATA_NODE_CONTENT_R_NONE;
    return f->on_claim;
}
static iotdata_node_content_result_t fake_work(void *ctx, uint32_t id) {
    fake_t *f = (fake_t *)ctx;
    (void)id;
    f->works++;
    if (f->work_i < f->work_n)
        return f->work_script[f->work_i++];
    return R_MINE; /* nothing more to do, keep it */
}
static int fake_describe(void *ctx, uint32_t id, char *out, size_t size) {
    fake_t *f = (fake_t *)ctx;
    f->describes++;
    if (!f->describe_yes)
        return 0;
    return snprintf(out, size, "%s says 0x%06X", f->tag, (unsigned)id);
}

static iotdata_node_content_consumer_t consumer_of(fake_t *f) {
    return (iotdata_node_content_consumer_t){ .name = f->tag, .ctx = f, .claim = fake_claim, .work = fake_work, .describe = fake_describe };
}

/* 300 bytes of something that is not all the same byte, so a misplaced run is visible */
static uint32_t fill(uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i * 31u + 7u);
    return len;
}

/* Push a whole payload through the insert, in runs of `batch`, and return the module's event. */
static iotdata_node_content_event_t upload(iotdata_node_content_t *c, uint32_t id, const uint8_t *data, uint32_t len, uint32_t batch, uint64_t crc, uint32_t *now_ms) {
    if (iotdata_node_content_insert_begin(c, id, len, crc, *now_ms) != IOTDATA_NODE_CONTENT_OK)
        return IOTDATA_NODE_CONTENT_EVENT_NONE;
    for (uint32_t off = 0; off < len; off += batch) {
        const uint32_t n = ((len - off) < batch) ? (len - off) : batch;
        (void)iotdata_node_content_insert_write(c, off, &data[off], n, *now_ms);
        *now_ms += 10u;
    }
    return iotdata_node_content_tick(c, *now_ms);
}

/* ------------------------------------------------------------------------------------------- */

static void test_ids(void) {
    printf("\nthe id space, as §4 made it: 0x000 and 0xFFF are malformed, so both are ours\n");
    CHECK(iotdata_node_content_id_is_scratch(0x000001u), "0x000001 is scratch");
    CHECK(iotdata_node_content_id_is_scratch(0x000FFFu), "and so is 0x000FFF");
    CHECK(!iotdata_node_content_id_is_scratch(0x001000u), "0x001000 is not");
    CHECK(!iotdata_node_content_id_is_valid(0x000001u), "a scratch id is never valid on the wire");
    CHECK(!iotdata_node_content_id_is_valid(0xFFF001u), "nor is one in the broadcast station");
    CHECK(!iotdata_node_content_id_is_valid(0x1000000u), "nor one that does not fit u24");
    CHECK(iotdata_node_content_id_is_valid(0x5BF001u), "(station << 12) | counter is");
    CHECK(iotdata_node_content_id_is_valid(0x001000u), "and station 0x001 is a real station");
}

/* ------------------------------------------------------------------------------------------- */

static void test_backend(void) {
    printf("\nthe backend on its own: byte ranges, and knowing which of them it has\n");
    filestore_ram_t fs;
    const d_filestore_t *b = filestore_ram_init(&fs);
    CHECK(b != NULL && b->ctx == &fs, "initialises and carries its own context");

    uint8_t data[100];
    (void)fill(data, sizeof(data));
    CHECK(b->create(b->ctx, 0x002001u, 100u), "creates");
    uint32_t total = 0, held = 0;
    CHECK(b->stat(b->ctx, 0x002001u, &total, &held) && total == 100u && held == 0u, "empty: the size is known, none of it is here");
    CHECK(!b->have(b->ctx, 0x002001u, 0u), "and no chunk is present");

    uint8_t out[100];
    CHECK(b->read(b->ctx, 0x002001u, 0u, out, 10u) < 0, "reading what was never written FAILS rather than inventing it");

    CHECK(b->write(b->ctx, 0x002001u, 0u, data, 32u) == 32, "writes the first chunk");
    CHECK(b->have(b->ctx, 0x002001u, 0u), "chunk 0 is now present");
    CHECK(!b->have(b->ctx, 0x002001u, 1u), "chunk 1 is not");
    CHECK(b->stat(b->ctx, 0x002001u, &total, &held) && held == 32u, "32 bytes held");

    /* A hole: write chunk 2 and leave chunk 1 out, which is what a lost frame looks like. */
    CHECK(b->write(b->ctx, 0x002001u, 64u, &data[64], 32u) == 32, "writes chunk 2, skipping chunk 1");
    CHECK(b->have(b->ctx, 0x002001u, 2u) && !b->have(b->ctx, 0x002001u, 1u), "and the hole is visible as a hole");
    CHECK(b->read(b->ctx, 0x002001u, 32u, out, 32u) < 0, "the hole cannot be read");
    CHECK(b->read(b->ctx, 0x002001u, 64u, out, 32u) == 32 && memcmp(out, &data[64], 32u) == 0, "what is there reads back exactly");

    CHECK(b->write(b->ctx, 0x002001u, 32u, &data[32], 32u) == 32, "fills the hole");
    CHECK(b->write(b->ctx, 0x002001u, 96u, &data[96], 4u) == 4, "and the short final run");
    CHECK(b->have(b->ctx, 0x002001u, 3u), "the FINAL chunk is short and still counts as present");
    CHECK(b->stat(b->ctx, 0x002001u, &total, &held) && held == 100u, "all 100 bytes held");
    CHECK(b->complete(b->ctx, 0x002001u), "so it is complete");
    CHECK(b->read(b->ctx, 0x002001u, 0u, out, 100u) == 100 && memcmp(out, data, 100u) == 0, "and the whole thing round-trips");

    CHECK(b->write(b->ctx, 0x002001u, 99u, data, 2u) < 0, "a write past the declared size is refused");
    CHECK(b->read(b->ctx, 0x002001u, 99u, out, 2u) < 0, "and so is a read");

    CHECK(b->rename(b->ctx, 0x002001u, 0x003001u), "renames");
    CHECK(!b->stat(b->ctx, 0x002001u, NULL, NULL), "the old id is gone");
    CHECK(b->stat(b->ctx, 0x003001u, &total, &held) && held == 100u, "and the new one has the bytes");

    uint32_t cursor = 0u, id = 0u;
    int seen = 0;
    while (b->iterate(b->ctx, &cursor, &id))
        seen++;
    CHECK(seen == 1, "iterate finds the one file");
    b->destroy(b->ctx, 0x003001u);
    CHECK(iotdata_node_content_count(NULL) == 0u, "a NULL module counts nothing rather than crashing");
    cursor = 0u;
    CHECK(!b->iterate(b->ctx, &cursor, &id), "and after destroy there is nothing to find");
}

/* ------------------------------------------------------------------------------------------- */

static void test_insert_happy(void) {
    printf("\nan insert becomes a real file only once its checksum agrees\n");
    filestore_ram_t fs;
    iotdata_node_content_t c;
    fake_t keeper = { .tag = "keeper", .on_claim = R_MINE };
    const iotdata_node_content_consumer_t k = consumer_of(&keeper);
    iotdata_node_content_init(&c, filestore_ram_init(&fs));
    CHECK(iotdata_node_content_register(&c, &k), "a consumer registers");
    uint32_t now = 1000u;
    CHECK(iotdata_node_content_begin(&c, now) == 0, "an empty store announces nothing");

    uint8_t data[300];
    (void)fill(data, sizeof(data));
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);

    CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, sizeof(data), crc, now) == IOTDATA_NODE_CONTENT_OK, "the insert is declared");
    CHECK(iotdata_node_content_insert_active(&c), "and is in flight");
    uint32_t sid = 0u, cursor = 0u;
    CHECK(fs.backend.iterate(&fs, &cursor, &sid) && iotdata_node_content_id_is_scratch(sid), "the bytes are going to a SCRATCH id, not to 0x5BF001");
    CHECK(!fs.backend.stat(&fs, 0x5BF001u, NULL, NULL), "which does not exist yet");

    for (uint32_t off = 0; off < sizeof(data); off += 64u) {
        const uint32_t n = ((sizeof(data) - off) < 64u) ? (uint32_t)(sizeof(data) - off) : 64u;
        CHECK(iotdata_node_content_insert_write(&c, off, &data[off], n, now) == IOTDATA_NODE_CONTENT_OK, "a run lands");
        now += 10u;
    }
    uint32_t pid = 0, have = 0, want = 0;
    iotdata_node_content_insert_progress(&c, &pid, &have, &want);
    CHECK(pid == 0x5BF001u && have == sizeof(data) && want == sizeof(data), "progress says every declared byte is in");
    CHECK(keeper.claims == 0, "and nothing has been offered yet -- the tick does that, not the write path");

    CHECK(iotdata_node_content_tick(&c, now) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "the tick verifies it and reports ARRIVED");
    CHECK(!iotdata_node_content_insert_active(&c), "the insert is done");
    CHECK(fs.backend.stat(&fs, 0x5BF001u, NULL, NULL), "the file is at its real id");
    CHECK(!fs.backend.stat(&fs, sid, NULL, NULL), "and the scratch id is gone");
    CHECK(keeper.claims == 1 && keeper.last_id == 0x5BF001u && keeper.last_size == sizeof(data), "the consumer was offered it, by its real id and size");
    CHECK(c.stat_inserted == 1u && c.stat_insert_failed == 0u, "counted as one insert, no failures");
    CHECK(fs.stat_complete == 1u, "and the backend was given its chance to finalise");

    uint8_t back[300];
    CHECK(fs.backend.read(&fs, 0x5BF001u, 0u, back, sizeof(back)) == (int)sizeof(back) && memcmp(back, data, sizeof(data)) == 0, "the bytes are the bytes that went in");
}

/* ------------------------------------------------------------------------------------------- */

static void test_insert_refusals(void) {
    printf("\nand every way an insert does not become one\n");
    uint8_t data[300];
    (void)fill(data, sizeof(data));
    const uint64_t crc = _iotdata_crc64(data, sizeof(data), _IOTDATA_CRC64_INIT);

    /* --- the wrong bytes, in the right number --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t f = { .tag = "f", .on_claim = R_MINE };
        const iotdata_node_content_consumer_t k = consumer_of(&f);
        iotdata_node_content_init(&c, filestore_ram_init(&fs));
        (void)iotdata_node_content_register(&c, &k);
        uint32_t now = 1000u;
        (void)iotdata_node_content_begin(&c, now);
        uint8_t bent[300];
        memcpy(bent, data, sizeof(bent));
        bent[150] ^= 0x01u; /* one bit, in the middle */
        CHECK(upload(&c, 0x5BF001u, bent, sizeof(bent), 64u, crc, &now) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "one flipped bit fails the checksum");
        CHECK(iotdata_node_content_insert_why(&c) == IOTDATA_NODE_CONTENT_ERR_CHECKSUM, "and says why");
        CHECK(!fs.backend.stat(&fs, 0x5BF001u, NULL, NULL), "nothing landed at the real id");
        CHECK(iotdata_node_content_count(&c) == 0u, "and the scratch was destroyed, so the store is empty");
        CHECK(f.claims == 0, "a file that failed is never offered to anybody");
    }

    /* --- a line went missing --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        iotdata_node_content_init(&c, filestore_ram_init(&fs));
        uint32_t now = 1000u;
        (void)iotdata_node_content_begin(&c, now);
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, sizeof(data), crc, now) == IOTDATA_NODE_CONTENT_OK, "declared");
        CHECK(iotdata_node_content_insert_write(&c, 0u, data, 64u, now) == IOTDATA_NODE_CONTENT_OK, "run 1");
        CHECK(iotdata_node_content_insert_write(&c, 128u, &data[128], 64u, now) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "run 3 arrives where run 2 should be, and is refused");
        /* A REFUSAL STICKS, so the caller can keep feeding to the declared end -- which is what a
           console must do rather than drop back to interpreting the stream as commands. */
        CHECK(iotdata_node_content_insert_write(&c, 64u, &data[64], 64u, now) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "and so is the run that WOULD have been correct");
        CHECK(iotdata_node_content_insert_write(&c, 192u, &data[192], 64u, now) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "and every one after it");
        CHECK(iotdata_node_content_tick(&c, now) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "the tick reports the failure");
        CHECK(iotdata_node_content_insert_why(&c) == IOTDATA_NODE_CONTENT_ERR_OFFSET, "naming the offset, not the checksum");
        CHECK(iotdata_node_content_count(&c) == 0u, "and the scratch is gone");
    }

    /* --- nothing arrives --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        iotdata_node_content_init(&c, filestore_ram_init(&fs));
        uint32_t now = 1000u;
        (void)iotdata_node_content_begin(&c, now);
        (void)iotdata_node_content_insert_begin(&c, 0x5BF001u, sizeof(data), crc, now);
        (void)iotdata_node_content_insert_write(&c, 0u, data, 64u, now);
        CHECK(iotdata_node_content_tick(&c, now + 1000u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "a second later it is simply still arriving");
        CHECK(iotdata_node_content_tick(&c, now + (IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S * 1000u)) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "at the timeout it is abandoned");
        CHECK(iotdata_node_content_insert_why(&c) == IOTDATA_NODE_CONTENT_ERR_TIMEOUT, "for the right reason");
        CHECK(iotdata_node_content_count(&c) == 0u, "and leaves nothing behind");
    }

    /* --- too many bytes, and a store that says no --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        iotdata_node_content_init(&c, filestore_ram_init(&fs));
        uint32_t now = 1000u;
        (void)iotdata_node_content_begin(&c, now);
        (void)iotdata_node_content_insert_begin(&c, 0x5BF001u, 100u, crc, now);
        CHECK(iotdata_node_content_insert_write(&c, 0u, data, 101u, now) == IOTDATA_NODE_CONTENT_ERR_SIZE, "more bytes than were declared is refused");
        (void)iotdata_node_content_tick(&c, now);

        fs.fail_write = true;
        (void)iotdata_node_content_insert_begin(&c, 0x5BF002u, 100u, crc, now);
        CHECK(iotdata_node_content_insert_write(&c, 0u, data, 50u, now) == IOTDATA_NODE_CONTENT_ERR_STORE, "a store that refuses the write is reported as such");
        CHECK(iotdata_node_content_tick(&c, now) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "and the insert fails");

        fs.fail_create = true;
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF003u, 100u, crc, now) == IOTDATA_NODE_CONTENT_ERR_STORE, "a store that cannot create the scratch refuses at the start");
    }

    /* --- declarations that are not worth starting --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        iotdata_node_content_init(&c, filestore_ram_init(&fs));
        uint32_t now = 1000u;
        (void)iotdata_node_content_begin(&c, now);
        CHECK(iotdata_node_content_insert_begin(&c, 0x000001u, 100u, 0u, now) == IOTDATA_NODE_CONTENT_ERR_ID, "a scratch id is not somewhere a file can land");
        CHECK(iotdata_node_content_insert_begin(&c, 0xFFF001u, 100u, 0u, now) == IOTDATA_NODE_CONTENT_ERR_ID, "nor is the broadcast station");
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, 0u, 0u, now) == IOTDATA_NODE_CONTENT_ERR_SIZE, "a zero-length file is not a file");
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, FILESTORE_RAM_BYTES + 1u, 0u, now) == IOTDATA_NODE_CONTENT_ERR_SIZE, "and one the store has no room for is refused before a byte moves");
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, 100u, 0u, now) == IOTDATA_NODE_CONTENT_OK, "a sound declaration is accepted");
        CHECK(iotdata_node_content_insert_begin(&c, 0x5BF002u, 100u, 0u, now) == IOTDATA_NODE_CONTENT_ERR_STATE, "and a SECOND one is not -- one active job");
    }
}

/* ------------------------------------------------------------------------------------------- */

static void test_supersede(void) {
    printf("\nnew bytes under an old id replace it -- but only once they have been proven\n");
    filestore_ram_t fs;
    iotdata_node_content_t c;
    iotdata_node_content_init(&c, filestore_ram_init(&fs));
    uint32_t now = 1000u;
    (void)iotdata_node_content_begin(&c, now);

    uint8_t first[100], second[200];
    (void)fill(first, sizeof(first));
    for (uint32_t i = 0; i < sizeof(second); i++)
        second[i] = (uint8_t)(0xA0u + (i & 0x0Fu));

    CHECK(upload(&c, 0x5BF001u, first, sizeof(first), 64u, _iotdata_crc64(first, sizeof(first), _IOTDATA_CRC64_INIT), &now) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "the first lands");
    uint32_t total = 0u;
    CHECK(fs.backend.stat(&fs, 0x5BF001u, &total, NULL) && total == sizeof(first), "at its own size");

    /* A FAILED replacement must not cost the good file -- which is why the destroy happens at the
       rename and not when the insert is declared. */
    uint8_t bent[200];
    memcpy(bent, second, sizeof(bent));
    bent[7] ^= 0xFFu;
    CHECK(upload(&c, 0x5BF001u, bent, sizeof(bent), 64u, _iotdata_crc64(second, sizeof(second), _IOTDATA_CRC64_INIT), &now) == IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED, "a replacement that fails its checksum fails");
    CHECK(fs.backend.stat(&fs, 0x5BF001u, &total, NULL) && total == sizeof(first), "and the file that was already there is UNTOUCHED");
    uint8_t back[100];
    CHECK(fs.backend.read(&fs, 0x5BF001u, 0u, back, sizeof(back)) == (int)sizeof(back) && memcmp(back, first, sizeof(first)) == 0, "byte for byte");

    CHECK(upload(&c, 0x5BF001u, second, sizeof(second), 64u, _iotdata_crc64(second, sizeof(second), _IOTDATA_CRC64_INIT), &now) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "a replacement that passes lands");
    CHECK(fs.backend.stat(&fs, 0x5BF001u, &total, NULL) && total == sizeof(second), "and the id now holds the new bytes");
    CHECK(iotdata_node_content_count(&c) == 1u, "with no second copy left over");
}

/* ------------------------------------------------------------------------------------------- */

static void test_cold_boot(void) {
    printf("\na cold boot: scratch offered, then reaped; everything else offered to its consumers\n");
    uint8_t blob[64];
    (void)fill(blob, sizeof(blob));

    /* --- nobody wants the scratch --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t f = { .tag = "keeper", .on_claim = R_MINE };
        const iotdata_node_content_consumer_t k = consumer_of(&f);
        (void)filestore_ram_init(&fs);
        CHECK(filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob)), "a real file is planted, as a factory image would");
        CHECK(filestore_ram_plant(&fs, 0x5BF002u, blob, sizeof(blob)), "and a second");
        CHECK(filestore_ram_plant(&fs, 0x000003u, blob, sizeof(blob)), "and a scratch file, as an interrupted upload would leave");
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);

        CHECK(iotdata_node_content_begin(&c, 1000u) == 3, "all three are announced");
        CHECK(!fs.backend.stat(&fs, 0x000003u, NULL, NULL), "the scratch nobody claimed is GONE");
        CHECK(c.stat_scratch_reaped == 1u, "and counted as reaped");
        CHECK(fs.backend.stat(&fs, 0x5BF001u, NULL, NULL) && fs.backend.stat(&fs, 0x5BF002u, NULL, NULL), "the real files are untouched");
        CHECK(c.owns == 2u, "and both are owned");
        CHECK(f.claims == 3, "the scratch WAS offered first -- that is the hook, not an oversight");
    }

    /* --- an assembler takes its scratch back --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t asm_ = { .tag = "assembler", .on_claim = R_MINE, .claim_scratch = true };
        const iotdata_node_content_consumer_t k = consumer_of(&asm_);
        (void)filestore_ram_init(&fs);
        (void)filestore_ram_plant(&fs, 0x000003u, blob, sizeof(blob));
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);
        CHECK(iotdata_node_content_begin(&c, 1000u) == 1, "the scratch is announced");
        CHECK(fs.backend.stat(&fs, 0x000003u, NULL, NULL), "and SURVIVES, because a consumer claimed it");
        CHECK(c.stat_scratch_reaped == 0u, "nothing was reaped");
        CHECK(c.owns == 1u, "it is owned");
    }

    /* --- ownership is not persisted, and does not need to be --- */
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t f = { .tag = "keeper", .on_claim = R_MINE };
        const iotdata_node_content_consumer_t k = consumer_of(&f);
        (void)filestore_ram_init(&fs);
        (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);
        (void)iotdata_node_content_begin(&c, 1000u);
        CHECK(c.owns == 1u && f.claims == 1, "claimed on the first boot");
        /* A restart: the module forgets everything and the store does not. */
        iotdata_node_content_end(&c);
        CHECK(c.owns == 0u, "end() clears the ownership table");
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);
        (void)iotdata_node_content_begin(&c, 2000u);
        CHECK(c.owns == 1u && f.claims == 2, "and the second boot offers it again, which is how a soft-load re-runs");
    }
}

/* ------------------------------------------------------------------------------------------- */

static void test_claim_results(void) {
    printf("\nwhat each answer a consumer can give actually does\n");
    uint8_t blob[64];
    (void)fill(blob, sizeof(blob));

    struct {
        const char *what;
        iotdata_node_content_result_t r;
        bool survives, owned;
    } cases[] = {
        { "nothing: not mine, so it just sits there", IOTDATA_NODE_CONTENT_R_NONE, true, false },
        { "MINE: kept, and the owner is recorded", R_MINE, true, true },
        { "DELETE alone: refused outright and destroyed", R_DELETE, false, false },
        { "MINE|DELETE: taken, dealt with, and destroyed", R_MINE | R_DELETE, false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t f = { .tag = "f", .on_claim = cases[i].r };
        const iotdata_node_content_consumer_t k = consumer_of(&f);
        (void)filestore_ram_init(&fs);
        (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);
        (void)iotdata_node_content_begin(&c, 1000u);
        CHECK(fs.backend.stat(&fs, 0x5BF001u, NULL, NULL) == cases[i].survives, cases[i].what);
        CHECK((c.owns == 1u) == cases[i].owned, "  ... and the ownership table agrees");
    }

    printf("\nMINE|WORK is ticked until it stops, and the last word can demand a reboot\n");
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t f = { .tag = "ota", .on_claim = R_MINE | R_WORK, .work_n = 3 };
        f.work_script[0] = R_MINE | R_WORK;              /* still going */
        f.work_script[1] = R_MINE | R_WORK;              /* still going */
        f.work_script[2] = R_MINE | R_DELETE | R_REBOOT; /* done: bin it and restart */
        const iotdata_node_content_consumer_t k = consumer_of(&f);
        (void)filestore_ram_init(&fs);
        (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c, &fs.backend);
        (void)iotdata_node_content_register(&c, &k);
        (void)iotdata_node_content_begin(&c, 1000u);
        CHECK(c.owns == 1u && c.owned[0].working, "claimed, and marked as working");

        CHECK(iotdata_node_content_tick(&c, 1010u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "tick 1: a slice, nothing to report");
        CHECK(iotdata_node_content_tick(&c, 1020u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "tick 2: another slice");
        CHECK(f.works == 2, "and it really was called twice -- one bounded piece per pass");
        CHECK(iotdata_node_content_tick(&c, 1030u) == IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, "tick 3: finished, and the node must restart");
        CHECK(!fs.backend.stat(&fs, 0x5BF001u, NULL, NULL), "the file it consumed is gone");
        CHECK(c.owns == 0u, "and so is the ownership record");
        CHECK(iotdata_node_content_tick(&c, 1040u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "with nothing left to tick");
    }

    printf("\nthe first claimant wins, and the ones after it are never asked\n");
    {
        filestore_ram_t fs;
        iotdata_node_content_t c;
        fake_t a = { .tag = "first", .on_claim = IOTDATA_NODE_CONTENT_R_NONE };
        fake_t b = { .tag = "second", .on_claim = R_MINE };
        fake_t d = { .tag = "third", .on_claim = R_MINE };
        const iotdata_node_content_consumer_t ka = consumer_of(&a), kb = consumer_of(&b), kd = consumer_of(&d);
        (void)filestore_ram_init(&fs);
        (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c, &fs.backend);
        CHECK(iotdata_node_content_register(&c, &ka) && iotdata_node_content_register(&c, &kb) && iotdata_node_content_register(&c, &kd), "three register");
        (void)iotdata_node_content_begin(&c, 1000u);
        CHECK(a.claims == 1 && b.claims == 1, "the first two were asked");
        CHECK(d.claims == 0, "the third never was");
        const char *owner = NULL;
        uint32_t cursor = 0u, id = 0u;
        CHECK(iotdata_node_content_iterate(&c, &cursor, &id, NULL, NULL, &owner) && owner != NULL && strcmp(owner, "second") == 0, "and the listing names the one that took it");
    }
}

/* ------------------------------------------------------------------------------------------- */

static void test_reoffer(void) {
    printf("\na bare MINE means \"ask me again\", which is why there is no separate \"not now\"\n");
    uint8_t blob[64];
    (void)fill(blob, sizeof(blob));
    filestore_ram_t fs;
    iotdata_node_content_t c;
    fake_t f = { .tag = "ota", .on_claim = R_MINE };
    const iotdata_node_content_consumer_t k = consumer_of(&f);
    (void)filestore_ram_init(&fs);
    (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
    iotdata_node_content_init(&c, &fs.backend);
    (void)iotdata_node_content_register(&c, &k);
    (void)iotdata_node_content_begin(&c, 1000u);
    CHECK(f.claims == 1, "claimed once at boot");

    (void)iotdata_node_content_tick(&c, 1000u + 1000u);
    CHECK(f.claims == 1, "a second later it is not pestered");
    const uint32_t due = 1000u + (IOTDATA_NODE_CONTENT_REOFFER_S * 1000u);
    (void)iotdata_node_content_tick(&c, due);
    CHECK(f.claims == 2, "at the cadence it is asked again");
    CHECK(c.stat_reoffers == 1u, "and that is counted as a re-offer, not as a fresh announce");
    CHECK(c.stat_announced == 1u, "which it is not");

    /* The whole point: the answer can CHANGE. This is OTA_SCHEDULE arriving, or a battery
       recovering, without any of it being modelled here. */
    f.on_claim = R_MINE | R_WORK;
    f.work_n = 1;
    f.work_script[0] = R_MINE;
    (void)iotdata_node_content_tick(&c, due + (IOTDATA_NODE_CONTENT_REOFFER_S * 1000u));
    CHECK(c.owns == 1u && c.owned[0].working, "the owner can upgrade itself to working on a re-offer");
    CHECK(iotdata_node_content_tick(&c, due + 200000u) == IOTDATA_NODE_CONTENT_EVENT_WORK_DONE, "and then be ticked");

    printf("\nbut an UNOWNED file is never re-offered: nobody wanted it and nothing has changed\n");
    {
        filestore_ram_t fs2;
        iotdata_node_content_t c2;
        fake_t g = { .tag = "nobody", .on_claim = IOTDATA_NODE_CONTENT_R_NONE };
        const iotdata_node_content_consumer_t kg = consumer_of(&g);
        (void)filestore_ram_init(&fs2);
        (void)filestore_ram_plant(&fs2, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c2, &fs2.backend);
        (void)iotdata_node_content_register(&c2, &kg);
        (void)iotdata_node_content_begin(&c2, 1000u);
        CHECK(g.claims == 1 && c2.owns == 0u, "offered once, taken by nobody");
        for (uint32_t t = 1000u; t < 1000u + (IOTDATA_NODE_CONTENT_REOFFER_S * 5000u); t += 10000u)
            (void)iotdata_node_content_tick(&c2, t);
        CHECK(g.claims == 1, "and five cadences later it has still only been asked once");
        CHECK(fs2.backend.stat(&fs2, 0x5BF001u, NULL, NULL), "while the file itself simply sits there");
    }
}

/* ------------------------------------------------------------------------------------------- */

static void test_list_delete_describe(void) {
    printf("\nlist, delete and inspect\n");
    uint8_t blob[64];
    (void)fill(blob, sizeof(blob));
    filestore_ram_t fs;
    iotdata_node_content_t c;
    fake_t owner = { .tag = "ota", .on_claim = R_MINE, .describe_yes = true };
    fake_t quiet = { .tag = "quiet", .on_claim = IOTDATA_NODE_CONTENT_R_NONE, .describe_yes = true };
    const iotdata_node_content_consumer_t ko = consumer_of(&owner), kq = consumer_of(&quiet);
    (void)filestore_ram_init(&fs);
    (void)filestore_ram_plant(&fs, 0x5BF001u, blob, sizeof(blob));
    iotdata_node_content_init(&c, &fs.backend);
    (void)iotdata_node_content_register(&c, &ko);
    (void)iotdata_node_content_register(&c, &kq);
    (void)iotdata_node_content_begin(&c, 1000u);

    /* Unowned files are listed too -- that is what the owner column is for. */
    (void)filestore_ram_plant(&fs, 0x5BF009u, blob, 32u);
    CHECK(iotdata_node_content_count(&c) == 2u, "the listing walks the STORE, so an unannounced file is in it");
    uint32_t cursor = 0u, id = 0u, total = 0u, held = 0u;
    int owned_seen = 0, unowned_seen = 0;
    const char *who = NULL;
    while (iotdata_node_content_iterate(&c, &cursor, &id, &total, &held, &who)) {
        if (who != NULL)
            owned_seen++;
        else
            unowned_seen++;
        CHECK(total == held, "every file listed is whole");
    }
    CHECK(owned_seen == 1 && unowned_seen == 1, "one owned, one not");

    char buf[IOTDATA_NODE_CONTENT_DESCRIBE_MAX + 1];
    CHECK(iotdata_node_content_describe(&c, 0x5BF001u, buf, sizeof(buf)) > 0 && strncmp(buf, "ota says", 8) == 0, "an owned file is described by its OWNER");
    CHECK(owner.describes == 1 && quiet.describes == 0, "and nobody else is asked -- there is one opinion about who owns a file");
    CHECK(iotdata_node_content_describe(&c, 0x5BF009u, buf, sizeof(buf)) > 0 && strncmp(buf, "ota says", 8) == 0, "an UNOWNED file is offered round until somebody can say something");

    owner.describe_yes = false;
    quiet.describe_yes = false;
    CHECK(iotdata_node_content_describe(&c, 0x5BF009u, buf, sizeof(buf)) == 0 && buf[0] == '\0', "and if nobody can, it says nothing rather than something made up");

    CHECK(iotdata_node_content_delete(&c, 0x5BF009u) == IOTDATA_NODE_CONTENT_OK, "an unowned file deletes");
    /* ABSENT and not ERR_ID: a well-formed id with nothing under it is a different complaint from
       an id that could never name anything, and an operator reading one line needs to know which. */
    CHECK(iotdata_node_content_delete(&c, 0x5BF009u) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "and deleting it twice says it is not there");
    CHECK(iotdata_node_content_delete(&c, 0x000123u) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "a scratch id nothing is under is absent too");
    CHECK(iotdata_node_content_delete(&c, 0x5BF001u) == IOTDATA_NODE_CONTENT_OK, "an owned but idle file deletes");
    CHECK(c.owns == 0u, "and takes its ownership record with it");

    printf("\nbut not out from under something that is using it\n");
    {
        filestore_ram_t fs2;
        iotdata_node_content_t c2;
        fake_t w = { .tag = "busy", .on_claim = R_MINE | R_WORK, .work_n = 4 };
        for (int i = 0; i < 4; i++)
            w.work_script[i] = R_MINE | R_WORK;
        const iotdata_node_content_consumer_t kw = consumer_of(&w);
        (void)filestore_ram_init(&fs2);
        (void)filestore_ram_plant(&fs2, 0x5BF001u, blob, sizeof(blob));
        iotdata_node_content_init(&c2, &fs2.backend);
        (void)iotdata_node_content_register(&c2, &kw);
        (void)iotdata_node_content_begin(&c2, 1000u);
        CHECK(iotdata_node_content_delete(&c2, 0x5BF001u) == IOTDATA_NODE_CONTENT_ERR_STATE, "a file its owner is working on will not be deleted");
        CHECK(iotdata_node_content_insert_begin(&c2, 0x5BF002u, 64u, 0u, 1000u) == IOTDATA_NODE_CONTENT_ERR_STATE, "and no insert may start while it works -- one active job");

        /* And the other way round: the target of an upload in flight is not deletable either. */
        iotdata_node_content_t c3;
        filestore_ram_t fs3;
        iotdata_node_content_init(&c3, filestore_ram_init(&fs3));
        (void)iotdata_node_content_begin(&c3, 1000u);
        (void)iotdata_node_content_insert_begin(&c3, 0x5BF001u, 64u, 0u, 1000u);
        CHECK(iotdata_node_content_delete(&c3, 0x5BF001u) == IOTDATA_NODE_CONTENT_ERR_STATE, "the destination of a running insert is not deletable");
    }
}

/* ------------------------------------------------------------------------------------------- */

static void test_dimensions(void) {
    printf("\nmisdimensioned firmware is counted, not hidden\n");
    uint8_t blob[16];
    (void)fill(blob, sizeof(blob));
    filestore_ram_t fs;
    iotdata_node_content_t c;
    fake_t f = { .tag = "greedy", .on_claim = R_MINE };
    const iotdata_node_content_consumer_t k = consumer_of(&f);
    (void)filestore_ram_init(&fs);
    for (uint32_t i = 0; i < FILESTORE_RAM_SLOTS; i++)
        (void)filestore_ram_plant(&fs, 0x5BF000u + i + 1u, blob, sizeof(blob));
    iotdata_node_content_init(&c, &fs.backend);
    (void)iotdata_node_content_register(&c, &k);
    (void)iotdata_node_content_begin(&c, 1000u);

    CHECK(c.owns == IOTDATA_NODE_CONTENT_OWNED_MAX, "the ownership table fills to its bound");
    if (FILESTORE_RAM_SLOTS > IOTDATA_NODE_CONTENT_OWNED_MAX)
        CHECK(c.stat_own_full > 0u, "and says so when it cannot record another claim");
    else
        printf("  (store holds %d, table holds %d -- no overflow to provoke)\n", FILESTORE_RAM_SLOTS, IOTDATA_NODE_CONTENT_OWNED_MAX);

    /* register() is bounded too, and refuses rather than overruns. */
    fake_t extra[IOTDATA_NODE_CONTENT_CONSUMERS_MAX + 1];
    iotdata_node_content_consumer_t ke[IOTDATA_NODE_CONTENT_CONSUMERS_MAX + 1];
    iotdata_node_content_t c2;
    iotdata_node_content_init(&c2, &fs.backend);
    bool refused = false;
    for (int i = 0; i <= IOTDATA_NODE_CONTENT_CONSUMERS_MAX; i++) {
        extra[i] = (fake_t){ .tag = "x" };
        ke[i] = consumer_of(&extra[i]);
        if (!iotdata_node_content_register(&c2, &ke[i]))
            refused = (i == IOTDATA_NODE_CONTENT_CONSUMERS_MAX);
    }
    CHECK(refused, "the consumer table takes its bound and refuses the next");
}

/* ------------------------------------------------------------------------------------------- */

static void test_degenerate(void) {
    printf("\nand the calls nobody should make\n");
    iotdata_node_content_t c;
    iotdata_node_content_init(&c, NULL);
    CHECK(iotdata_node_content_begin(&c, 0u) == -1, "no backend: begin says so rather than crashing");
    CHECK(iotdata_node_content_tick(&c, 0u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "and the tick does nothing");
    CHECK(iotdata_node_content_insert_begin(&c, 0x5BF001u, 1u, 0u, 0u) == IOTDATA_NODE_CONTENT_ERR_STATE, "and no insert can start");
    iotdata_node_content_init(NULL, NULL);
    CHECK(!iotdata_node_content_register(NULL, NULL), "a NULL module registers nothing");
    CHECK(iotdata_node_content_announce(NULL, 1u, 0u) == IOTDATA_NODE_CONTENT_R_NONE, "and announces nothing");
    iotdata_node_content_end(NULL);

    filestore_ram_t fs;
    iotdata_node_content_t c2;
    iotdata_node_content_init(&c2, filestore_ram_init(&fs));
    CHECK(iotdata_node_content_tick(&c2, 0u) == IOTDATA_NODE_CONTENT_EVENT_NONE, "a module that was never begun does nothing");
    CHECK(iotdata_node_content_insert_write(&c2, 0u, "x", 1u, 0u) == IOTDATA_NODE_CONTENT_ERR_STATE, "a write with no insert in flight is refused");
    CHECK(iotdata_node_content_announce(&c2, 0x5BF001u, 0u) == IOTDATA_NODE_CONTENT_R_NONE, "announcing a file that is not there claims nothing");
    CHECK(c2.stat_announced == 0u, "and is not even counted as an announcement");

    /* end() during an upload must not leave the scratch behind. */
    (void)iotdata_node_content_begin(&c2, 1000u);
    (void)iotdata_node_content_insert_begin(&c2, 0x5BF001u, 64u, 0u, 1000u);
    CHECK(iotdata_node_content_count(&c2) == 1u, "an upload in flight has a scratch file");
    iotdata_node_content_end(&c2);
    CHECK(iotdata_node_content_count(&c2) == 0u, "and end() takes it with it");
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_node_content\n");
    test_ids();
    test_backend();
    test_insert_happy();
    test_insert_refusals();
    test_supersede();
    test_cold_boot();
    test_claim_results();
    test_reoffer();
    test_list_delete_describe();
    test_dimensions();
    test_degenerate();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
