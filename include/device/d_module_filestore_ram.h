#ifndef D_MODULE_FILESTORE_RAM_H
#define D_MODULE_FILESTORE_RAM_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_filestore_ram.h - a content backend in RAM. The HARNESS's half of the seam that
// d_module_filestore_esp32.h fills with SPIFFS and d_module_filestore_linux.h with a directory.
//
// WHY A FILESTORE AND NOT A DATASTORE, since d_module_datastore_* already exists. A datastore is
// whole blobs by key: set it, get it, atomic by construction. A filestore is BYTE RANGES by id --
// writeable at an offset, readable back at another, and knowing which parts of itself it has. The
// two are not variations on each other and a content transfer needs the second.
//
// ------------------------------------------------------------------------------------------------------------------------
// IT IS EXACT, WHICH THE REAL BACKENDS WILL NOT BE
//
// This one keeps a per-BYTE written map, so have() and the held count are exactly right even for
// writes that arrive out of order or overlap. That is affordable here and nowhere else: a 4 KB
// slot costs 512 bytes of map, where a 200 KB blob in flash would cost 25 KB.
//
// So it is deliberately the STRICTEST backend, not the most representative one. A device backend
// will track a high-water mark and a short list of holes (§10), and the point of being exact here
// is that when the wire path lands, a disagreement between this and a device backend is a bug in
// the device backend rather than an ambiguity in the seam.
//
// FAILURE INJECTION is part of its job. A store that only ever succeeds leaves the module's
// ERR_STORE paths unreachable, so create, write and rename can each be told to refuse once.
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include "d_module_filestore.h"

// ------------------------------------------------------------------------------------------------------------------------

#ifndef FILESTORE_RAM_SLOTS
#define FILESTORE_RAM_SLOTS 8
#endif
#ifndef FILESTORE_RAM_BYTES
#define FILESTORE_RAM_BYTES 4096u
#endif
#define FILESTORE_RAM_MAP_BYTES ((FILESTORE_RAM_BYTES + 7u) / 8u)

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool used;
    uint32_t id;
    uint32_t total; /* what create() was told to expect */
    uint8_t data[FILESTORE_RAM_BYTES];
    uint8_t map[FILESTORE_RAM_MAP_BYTES]; /* one bit per byte: written, or not yet */
    /* The sidecar, in the slot rather than beside it -- there is no directory here. It lives with
       the content for the same reason it does on the other two stores: destroying the slot takes
       it, so a description cannot outlive the bytes it describes. */
    uint8_t meta[D_FILESTORE_META_MAX];
    uint8_t meta_len;
} filestore_ram_slot_t;

typedef struct {
    filestore_ram_slot_t slot[FILESTORE_RAM_SLOTS];
    d_filestore_t backend;
    /* one-shot refusals, so the module's store-failure paths are reachable from a test */
    bool fail_create, fail_write, fail_rename;
    /* what was asked of it, for a test to assert on rather than infer from side effects */
    uint32_t stat_create, stat_write, stat_read, stat_destroy, stat_rename, stat_complete, stat_meta_write, stat_meta_read;
    uint32_t capacity; /* what space() reports; 0 means "as much as a free slot holds" */
} filestore_ram_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline filestore_ram_slot_t *_filestore_ram_find(filestore_ram_t *const fs, const uint32_t id) {
    for (int i = 0; i < (int)(sizeof(fs->slot) / sizeof(fs->slot[0])); i++)
        if (fs->slot[i].used && fs->slot[i].id == id)
            return &fs->slot[i];
    return NULL;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline uint32_t _filestore_ram_held(const filestore_ram_slot_t *const s) {
    uint32_t n = 0u;
    for (uint32_t i = 0; i < s->total && i < FILESTORE_RAM_BYTES; i++)
        if ((s->map[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0u)
            n++;
    return n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_create(void *const ctx, const uint32_t id, const uint32_t total) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_create++;
    if (fs->fail_create) {
        fs->fail_create = false;
        return false;
    }
    if (total == 0u || total > FILESTORE_RAM_BYTES)
        return false;
    filestore_ram_slot_t *s = _filestore_ram_find(fs, id);
    if (s == NULL)
        for (int i = 0; i < (int)(sizeof(fs->slot) / sizeof(fs->slot[0])); i++)
            if (!fs->slot[i].used) {
                s = &fs->slot[i];
                break;
            }
    if (s == NULL)
        return false;
    *s = (filestore_ram_slot_t){ 0 };
    s->used = true;
    s->id = id;
    s->total = total;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _filestore_ram_write(void *const ctx, const uint32_t id, const uint32_t off, const void *const buf, const size_t len) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_write++;
    if (fs->fail_write) {
        fs->fail_write = false;
        return -1;
    }
    filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s == NULL || buf == NULL)
        return -1;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)s->total)
        return -1;
    memcpy(&s->data[off], buf, len);
    for (uint32_t i = off; i < off + (uint32_t)len; i++)
        s->map[i >> 3] |= (uint8_t)(1u << (i & 7u));
    return (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _filestore_ram_read(void *const ctx, const uint32_t id, const uint32_t off, void *const buf, const size_t len) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_read++;
    const filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s == NULL || buf == NULL)
        return -1;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)s->total)
        return -1;
    for (uint32_t i = off; i < off + (uint32_t)len; i++)
        if ((s->map[i >> 3] & (uint8_t)(1u << (i & 7u))) == 0u)
            return -1;
    memcpy(buf, &s->data[off], len);
    return (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_ram_destroy(void *const ctx, const uint32_t id) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_destroy++;
    filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s != NULL)
        *s = (filestore_ram_slot_t){ 0 };
}

// ------------------------------------------------------------------------------------------------------------------------

/* `len` 0 removes it, which is how a description is withdrawn without touching the bytes. */
static inline int _filestore_ram_meta_write(void *const ctx, const uint32_t id, const void *const buf, const size_t len) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s == NULL || len > sizeof(s->meta))
        return -1;
    if (len > 0u && buf == NULL)
        return -1;
    if (len > 0u)
        memcpy(s->meta, buf, len);
    s->meta_len = (uint8_t)len;
    fs->stat_meta_write++;
    return (int)len;
}

/* The bytes, or 0 when there is no sidecar -- which is not an error: most content has none. */
static inline int _filestore_ram_meta_read(void *const ctx, const uint32_t id, void *const buf, const size_t len) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    const filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    fs->stat_meta_read++;
    if (s == NULL || buf == NULL)
        return (s == NULL) ? 0 : -1;
    const size_t n = (s->meta_len < len) ? (size_t)s->meta_len : len;
    memcpy(buf, s->meta, n);
    return (int)n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_rename(void *const ctx, const uint32_t from, const uint32_t to) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_rename++;
    if (fs->fail_rename) {
        fs->fail_rename = false;
        return false;
    }
    filestore_ram_slot_t *const s = _filestore_ram_find(fs, from);
    if (s == NULL || from == to)
        return false;
    filestore_ram_slot_t *const dst = _filestore_ram_find(fs, to);
    if (dst != NULL)
        *dst = (filestore_ram_slot_t){ 0 };
    s->id = to;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_iterate(void *const ctx, uint32_t *const cursor, uint32_t *const id) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    for (uint32_t i = *cursor; i < (uint32_t)(sizeof(fs->slot) / sizeof(fs->slot[0])); i++)
        if (fs->slot[i].used) {
            *id = fs->slot[i].id;
            *cursor = i + 1u;
            return true;
        }
    *cursor = (uint32_t)(sizeof(fs->slot) / sizeof(fs->slot[0]));
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_stat(void *const ctx, const uint32_t id, uint32_t *const total, uint32_t *const held) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    const filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s == NULL)
        return false;
    if (total != NULL)
        *total = s->total;
    if (held != NULL)
        *held = _filestore_ram_held(s);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline uint32_t _filestore_ram_space(void *const ctx) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    if (fs->capacity != 0u)
        return fs->capacity;
    for (int i = 0; i < (int)(sizeof(fs->slot) / sizeof(fs->slot[0])); i++)
        if (!fs->slot[i].used)
            return FILESTORE_RAM_BYTES;
    return 0u;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_have(void *const ctx, const uint32_t id, const uint16_t chunk) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    const filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    if (s == NULL)
        return false;
    const uint32_t from = (uint32_t)chunk * D_FILESTORE_CHUNK_BYTES;
    if (from >= s->total)
        return false;
    uint32_t to = from + D_FILESTORE_CHUNK_BYTES;
    if (to > s->total)
        to = s->total;
    for (uint32_t i = from; i < to; i++)
        if ((s->map[i >> 3] & (uint8_t)(1u << (i & 7u))) == 0u)
            return false;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_ram_complete(void *const ctx, const uint32_t id) {
    filestore_ram_t *const fs = (filestore_ram_t *)ctx;
    fs->stat_complete++;
    const filestore_ram_slot_t *const s = _filestore_ram_find(fs, id);
    return s != NULL && _filestore_ram_held(s) == s->total;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const d_filestore_t *filestore_ram_init(filestore_ram_t *const fs) {
    if (fs == NULL)
        return NULL;
    *fs = (filestore_ram_t){ 0 };
    fs->backend = (const d_filestore_t){
        .ctx = fs,
        .create = _filestore_ram_create,
        .write = _filestore_ram_write,
        .read = _filestore_ram_read,
        .destroy = _filestore_ram_destroy,
        .rename = _filestore_ram_rename,
        .iterate = _filestore_ram_iterate,
        .stat = _filestore_ram_stat,
        .space = _filestore_ram_space,
        .have = _filestore_ram_have,
        .complete = _filestore_ram_complete,
        .meta_write = _filestore_ram_meta_write,
        .meta_read = _filestore_ram_meta_read,
    };
    return &fs->backend;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Put a file there directly, as a factory image would -- no insert, no transfer. This is the shape
   the baked-in calibration blob case takes, and a test needs it to reach cold-boot discovery. */
static inline bool filestore_ram_plant(filestore_ram_t *const fs, const uint32_t id, const void *const data, const uint32_t len) {
    if (fs == NULL || !_filestore_ram_create(fs, id, len))
        return false;
    return _filestore_ram_write(fs, id, 0u, data, len) == (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_FILESTORE_RAM_H */
