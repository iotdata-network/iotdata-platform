//
// test_node_filestore_linux.c - the directory backend, against a real filesystem.
//
// THIS IS THE SUITE THE RAM BACKEND WAS WRITTEN TO BE COMPARED AGAINST. d_module_filestore_ram.h
// says so in as many words: it keeps a per-byte written map so that it is the strictest backend
// there is, and "the point of being exact here is that when the wire path lands, a disagreement
// between this and a device backend is a bug in the device backend rather than an ambiguity in the
// seam". Until now there was nothing to disagree with on a host -- the SPIFFS backend only exists
// on a device, so the only way to find out whether a real filesystem behaved like the model was to
// flash a relay. So the last block here drives BOTH backends through the same script and compares
// them answer for answer.
//
// It uses a real temporary directory and removes it afterwards, because the things worth testing
// about this backend are the things a mock cannot have: that a rename replaces, that a walk
// survives having an entry deleted under it, that a file nobody named as an id is left alone, and
// that the store's idea of what it holds comes back after a close and reopen.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* CRC-64 LIVES IN d_common.h, which needs a platform under it for its timer and delay helpers --
   so the host platform goes in first, exactly as the gateway does it. This suite calls neither;
   it is here for the checksum iotdata_node_content.h sums an upload with. */
#include "d_platform_linux.h"
#include "d_common.h"
#include "d_module_filestore.h" /* the seam: content.h is written against it */
#include "iotdata_node_content.h"
#include "d_module_filestore_linux.h"
#include "d_module_filestore_ram.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

static char g_dir[128];

static void rmrf(const char *const dir) {
    DIR *const d = opendir(dir);
    if (d != NULL) {
        for (const struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char path[512];
            (void)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            (void)unlink(path);
        }
        (void)closedir(d);
    }
    (void)rmdir(dir);
}

/* A fresh store, every time: a test that inherited the previous one's files would be testing the
   order the suite happens to run in. */
static const d_filestore_t *rig(filestore_linux_t *const fs, const uint32_t budget) {
    rmrf(g_dir);
    return filestore_linux_open(fs, g_dir, budget);
}

static void fill(uint8_t *const p, const uint32_t n, const uint8_t seed) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 31u);
}

static bool plant(const d_filestore_t *const be, const uint32_t id, const uint8_t *const data, const uint32_t len) {
    return be->create(be->ctx, id, len) && be->write(be->ctx, id, 0u, data, len) == (int)len && be->complete(be->ctx, id);
}

/* ------------------------------------------------------------------------------------------- */

static void test_open(void) {
    printf("\na directory becomes a store, and says whether it can keep anything\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    CHECK(be != NULL, "a directory that did not exist is created");
    CHECK(be->ctx == &fs, "and the vtable carries the store it belongs to");
    /* NOT asserted of this directory: /tmp is tmpfs on some of the boxes this suite runs on and a
       real filesystem on others, so its nature is a property of the machine and not of the code.
       What IS deterministic is /dev/shm, which is tmpfs everywhere it exists -- and that is the
       case worth pinning anyway, because a store that silently loses a firmware bundle on reboot
       is the failure this flag exists to report. */
    filestore_linux_t shm;
    char shmdir[128];
    (void)snprintf(shmdir, sizeof(shmdir), "/dev/shm/iotdata_filestore_test_%ld", (long)getpid());
    if (filestore_linux_open(&shm, shmdir, 0u) != NULL) {
        CHECK(!filestore_linux_persistent(&shm), "a store on tmpfs says so rather than hiding it");
        filestore_linux_close(&shm);
        rmrf(shmdir);
    }

    uint32_t used = 0u, budget = 0u;
    CHECK(filestore_linux_info(&fs, &used, &budget), "it can describe itself");
    CHECK(used == 0u, "a new store holds nothing");
    CHECK(budget > 0u, "and with no budget set, reports the filesystem's room as its own");
    filestore_linux_close(&fs);

    CHECK(filestore_linux_open(&fs, g_dir, 0u) != NULL, "opening it a second time finds it");
    filestore_linux_close(&fs);
    CHECK(!filestore_linux_persistent(&fs), "a closed store makes no claims at all");

    /* The failure that matters: a path that cannot be made. A caller is expected to carry on. */
    filestore_linux_t bad;
    CHECK(filestore_linux_open(&bad, "/proc/nowhere/content", 0u) == NULL, "a directory that cannot be created is refused, not faked");
    CHECK(filestore_linux_open(&bad, NULL, 0u) == NULL, "and so is no directory at all");
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

static void test_roundtrip(void) {
    printf("\nwhat goes in comes back out, in pieces and at offsets\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    uint8_t data[1000], back[1000];
    fill(data, sizeof(data), 7u);

    CHECK(be->create(be->ctx, 0x0537a1u, sizeof(data)), "created at its intended size");
    uint32_t total = 0u, held = 0u;
    CHECK(be->stat(be->ctx, 0x0537a1u, &total, &held) && total == sizeof(data) && held == 0u, "which stat reports before a byte arrives -- from RAM, since the disk cannot know it");

    /* in four pieces, because an insert is thousands of them */
    for (uint32_t off = 0; off < sizeof(data); off += 250u)
        CHECK(be->write(be->ctx, 0x0537a1u, off, data + off, 250u) == 250, "a piece lands");
    CHECK(be->stat(be->ctx, 0x0537a1u, &total, &held) && held == sizeof(data), "and the held count followed it");
    CHECK(be->complete(be->ctx, 0x0537a1u), "complete() agrees it is all there");

    CHECK(be->read(be->ctx, 0x0537a1u, 0u, back, sizeof(data)) == (int)sizeof(data), "read back whole");
    CHECK(memcmp(data, back, sizeof(data)) == 0, "byte for byte");
    memset(back, 0, sizeof(back));
    CHECK(be->read(be->ctx, 0x0537a1u, 400u, back, 100u) == 100, "and at an offset, which is what a download does");
    CHECK(memcmp(data + 400, back, 100u) == 0, "from the right place");

    /* The refusals, which are the half that keeps a bad transfer from looking like a good one. */
    CHECK(be->read(be->ctx, 0x0537a1u, 990u, back, 100u) == -1, "a range running off the end is REFUSED, not shortened");
    CHECK(be->read(be->ctx, 0x0537ffu, 0u, back, 10u) == -1, "and a file that is not there cannot be read");
    CHECK(fs.stat_read_fail == 2u, "both counted");
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

static void test_append_only(void) {
    printf("\na write is an append, and anything else is refused\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    uint8_t data[100];
    fill(data, sizeof(data), 3u);

    CHECK(be->create(be->ctx, 0x0537b1u, sizeof(data)), "created");
    CHECK(be->write(be->ctx, 0x0537b1u, 0u, data, 40u) == 40, "the first 40");
    CHECK(be->write(be->ctx, 0x0537b1u, 60u, data + 60, 40u) == -1, "a gap is refused -- a hole filled silently is the right length and the wrong content");
    CHECK(be->write(be->ctx, 0x0537b1u, 20u, data + 20, 20u) == -1, "and so is going back over what is already written");
    CHECK(be->write(be->ctx, 0x0537b1u, 40u, data + 40, 80u) == -1, "and so is running past the size it was created at");
    CHECK(be->write(be->ctx, 0x0537b1u, 40u, data + 40, 60u) == 60, "the actual next bytes are taken");

    uint32_t held = 0u;
    CHECK(be->stat(be->ctx, 0x0537b1u, NULL, &held) && held == sizeof(data), "and only those counted");
    CHECK(be->write(be->ctx, 0x0537c1u, 0u, data, 10u) == -1, "a write to an id nothing created goes nowhere");
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

static void test_rename_is_atomic(void) {
    printf("\nthe rename replaces, which is what makes an insert atomic\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    uint8_t older[50], newer[80], back[80];
    fill(older, sizeof(older), 1u);
    fill(newer, sizeof(newer), 9u);

    CHECK(plant(be, 0x0537d1u, older, sizeof(older)), "a file at a real id");
    CHECK(plant(be, 0x000001u, newer, sizeof(newer)), "and a scratch that is about to replace it");
    CHECK(be->rename(be->ctx, 0x000001u, 0x0537d1u), "the rename succeeds OVER the existing file");

    uint32_t total = 0u;
    CHECK(be->stat(be->ctx, 0x0537d1u, &total, NULL) && total == sizeof(newer), "and the destination is the new content");
    CHECK(be->read(be->ctx, 0x0537d1u, 0u, back, sizeof(newer)) == (int)sizeof(newer) && memcmp(newer, back, sizeof(newer)) == 0, "all of it");
    CHECK(!be->stat(be->ctx, 0x000001u, NULL, NULL), "with the scratch name gone");

    CHECK(!be->rename(be->ctx, 0x000002u, 0x0537d2u), "a rename of nothing fails rather than inventing a file");
    CHECK(!be->rename(be->ctx, 0x0537d1u, 0x0537d1u), "and an id cannot be renamed to itself");
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

static void test_walk(void) {
    printf("\nthe walk finds content, ignores everything else, and survives being disturbed\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    uint8_t data[10];
    fill(data, sizeof(data), 5u);
    CHECK(plant(be, 0x0537e1u, data, sizeof(data)), "three files");
    CHECK(plant(be, 0x0537e2u, data, sizeof(data)), "planted");
    CHECK(plant(be, 0x000003u, data, sizeof(data)), "one of them scratch");

    /* AND SOMETHING THAT IS NOT CONTENT. A gateway's content directory may well be a directory a
       person also keeps things in, and a store that swallowed a README would be unusable there. */
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/README", g_dir);
    FILE *const f = fopen(path, "wb");
    CHECK(f != NULL, "beside a file that is not named as an id");
    if (f != NULL) {
        (void)fputs("not content\n", f);
        (void)fclose(f);
    }

    uint32_t cursor = 0u, id = 0u, seen = 0u, mask = 0u;
    while (be->iterate(be->ctx, &cursor, &id)) {
        seen++;
        if (id == 0x0537e1u)
            mask |= 1u;
        else if (id == 0x0537e2u)
            mask |= 2u;
        else if (id == 0x000003u)
            mask |= 4u;
        else
            mask |= 8u; /* anything else is a bug: the README must not appear */
    }
    CHECK(seen == 3u, "exactly the three content files are walked");
    CHECK(mask == 7u, "and they are the three that were planted");

    /* An abandoned walk costs a rewind, not a leak -- and the proof is that the next one is whole. */
    cursor = 0u;
    CHECK(be->iterate(be->ctx, &cursor, &id), "a walk is started");
    cursor = 0u;
    seen = 0u;
    while (be->iterate(be->ctx, &cursor, &id))
        seen++;
    CHECK(seen == 3u, "and restarting it from zero sees everything again");

    /* A destroy mid-walk: the entry under the handle goes, and the walk must not keep claiming it. */
    cursor = 0u;
    CHECK(be->iterate(be->ctx, &cursor, &id), "mid-walk");
    be->destroy(be->ctx, 0x0537e1u);
    be->destroy(be->ctx, 0x0537e2u);
    be->destroy(be->ctx, 0x000003u);
    cursor = 0u;
    seen = 0u;
    while (be->iterate(be->ctx, &cursor, &id))
        seen++;
    CHECK(seen == 0u, "a store everything was deleted from walks as empty");
    CHECK(fs.stat_destroy == 3u, "and all three destroys were real");
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

/* THE SIDECAR, against a real filesystem. What the RAM model cannot test is the part that is
   actually filesystem behaviour: that a `.m` file is invisible to the walk, that a half-written one
   can never be read back, and that create() and destroy() both take it with them. */
static void test_the_sidecar(void) {
    printf("\na description beside the bytes: invisible to the walk, and never outliving them\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    uint8_t data[64];
    fill(data, sizeof(data), 9u);
    CHECK(plant(be, 0x0537d1u, data, sizeof(data)), "a file");
    CHECK(be->meta_write != NULL && be->meta_read != NULL, "the store keeps sidecars");

    uint8_t side[16] = { 'd', 'e', 's', 'c', 0 };
    CHECK(be->meta_write(be->ctx, 0x0537d1u, side, 5u) == 5, "a description is written");
    uint8_t back[D_FILESTORE_META_MAX];
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 5, "and read back whole");
    CHECK(memcmp(back, side, 5u) == 0, "byte for byte");
    /* NO SIDECAR IS NOT AN ERROR: most content has none, and a reader must be able to tell that
       from a store that is broken. */
    CHECK(be->meta_read(be->ctx, 0x0537d2u, back, sizeof(back)) == 0, "an id with none reads as none");

    /* INVISIBLE TO THE WALK, for nothing: the id recogniser admits a name only if it is exactly
       six hex digits, so `0537d1.m` was already not content. This is the property the whole choice
       of filename rests on, so it is pinned rather than assumed. */
    uint32_t cursor = 0u, id = 0u, seen = 0u;
    while (be->iterate(be->ctx, &cursor, &id)) {
        seen++;
        CHECK(id == 0x0537d1u, "the walk sees the content and not its description");
    }
    CHECK(seen == 1u, "one file, not two");

    /* AND SO IS THE TEMPORARY IT IS WRITTEN THROUGH, which is what stops a half-written sidecar
       from ever being read: `.m.t` is not six hex digits either. */
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/0537d1.m.t", g_dir);
    FILE *const t = fopen(path, "wb");
    CHECK(t != NULL, "a stray temporary");
    if (t != NULL) {
        (void)fputs("half", t);
        (void)fclose(t);
    }
    cursor = 0u;
    seen = 0u;
    while (be->iterate(be->ctx, &cursor, &id))
        seen++;
    CHECK(seen == 1u, "is not content either");
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 5, "and the real one still reads");

    /* LONGER THAN A SIDECAR MAY BE is refused rather than truncated: a description cut in half
       would parse as a different description. */
    uint8_t big[D_FILESTORE_META_MAX + 8];
    memset(big, 0xAB, sizeof(big));
    CHECK(be->meta_write(be->ctx, 0x0537d1u, big, sizeof(big)) < 0, "an oversized description is refused");
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 5, "leaving the one that was there");

    /* ZERO LENGTH WITHDRAWS IT, without touching the bytes. */
    CHECK(be->meta_write(be->ctx, 0x0537d1u, NULL, 0u) == 0, "a zero-length write removes it");
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 0, "and it is gone");
    uint32_t total = 0u, held = 0u;
    CHECK(be->stat(be->ctx, 0x0537d1u, &total, &held) && held == sizeof(data), "the content is untouched");

    /* DESTROY TAKES IT, or the next id to reuse that number inherits somebody else's description. */
    CHECK(be->meta_write(be->ctx, 0x0537d1u, side, 5u) == 5, "described again");
    be->destroy(be->ctx, 0x0537d1u);
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 0, "and destroying the file takes it");
    CHECK(plant(be, 0x0537d1u, data, sizeof(data)), "so a reused id");
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 0, "starts undescribed");

    /* AND SO DOES CREATE, because a new object under an old id is a different object -- the size
       and the hash can be re-derived from the new bytes, the NAME cannot. */
    CHECK(be->meta_write(be->ctx, 0x0537d1u, side, 5u) == 5, "described");
    CHECK(be->create(be->ctx, 0x0537d1u, 32u), "and then written over");
    CHECK(be->meta_read(be->ctx, 0x0537d1u, back, sizeof(back)) == 0, "the description went with the old bytes");

    filestore_linux_close(&fs);
}

static void test_budget(void) {
    printf("\nthe budget is the edge a directory does not otherwise have\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 1000u);
    uint8_t data[400];
    fill(data, sizeof(data), 11u);

    const uint32_t empty = be->space(be->ctx);
    CHECK(empty == 1000u, "an empty store has its whole budget, not the whole disk");
    CHECK(plant(be, 0x0537f1u, data, sizeof(data)), "400 bytes in");
    CHECK(be->space(be->ctx) == 600u, "and 600 of the budget is left");
    CHECK(plant(be, 0x0537f2u, data, sizeof(data)), "400 more");
    CHECK(be->space(be->ctx) == 200u, "leaving 200");
    CHECK(plant(be, 0x0537f3u, data, 200u), "which exactly fits");
    CHECK(be->space(be->ctx) == 0u, "and the store is full by its own reckoning");

    /* Over budget rather than over disk: destroying gives the room back, which a counter kept in
       RAM would also do -- the next case is the one that separates them. */
    be->destroy(be->ctx, 0x0537f1u);
    CHECK(be->space(be->ctx) == 400u, "deleting a file returns its bytes to the budget");

    /* COUNTED BY WALKING, so a file this store never inserted still counts against it. That is the
       whole reason used() is not a counter: a bundle dropped in by hand, or left by the previous
       run, is occupying the disk whether or not this process put it there. */
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/0537f9", g_dir);
    FILE *const f = fopen(path, "wb");
    CHECK(f != NULL, "a file arrives by other means");
    if (f != NULL) {
        CHECK(fwrite(data, 1u, 300u, f) == 300u, "300 bytes of it");
        (void)fclose(f);
    }
    CHECK(be->space(be->ctx) == 100u, "and it counts against the budget, because it is on the disk");

    uint32_t used = 0u, budget = 0u;
    CHECK(filestore_linux_info(&fs, &used, &budget) && used == 900u && budget == 1000u, "which is what info() reports too");
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

static void test_survives_reopen(void) {
    printf("\nand it is still there after the process that wrote it has gone\n");
    filestore_linux_t fs;
    const d_filestore_t *be = rig(&fs, 0u);
    uint8_t data[333], back[333];
    fill(data, sizeof(data), 23u);
    CHECK(plant(be, 0x053701u, data, sizeof(data)), "a file is written");
    filestore_linux_close(&fs);

    /* A different store struct entirely, as a restarted process would have. */
    filestore_linux_t again;
    be = filestore_linux_open(&again, g_dir, 0u);
    CHECK(be != NULL, "the directory is opened again");
    uint32_t total = 0u, held = 0u;
    CHECK(be->stat(be->ctx, 0x053701u, &total, &held), "the file is found");
    CHECK(total == sizeof(data) && held == total, "at its full length -- a finished file's size IS its intended size");
    CHECK(be->read(be->ctx, 0x053701u, 0u, back, sizeof(data)) == (int)sizeof(data) && memcmp(data, back, sizeof(data)) == 0, "and reads back unchanged");
    filestore_linux_close(&again);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

/* THE COMPARISON THE RAM BACKEND EXISTS FOR. One script, both backends, every answer checked
   against the other rather than against what this test thinks it should be -- so a disagreement is
   reported wherever it is, and neither backend is the assumed-correct one. */
static void test_agrees_with_the_model(void) {
    printf("\nthe directory and the exact RAM model answer identically\n");
    filestore_linux_t lfs;
    filestore_ram_t rfs;
    const d_filestore_t *const l = rig(&lfs, 0u);
    const d_filestore_t *const r = filestore_ram_init(&rfs);
    CHECK(l != NULL && r != NULL, "two stores of different kinds");

    uint8_t data[600], lb[600], rb[600];
    fill(data, sizeof(data), 41u);

#define BOTH(expr_l, expr_r, msg) CHECK((expr_l) == (expr_r), msg)

    BOTH(l->create(l->ctx, 0x053711u, 600u), r->create(r->ctx, 0x053711u, 600u), "create agrees");
    BOTH(l->write(l->ctx, 0x053711u, 0u, data, 200u), r->write(r->ctx, 0x053711u, 0u, data, 200u), "a write agrees");
    /* THE ONE PLACE THEY ARE MEANT TO DISAGREE, pinned here so nobody "fixes" either side into
       agreement by accident. A real backend takes an append and nothing else, because that is all
       the console path can produce and a hole filled silently is undetectable. The RAM model
       deliberately accepts an out-of-order write and tracks it per byte, because that is the shape
       §10's wire path will need and the model is where it gets designed. The module above the seam
       refuses a non-append offset itself, so neither store is ever asked for one in anger. */
    CHECK(l->write(l->ctx, 0x053711u, 400u, data + 400, 200u) == -1, "the directory refuses a GAP");
    CHECK(r->write(r->ctx, 0x053711u, 400u, data + 400, 200u) == 200, "where the model accepts one on purpose -- out-of-order is what it is for");
    CHECK(l->write(l->ctx, 0x053711u, 200u, data + 200, 400u) == 400, "the directory then takes the rest as one append");
    CHECK(r->write(r->ctx, 0x053711u, 200u, data + 200, 200u) == 200, "and the model needs only the hole filling");

    uint32_t lt = 0u, lh = 0u, rt = 0u, rh = 0u;
    BOTH(l->stat(l->ctx, 0x053711u, &lt, &lh), r->stat(r->ctx, 0x053711u, &rt, &rh), "stat agrees it exists");
    CHECK(lt == rt && lh == rh, "and agrees on both numbers");
    BOTH(l->complete(l->ctx, 0x053711u), r->complete(r->ctx, 0x053711u), "complete agrees");

    BOTH(l->read(l->ctx, 0x053711u, 100u, lb, 300u), r->read(r->ctx, 0x053711u, 100u, rb, 300u), "a ranged read agrees in length");
    CHECK(memcmp(lb, rb, 300u) == 0, "and in content");
    BOTH(l->read(l->ctx, 0x053711u, 500u, lb, 200u), r->read(r->ctx, 0x053711u, 500u, rb, 200u), "an over-long read is refused by both");

    /* have(), which is the one the wire path will lean on and the one most likely to drift: the
       model knows per byte, this one infers from the length, and while writes are an append the
       two must not be able to tell each other apart. */
    for (uint16_t chunk = 0; chunk < 20u; chunk++)
        BOTH(l->have(l->ctx, 0x053711u, chunk), r->have(r->ctx, 0x053711u, chunk), "have() agrees chunk by chunk");

    BOTH(l->rename(l->ctx, 0x053711u, 0x053712u), r->rename(r->ctx, 0x053711u, 0x053712u), "rename agrees");
    BOTH(l->stat(l->ctx, 0x053711u, NULL, NULL), r->stat(r->ctx, 0x053711u, NULL, NULL), "the old name is gone from both");
    BOTH(l->stat(l->ctx, 0x053712u, NULL, NULL), r->stat(r->ctx, 0x053712u, NULL, NULL), "and the new one is present in both");

    /* A PARTIAL file, which is the state the two could most easily disagree about: the model has a
       per-byte map and this one has a length, and a half-written file is where that shows. */
    BOTH(l->create(l->ctx, 0x053713u, 500u), r->create(r->ctx, 0x053713u, 500u), "a second file is created in both");
    BOTH(l->write(l->ctx, 0x053713u, 0u, data, 100u), r->write(r->ctx, 0x053713u, 0u, data, 100u), "and partly written");
    BOTH(l->stat(l->ctx, 0x053713u, &lt, &lh), r->stat(r->ctx, 0x053713u, &rt, &rh), "stat agrees on a partial file");
    CHECK(lt == rt && lh == rh, "including that it holds 100 of 500");
    BOTH(l->complete(l->ctx, 0x053713u), r->complete(r->ctx, 0x053713u), "and both say it is NOT complete");
    for (uint16_t chunk = 0; chunk < 20u; chunk++)
        BOTH(l->have(l->ctx, 0x053713u, chunk), r->have(r->ctx, 0x053713u, chunk), "have() agrees on a partial file too");

    l->destroy(l->ctx, 0x053712u);
    r->destroy(r->ctx, 0x053712u);
    BOTH(l->stat(l->ctx, 0x053712u, NULL, NULL), r->stat(r->ctx, 0x053712u, NULL, NULL), "destroy agrees");

#undef BOTH
    filestore_linux_close(&lfs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

/* And the module above the seam drives it, which is the only thing that proves the vtable was
   filled in the right order -- a transposed pair of function pointers is invisible to everything
   above except an insert that goes wrong in a confusing way. */
static void test_the_module_drives_it(void) {
    printf("\nand the content module itself runs on a directory\n");
    filestore_linux_t fs;
    const d_filestore_t *const be = rig(&fs, 0u);
    iotdata_node_content_t c;
    iotdata_node_content_init(&c, be);
    CHECK(iotdata_node_content_begin(&c, 1000u) == 0, "a fresh store offers nothing");

    uint8_t data[700];
    fill(data, sizeof(data), 59u);
    uint64_t crc = 0u;
    for (uint32_t i = 0; i < sizeof(data); i++)
        crc = _iotdata_crc64(&data[i], 1u, crc);

    CHECK(iotdata_node_content_insert_begin(&c, 0x053721u, sizeof(data), crc, 1000u) == IOTDATA_NODE_CONTENT_OK, "an insert starts");
    for (uint32_t off = 0; off < sizeof(data); off += 100u)
        CHECK(iotdata_node_content_insert_write(&c, off, data + off, 100u, 1000u) == IOTDATA_NODE_CONTENT_OK, "and its pieces are taken");
    CHECK(iotdata_node_content_tick(&c, 1000u) == IOTDATA_NODE_CONTENT_EVENT_ARRIVED, "and it arrives, checksum and all");
    CHECK(iotdata_node_content_count(&c) == 1u, "leaving one file");

    uint32_t total = 0u, len = 0u;
    uint64_t back = 0u;
    CHECK(iotdata_node_content_checksum(&c, 0x053721u, &back, &len) == IOTDATA_NODE_CONTENT_OK, "which sums from the store");
    CHECK(back == crc && len == sizeof(data), "to the same number the sender sent");
    CHECK(iotdata_node_content_stat(&c, 0x053721u, &total, NULL) && total == sizeof(data), "at the right length");

    /* The scratch id the insert used must not be left behind as content. */
    uint32_t cursor = 0u, id = 0u, n = 0u;
    while (be->iterate(be->ctx, &cursor, &id)) {
        n++;
        CHECK(id == 0x053721u, "and the only file on the disk is the real one");
    }
    CHECK(n == 1u, "with no scratch left over");

    CHECK(iotdata_node_content_delete(&c, 0x053721u) == IOTDATA_NODE_CONTENT_OK, "it can be deleted");
    CHECK(iotdata_node_content_delete(&c, 0x053721u) == IOTDATA_NODE_CONTENT_ERR_ABSENT, "and is then absent, not malformed");
    iotdata_node_content_end(&c);
    filestore_linux_close(&fs);
    rmrf(g_dir);
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    (void)snprintf(g_dir, sizeof(g_dir), "/tmp/iotdata_filestore_test_%ld", (long)getpid());
    printf("test_node_filestore_linux: a content store in a directory\n");
    test_open();
    test_roundtrip();
    test_append_only();
    test_rename_is_atomic();
    test_walk();
    test_the_sidecar();
    test_budget();
    test_survives_reopen();
    test_agrees_with_the_model();
    test_the_module_drives_it();
    rmrf(g_dir);
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
