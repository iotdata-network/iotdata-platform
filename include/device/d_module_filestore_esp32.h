#ifndef D_MODULE_FILESTORE_ESP32_H
#define D_MODULE_FILESTORE_ESP32_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_filestore_esp32.h - a content backend in SPIFFS, on its own partition.
//
// One file per content id, named as the id in six hex digits, in one flat namespace.
//
// WRITES ARE AN APPEND, AND THAT IS THE MODULE'S RULE NOT A SHORTCUT
//
// write() accepts an offset equal to what the file already holds and refuses anything else. That is
// not a simplification of §9: iotdata_node_content_insert_write() checks the same thing one layer
// up, because a dropped line that was silently filled with a hole is a file of the right length and
// the wrong content.
//
// Out-of-order arrival is the WIRE path's problem, and it brings its own answer -- §10's high-water
// mark plus a short hole list, which has to be persisted to survive a reboot mid-transfer. There is
// deliberately none of that here yet: have() answers from the length, which is exactly right while
// every byte before the end is present, and will be a lie the moment chunks arrive out of order.
// Whoever adds the transfer adds the map, and the host harness's exact per-byte backend is there to
// be compared against when they do.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_spiffs.h"
#pragma GCC diagnostic pop

#include "d_module_filestore.h"

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef FILESTORE_ESP32_LABEL
#define FILESTORE_ESP32_LABEL "storage" /* the partition, by its name in partitions.csv */
#endif

#ifndef FILESTORE_ESP32_BASE
#define FILESTORE_ESP32_BASE "/content"
#endif

/* WHAT FRACTION OF esp_spiffs_info()'s `total` CAN ACTUALLY HOLD A FILE, as a percentage.
 *
 * Not a safety margin, and not tuning: SPIFFS reports a `total` it will not let you fill, and the
 * shortfall is large and repeatable. Measured on a 'storage' partition that reports 956561 bytes:
 *
 *     6 x 120000 bytes, written in turn   refused after 728904 bytes   76.2% of total
 *     1 x 800000 bytes into an empty one  refused after 733824 bytes   76.7% of total
 *
 * The second case is the one that settles it. An empty volume and a single sequential file hit the
 * same wall as six files did, so this is not fragmentation and cannot be garbage-collected away --
 * it is a fixed reserve inside SPIFFS that `total` counts and a write can never reach.
 *
 * WHY IT MATTERS MORE THAN IT LOOKS. The content store's one job is to hold a firmware bundle,
 * which is the largest thing it will ever be given; insert_begin() refuses an upload it has no room
 * for, and it does so by asking space(). With the raw figure the arithmetic said yes, the host spent
 * three minutes sending 384KB over a 4.7KB/s console, and the write failed at 93% -- the worst
 * possible place, because every byte was paid for. Reporting the truth turns that into an immediate
 * refusal with a number the operator can act on.
 *
 * 75 leaves about 11KB of margin under the lower measurement. Raise it only with the same experiment
 * run on the geometry in question: the reserve is a property of the SPIFFS build and page size, not
 * of this code. */
#ifndef FILESTORE_ESP32_USABLE_PERCENT
#define FILESTORE_ESP32_USABLE_PERCENT 75u
#endif

#ifndef FILESTORE_ESP32_FILES_MAX
#define FILESTORE_ESP32_FILES_MAX 4 /* VFS descriptors: a writer, a reader, and room to spare */
#endif

#define FILESTORE_ESP32_NAME_LEN 6 /* six hex digits: §4's 24-bit id, exactly */

#define FILESTORE_ESP32_PATH_MAX (sizeof(FILESTORE_ESP32_BASE) + FILESTORE_ESP32_NAME_LEN + 6)

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_filestore = "device-filestore";

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool mounted;
    d_filestore_t backend;

    FILE *wr; /* the insert in flight, and its intended size */
    uint32_t wr_id, wr_total, wr_held;

    FILE *rd; /* the most recently read file, kept open for the next range of it */
    uint32_t rd_id;

    DIR *dir; /* a walk in progress, and the cursor it is positioned at */
    uint32_t dir_at;

    uint32_t stat_create, stat_destroy, stat_rename;
    uint32_t stat_write_fail, stat_read_fail, stat_create_fail;
} filestore_esp32_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline char *_filestore_esp32_path(const uint32_t id, char *const out, const size_t size) {
    return snprintf_inline(out, size, FILESTORE_ESP32_BASE "/%06lx", (unsigned long)(id & 0xFFFFFFu));
}

static inline char *_filestore_esp32_meta_path(const uint32_t id, char *const out, const size_t size) {
    return snprintf_inline(out, size, FILESTORE_ESP32_BASE "/%06lx.m", (unsigned long)(id & 0xFFFFFFu));
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_id(const char *const name, uint32_t *const id) {
    if (name == NULL || strlen(name) != (size_t)FILESTORE_ESP32_NAME_LEN)
        return false;
    uint32_t v = 0u;
    for (int i = 0; i < FILESTORE_ESP32_NAME_LEN; i++) {
        const char c = name[i];
        const uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : ((c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10) : 0xFFu);
        if (d == 0xFFu)
            return false;
        v = (v << 4) | d;
    }
    *id = v;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_esp32_wr_close(filestore_esp32_t *const fs) {
    if (fs->wr != NULL) {
        (void)fclose(fs->wr);
        fs->wr = NULL;
        fs->wr_id = fs->wr_total = fs->wr_held = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_esp32_rd_close(filestore_esp32_t *const fs) {
    if (fs->rd != NULL) {
        (void)fclose(fs->rd);
        fs->rd = NULL;
        fs->rd_id = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_esp32_dr_close(filestore_esp32_t *const fs) {
    if (fs->dir != NULL) {
        (void)closedir(fs->dir);
        fs->dir = NULL;
        fs->dir_at = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_esp32_release(filestore_esp32_t *const fs, const uint32_t id) {
    if (fs->wr != NULL && fs->wr_id == id)
        _filestore_esp32_wr_close(fs);
    if (fs->rd != NULL && fs->rd_id == id)
        _filestore_esp32_rd_close(fs);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_size(const uint32_t id, uint32_t *const size) {
    char path[FILESTORE_ESP32_PATH_MAX];
    struct stat st;
    if (stat(_filestore_esp32_path(id, path, sizeof(path)), &st) != 0)
        return false;
    *size = (st.st_size > 0) ? (uint32_t)st.st_size : 0u;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_create(void *const ctx, const uint32_t id, const uint32_t total) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return false;
    _filestore_esp32_release(fs, id);
    _filestore_esp32_wr_close(fs); /* only one insert at a time, so an older writer is finished with */
    char path[FILESTORE_ESP32_PATH_MAX];
    (void)unlink(_filestore_esp32_meta_path(id, path, sizeof(path)));
    /* "wb" truncates, which is the create-over-an-existing-id case and wants no separate unlink. */
    if ((fs->wr = fopen(_filestore_esp32_path(id, path, sizeof(path)), "wb")) == NULL) {
        ESP_LOGE(__tag_device_filestore, "create: fopen(%s): %s", path, strerror(errno));
        fs->stat_create_fail++;
        return false;
    }
    fs->wr_id = id;
    fs->wr_total = total;
    fs->wr_held = 0u;
    fs->stat_create++;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _filestore_esp32_write(void *const ctx, const uint32_t id, const uint32_t off, const void *const buf, const size_t len) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted || fs->wr == NULL || fs->wr_id != id || buf == NULL)
        return -1;
    if (off != fs->wr_held) /* an append, or nothing: see the header */
        return -1;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)fs->wr_total)
        return -1;
    if (len == 0u)
        return 0;
    const size_t n = fwrite(buf, 1u, len, fs->wr);
    if (n != len) {
        ESP_LOGE(__tag_device_filestore, "write: %06lx at %lu: %u of %u", (unsigned long)id, (unsigned long)off, (unsigned)n, (unsigned)len);
        fs->stat_write_fail++;
        fs->wr_held += (uint32_t)n; /* what did land, so a retry of the rest is still an append */
        return -1;
    }
    fs->wr_held += (uint32_t)len;
    return (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _filestore_esp32_read(void *const ctx, const uint32_t id, const uint32_t off, void *const buf, const size_t len) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted || buf == NULL)
        return -1;
    if (len == 0u)
        return 0;
    /* The file being written is readable, but only once what stdio is holding has gone down. */
    if (fs->wr != NULL && fs->wr_id == id) {
        (void)fflush(fs->wr);
        (void)fsync(fileno(fs->wr));
    }
    if (fs->rd != NULL && fs->rd_id != id)
        _filestore_esp32_rd_close(fs);
    if (fs->rd == NULL) {
        char path[FILESTORE_ESP32_PATH_MAX];
        if ((fs->rd = fopen(_filestore_esp32_path(id, path, sizeof(path)), "rb")) == NULL) {
            fs->stat_read_fail++;
            return -1;
        }
        fs->rd_id = id;
    }
    if (fseek(fs->rd, (long)off, SEEK_SET) != 0) {
        fs->stat_read_fail++;
        return -1;
    }
    const size_t n = fread(buf, 1u, len, fs->rd);
    if (n != len) {
        fs->stat_read_fail++;
        return -1; /* a short read is a refusal: a partial range read back is how verification passes by accident */
    }
    return (int)n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_esp32_destroy(void *const ctx, const uint32_t id) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return;
    _filestore_esp32_release(fs, id);
    _filestore_esp32_dr_close(fs); /* a walk holding this entry is no longer a walk of what is here */
    char path[FILESTORE_ESP32_PATH_MAX];
    (void)unlink(_filestore_esp32_meta_path(id, path, sizeof(path)));
    if (unlink(_filestore_esp32_path(id, path, sizeof(path))) == 0)
        fs->stat_destroy++;
}

static inline int _filestore_esp32_meta_write(void *const ctx, const uint32_t id, const void *const buf, const size_t len) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted || len > (size_t)D_FILESTORE_META_MAX)
        return -1;
    char path[FILESTORE_ESP32_PATH_MAX], tmp[FILESTORE_ESP32_PATH_MAX + 2];
    (void)_filestore_esp32_meta_path(id, path, sizeof(path));
    if (snprintf(tmp, sizeof(tmp), "%s.t", path) >= (int)sizeof(tmp))
        return -1;
    if (len == 0u) {
        (void)unlink(path);
        return 0;
    }
    FILE *const f = fopen(tmp, "wb");
    if (f == NULL)
        return -1;
    const size_t n = fwrite(buf, 1u, len, f);
    const bool ok = (n == len) && (fflush(f) == 0);
    (void)fclose(f);
    (void)unlink(path); /* SPIFFS will not rename over an existing name */
    if (!ok || rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    return (int)len;
}

/* The bytes, or 0 when there is no sidecar -- which is not an error: most content has none. */
static inline int _filestore_esp32_meta_read(void *const ctx, const uint32_t id, void *const buf, const size_t len) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted || buf == NULL)
        return -1;
    char path[FILESTORE_ESP32_PATH_MAX];
    FILE *const f = fopen(_filestore_esp32_meta_path(id, path, sizeof(path)), "rb");
    if (f == NULL)
        return 0;
    const size_t n = fread(buf, 1u, len, f);
    (void)fclose(f);
    return (int)n;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_rename(void *const ctx, const uint32_t from, const uint32_t to) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return false;
    _filestore_esp32_release(fs, from);
    _filestore_esp32_release(fs, to);
    _filestore_esp32_dr_close(fs);
    char a[FILESTORE_ESP32_PATH_MAX], b[FILESTORE_ESP32_PATH_MAX];
    /* The destination is removed first. SPIFFS's rename refuses an existing target, and the
       module has already decided the replacement is legitimate -- it destroys the old id itself
       before calling this, so this is belt and braces rather than policy. */
    (void)unlink(_filestore_esp32_path(to, b, sizeof(b)));
    if (rename(_filestore_esp32_path(from, a, sizeof(a)), b) != 0) {
        ESP_LOGE(__tag_device_filestore, "rename: %06lx -> %06lx: %s", (unsigned long)from, (unsigned long)to, strerror(errno));
        return false;
    }
    fs->stat_rename++;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Walks from *cursor == 0. The directory handle is kept between calls because readdir has no seek,
   and it is reopened and skipped forward whenever the cursor is not where the handle is -- a walk
   that was abandoned halfway, or one that something destroyed an entry under. So an abandoned walk
   costs a rewind rather than a leak, and no caller has to promise to finish. */
static inline bool _filestore_esp32_iterate(void *const ctx, uint32_t *const cursor, uint32_t *const id) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted || cursor == NULL || id == NULL)
        return false;
    if (fs->dir == NULL || fs->dir_at != *cursor) {
        _filestore_esp32_dr_close(fs);
        if ((fs->dir = opendir(FILESTORE_ESP32_BASE)) == NULL)
            return false;
        for (uint32_t skip = 0u; skip < *cursor; skip++)
            if (readdir(fs->dir) == NULL) {
                _filestore_esp32_dr_close(fs);
                return false;
            }
        fs->dir_at = *cursor;
    }
    for (const struct dirent *e = readdir(fs->dir); e != NULL; e = readdir(fs->dir)) {
        fs->dir_at++;
        *cursor = fs->dir_at;
        if (_filestore_esp32_id(e->d_name, id))
            return true; /* anything else on the partition is not content, and is left alone */
    }
    _filestore_esp32_dr_close(fs);
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_stat(void *const ctx, const uint32_t id, uint32_t *const total, uint32_t *const held) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return false;
    /* The one in flight answers from RAM: its intended size is not on the partition, and its length
       there trails whatever stdio has not flushed. */
    if (fs->wr != NULL && fs->wr_id == id) {
        if (total != NULL)
            *total = fs->wr_total;
        if (held != NULL)
            *held = fs->wr_held;
        return true;
    }
    uint32_t size = 0u;
    if (!_filestore_esp32_size(id, &size))
        return false;
    if (total != NULL)
        *total = size; /* finished, so intended and actual are the same number */
    if (held != NULL)
        *held = size;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* WHAT SPIFFS SAYS IS FREE IS AN UPPER BOUND AND NOT A PROMISE, and the gap is not small. On a
   1 MiB partition already holding three 200 KB files, `esp_spiffs_info` reported 322 KB free and a
   145 KB write ran out of room at 113 KB -- SPIFFS is log-structured and needs whole free blocks it
   can erase, so a nearly-full partition loses the use of a third of what it is counting.

   It is still worth asking. The module uses this to refuse an obviously impossible insert BEFORE
   erasing anything or taking a scratch id, which is cheap and catches the common case; a write that
   fails anyway fails the way every other store failure does, and the test for that path is a real
   one that happened. Nothing here pretends otherwise, and the module's comment on insert_begin does
   not call this a guarantee. */
static inline uint32_t _filestore_esp32_space(void *const ctx) {
    const filestore_esp32_t *const fs = (const filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return 0u;
    size_t total = 0u, used = 0u;
    if (esp_spiffs_info(FILESTORE_ESP32_LABEL, &total, &used) != ESP_OK || used > total)
        return 0u;
    /* THE BUDGET, NOT THE DIFFERENCE. total - used is what SPIFFS says is free and not what it will
       let us write; see FILESTORE_ESP32_USABLE_PERCENT for the measurement. */
    const uint64_t usable = (uint64_t)total * (uint64_t)FILESTORE_ESP32_USABLE_PERCENT / 100u;
    return ((uint64_t)used >= usable) ? 0u : (uint32_t)(usable - (uint64_t)used);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_esp32_have(void *const ctx, const uint32_t id, const uint16_t chunk) {
    uint32_t total = 0u, held = 0u;
    if (!_filestore_esp32_stat(ctx, id, &total, &held))
        return false;
    /* From the length, which is the truth only while writes are an append: see the header. */
    return ((uint64_t)chunk + 1u) * D_FILESTORE_CHUNK_BYTES <= (uint64_t)held || ((uint64_t)chunk * D_FILESTORE_CHUNK_BYTES < (uint64_t)held && held == total);
}

// ------------------------------------------------------------------------------------------------------------------------

/* IS IT ALL THERE -- and if it is, the handle goes down so the bytes are on flash rather than in
   stdio. The order matters and cost a bug: closing FIRST and asking afterwards always answers yes,
   because a closed file's length is the only size there is any longer and so `held == total`
   trivially. A half-written insert would have reported itself complete. The intended size lives
   only in RAM beside the open writer, so it has to be read while that writer is still open.
   Found on the host, by d_module_filestore_linux.h's suite driving this backend's twin side by
   side with the exact RAM model -- which is the entire argument for that model existing. */
static inline bool _filestore_esp32_complete(void *const ctx, const uint32_t id) {
    filestore_esp32_t *const fs = (filestore_esp32_t *)ctx;
    if (fs == NULL || !fs->mounted)
        return false;
    uint32_t total = 0u, held = 0u;
    if (!_filestore_esp32_stat(ctx, id, &total, &held) || held != total)
        return false;
    if (fs->wr != NULL && fs->wr_id == id) /* complete, so the writer is finished with */
        _filestore_esp32_wr_close(fs);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline const d_filestore_t *filestore_esp32_mount(filestore_esp32_t *const fs) {
    if (fs == NULL)
        return NULL;
    *fs = (filestore_esp32_t){ 0 };
    const esp_err_t err = esp_vfs_spiffs_register(&(const esp_vfs_spiffs_conf_t){
        .base_path = FILESTORE_ESP32_BASE,
        .partition_label = FILESTORE_ESP32_LABEL,
        .max_files = FILESTORE_ESP32_FILES_MAX,
        .format_if_mount_failed = true,
    });
    if (err != ESP_OK) {
        ESP_LOGE(__tag_device_filestore, "mount: %s on '%s': %s", FILESTORE_ESP32_BASE, FILESTORE_ESP32_LABEL, esp_err_to_name(err));
        return NULL;
    }
    fs->mounted = true;
    fs->backend = (const d_filestore_t){
        .ctx = fs,
        .create = _filestore_esp32_create,
        .write = _filestore_esp32_write,
        .read = _filestore_esp32_read,
        .destroy = _filestore_esp32_destroy,
        .rename = _filestore_esp32_rename,
        .iterate = _filestore_esp32_iterate,
        .stat = _filestore_esp32_stat,
        .space = _filestore_esp32_space,
        .meta_write = _filestore_esp32_meta_write,
        .meta_read = _filestore_esp32_meta_read,
        .have = _filestore_esp32_have,
        .complete = _filestore_esp32_complete,
    };
    size_t total = 0u, used = 0u;
    (void)esp_spiffs_info(FILESTORE_ESP32_LABEL, &total, &used);
    /* the budget and not `total`, so the number here and the number `content` prints agree */
    ESP_LOGI(__tag_device_filestore, "mount: %s on '%s', %u of %u bytes used (%u%% of %u is reachable)", FILESTORE_ESP32_BASE, FILESTORE_ESP32_LABEL, (unsigned)used,
             (unsigned)((uint64_t)total * (uint64_t)FILESTORE_ESP32_USABLE_PERCENT / 100u), (unsigned)FILESTORE_ESP32_USABLE_PERCENT, (unsigned)total);
    return &fs->backend;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void filestore_esp32_unmount(filestore_esp32_t *const fs) {
    if (fs == NULL || !fs->mounted)
        return;
    _filestore_esp32_wr_close(fs);
    _filestore_esp32_rd_close(fs);
    _filestore_esp32_dr_close(fs);
    (void)esp_vfs_spiffs_unregister(FILESTORE_ESP32_LABEL);
    fs->mounted = false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filestore_esp32_info(const filestore_esp32_t *const fs, uint32_t *const total, uint32_t *const used) {
    if (fs == NULL || !fs->mounted)
        return false;
    size_t t = 0u, u = 0u;
    if (esp_spiffs_info(FILESTORE_ESP32_LABEL, &t, &u) != ESP_OK)
        return false;
    if (total != NULL)
        *total = (uint32_t)t;
    if (used != NULL)
        *used = (uint32_t)u;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filestore_esp32_format(filestore_esp32_t *const fs) {
    if (fs == NULL || !fs->mounted)
        return false;
    _filestore_esp32_wr_close(fs);
    _filestore_esp32_rd_close(fs);
    _filestore_esp32_dr_close(fs);
    const esp_err_t err = esp_spiffs_format(FILESTORE_ESP32_LABEL);
    if (err != ESP_OK)
        ESP_LOGE(__tag_device_filestore, "format: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_FILESTORE_ESP32_H */
