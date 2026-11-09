#ifndef D_MODULE_FILESTORE_LINUX_H
#define D_MODULE_FILESTORE_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_filestore_linux.h - a content backend in a directory. The LINUX half of the seam that
// d_module_filestore_esp32.h fills with SPIFFS and d_module_filestore_ram.h stands in for.
//
// One file per content id, named as the id in six hex digits, in one flat directory.
//
// WRITES ARE AN APPEND, THE SAME RULE AS EVERY OTHER BACKEND
//
// write() accepts an offset equal to what the file already holds and refuses anything else, and
// iotdata_node_content_insert_write() checks the same thing one layer up. A dropped line silently
// filled with a hole is a file of the right length and the wrong content, which is the one failure
// a length check cannot see. Out-of-order arrival is the WIRE path's problem and brings §10's
// high-water mark plus a hole list with it; there is none of that here yet, and have() answers from
// the length, which is exactly right while every byte before the end is present.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "d_module_filestore.h"

// ------------------------------------------------------------------------------------------------------------------------

#ifndef FILESTORE_LINUX_PATH_MAX
#define FILESTORE_LINUX_PATH_MAX 256
#endif

#define FILESTORE_LINUX_NAME_LEN     6 /* six hex digits: §4's 24-bit id, exactly */

#define FILESTORE_LINUX_FULL_MAX     (FILESTORE_LINUX_PATH_MAX + FILESTORE_LINUX_NAME_LEN + 6)

/* The two filesystems that accept a write and lose it on reboot. Spelled out rather than taken from
   <linux/magic.h>, which is not present on every toolchain this builds under -- the same choice
   d_module_datastore_linux.h made, and the same two numbers. */
#define _FILESTORE_LINUX_TMPFS_MAGIC 0x01021994L
#define _FILESTORE_LINUX_RAMFS_MAGIC 0x858458f6L

/* POSIX.1-2008, hidden under a strict -std=c11 without _GNU_SOURCE. Falling back to 0 costs the
   directory fsync its O_DIRECTORY check, not the fsync itself. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool open;
    bool persistent; /* false: nothing here survives a reboot -- see the header */
    char dir[FILESTORE_LINUX_PATH_MAX];
    uint32_t budget; /* 0 = whatever the filesystem has */
    d_filestore_t backend;

    FILE *wr; /* the insert in flight, and its intended size */
    uint32_t wr_id, wr_total, wr_held;

    FILE *rd; /* the most recently read file, kept open for the next range of it */
    uint32_t rd_id;

    DIR *walk; /* a walk in progress, and the cursor it is positioned at */
    uint32_t walk_at;

    uint32_t stat_create, stat_destroy, stat_rename;
    uint32_t stat_write_fail, stat_read_fail, stat_create_fail;
} filestore_linux_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_path(const filestore_linux_t *const fs, const uint32_t id, char *const out, const size_t size) {
    return snprintf(out, size, "%s/%06lx", fs->dir, (unsigned long)(id & 0xFFFFFFu)) < (int)size;
}

static inline bool _filestore_linux_meta_path(const filestore_linux_t *const fs, const uint32_t id, char *const out, const size_t size) {
    return snprintf(out, size, "%s/%06lx.m", fs->dir, (unsigned long)(id & 0xFFFFFFu)) < (int)size;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_id(const char *const name, uint32_t *const id) {
    if (name == NULL || strlen(name) != (size_t)FILESTORE_LINUX_NAME_LEN)
        return false;
    uint32_t v = 0u;
    for (int i = 0; i < FILESTORE_LINUX_NAME_LEN; i++) {
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

static inline void _filestore_linux_wr_close(filestore_linux_t *const fs) {
    if (fs->wr != NULL) {
        (void)fclose(fs->wr);
        fs->wr = NULL;
        fs->wr_id = fs->wr_total = fs->wr_held = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_linux_rd_close(filestore_linux_t *const fs) {
    if (fs->rd != NULL) {
        (void)fclose(fs->rd);
        fs->rd = NULL;
        fs->rd_id = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_linux_walk_close(filestore_linux_t *const fs) {
    if (fs->walk != NULL) {
        (void)closedir(fs->walk);
        fs->walk = NULL;
        fs->walk_at = 0u;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_linux_release(filestore_linux_t *const fs, const uint32_t id) {
    if (fs->wr != NULL && fs->wr_id == id)
        _filestore_linux_wr_close(fs);
    if (fs->rd != NULL && fs->rd_id == id)
        _filestore_linux_rd_close(fs);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void _filestore_linux_dr_sync(const filestore_linux_t *const fs) {
    const int fd = open(fs->dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_size(const filestore_linux_t *const fs, const uint32_t id, uint32_t *const size) {
    char path[FILESTORE_LINUX_FULL_MAX];
    struct stat st;
    if (!_filestore_linux_path(fs, id, path, sizeof(path)) || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    *size = (st.st_size > 0) ? (uint32_t)st.st_size : 0u;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_create(void *const ctx, const uint32_t id, const uint32_t total) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open)
        return false;
    _filestore_linux_release(fs, id);
    _filestore_linux_wr_close(fs); /* one insert at a time, so an older writer is finished with */
    char path[FILESTORE_LINUX_FULL_MAX];
    /* "wb" truncates, which is the create-over-an-existing-id case and needs no separate unlink. */
    if (_filestore_linux_meta_path(fs, id, path, sizeof(path)))
        (void)unlink(path);
    if (!_filestore_linux_path(fs, id, path, sizeof(path)) || (fs->wr = fopen(path, "wb")) == NULL) {
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

static inline int _filestore_linux_write(void *const ctx, const uint32_t id, const uint32_t off, const void *const buf, const size_t len) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || fs->wr == NULL || fs->wr_id != id || buf == NULL)
        return -1;
    if (off != fs->wr_held) /* an append, or nothing: see the header */
        return -1;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)fs->wr_total)
        return -1;
    if (len == 0u)
        return 0;
    const size_t n = fwrite(buf, 1u, len, fs->wr);
    if (n != len) {
        fs->stat_write_fail++;
        fs->wr_held += (uint32_t)n; /* what did land, so a retry of the rest is still an append */
        return -1;
    }
    fs->wr_held += (uint32_t)len;
    return (int)len;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline int _filestore_linux_read(void *const ctx, const uint32_t id, const uint32_t off, void *const buf, const size_t len) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || buf == NULL)
        return -1;
    if (len == 0u)
        return 0;
    /* The file being written is readable, but only once what stdio is holding has gone down. */
    if (fs->wr != NULL && fs->wr_id == id)
        (void)fflush(fs->wr);
    if (fs->rd != NULL && fs->rd_id != id)
        _filestore_linux_rd_close(fs);
    if (fs->rd == NULL) {
        char path[FILESTORE_LINUX_FULL_MAX];
        if (!_filestore_linux_path(fs, id, path, sizeof(path)) || (fs->rd = fopen(path, "rb")) == NULL) {
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

static inline void _filestore_linux_destroy(void *const ctx, const uint32_t id) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open)
        return;
    _filestore_linux_release(fs, id);
    _filestore_linux_walk_close(fs); /* a walk holding this entry is no longer a walk of what is here */
    char path[FILESTORE_LINUX_FULL_MAX];
    if (_filestore_linux_meta_path(fs, id, path, sizeof(path)))
        (void)unlink(path);
    if (_filestore_linux_path(fs, id, path, sizeof(path)) && unlink(path) == 0) {
        fs->stat_destroy++;
        _filestore_linux_dr_sync(fs); /* gone, and stays gone */
    }
}

static inline int _filestore_linux_meta_write(void *const ctx, const uint32_t id, const void *const buf, const size_t len) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || len > (size_t)D_FILESTORE_META_MAX)
        return -1;
    char path[FILESTORE_LINUX_FULL_MAX], tmp[FILESTORE_LINUX_FULL_MAX];
    if (!_filestore_linux_meta_path(fs, id, path, sizeof(path)) || snprintf(tmp, sizeof(tmp), "%s.t", path) >= (int)sizeof(tmp))
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
    if (!ok || rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    return (int)len;
}

/* The bytes, or 0 when there is no sidecar -- which is not an error: most content has none. */
static inline int _filestore_linux_meta_read(void *const ctx, const uint32_t id, void *const buf, const size_t len) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || buf == NULL)
        return -1;
    char path[FILESTORE_LINUX_FULL_MAX];
    if (!_filestore_linux_meta_path(fs, id, path, sizeof(path)))
        return -1;
    FILE *const f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    const size_t n = fread(buf, 1u, len, f);
    (void)fclose(f);
    return (int)n;
}

// ------------------------------------------------------------------------------------------------------------------------

/* The scratch-to-real step, and the only place an insert becomes visible as finished content. The
   two fsyncs are what the header's DURABLE RENAME section is about: the bytes reach the disk before
   the name points at them, and the name outlives the power. */
static inline bool _filestore_linux_rename(void *const ctx, const uint32_t from, const uint32_t to) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || from == to)
        return false;
    if (fs->wr != NULL && fs->wr_id == from) { /* the usual case: the writer IS the source */
        (void)fflush(fs->wr);
        (void)fsync(fileno(fs->wr));
    }
    _filestore_linux_release(fs, from);
    _filestore_linux_release(fs, to);
    _filestore_linux_walk_close(fs);
    char a[FILESTORE_LINUX_FULL_MAX], b[FILESTORE_LINUX_FULL_MAX];
    if (!_filestore_linux_path(fs, from, a, sizeof(a)) || !_filestore_linux_path(fs, to, b, sizeof(b)))
        return false;
    /* rename(2) replaces the destination atomically, so unlike SPIFFS there is nothing to clear
       first -- and clearing it would open a window in which neither name exists. */
    if (rename(a, b) != 0)
        return false;
    _filestore_linux_dr_sync(fs);
    fs->stat_rename++;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* Walks from *cursor == 0. The handle is kept between calls because readdir has no seek, and is
   reopened and skipped forward whenever the cursor is not where the handle is -- a walk abandoned
   halfway, or one that had an entry destroyed under it. An abandoned walk costs a rewind rather
   than a leak, and no caller has to promise to finish. */
static inline bool _filestore_linux_iterate(void *const ctx, uint32_t *const cursor, uint32_t *const id) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open || cursor == NULL || id == NULL)
        return false;
    if (fs->walk == NULL || fs->walk_at != *cursor) {
        _filestore_linux_walk_close(fs);
        if ((fs->walk = opendir(fs->dir)) == NULL)
            return false;
        for (uint32_t skip = 0u; skip < *cursor; skip++)
            if (readdir(fs->walk) == NULL) {
                _filestore_linux_walk_close(fs);
                return false;
            }
        fs->walk_at = *cursor;
    }
    for (const struct dirent *e = readdir(fs->walk); e != NULL; e = readdir(fs->walk)) {
        fs->walk_at++;
        *cursor = fs->walk_at;
        /* Anything not named as an id is not content and is left alone -- which is what lets a
           content directory be a directory somebody else also keeps things in. */
        if (_filestore_linux_id(e->d_name, id))
            return true;
    }
    _filestore_linux_walk_close(fs);
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_stat(void *const ctx, const uint32_t id, uint32_t *const total, uint32_t *const held) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open)
        return false;
    /* The one in flight answers from RAM: its intended size is nowhere on the disk, and its length
       there trails whatever stdio has not flushed. */
    if (fs->wr != NULL && fs->wr_id == id) {
        if (total != NULL)
            *total = fs->wr_total;
        if (held != NULL)
            *held = fs->wr_held;
        return true;
    }
    uint32_t size = 0u;
    if (!_filestore_linux_size(fs, id, &size))
        return false;
    if (total != NULL)
        *total = size; /* finished, so intended and actual are the same number */
    if (held != NULL)
        *held = size;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

/* What this store HOLDS, which is what a budget has to be measured against. Walked rather than
   remembered: files arrive here by other means than an insert -- a bundle dropped in by hand, one
   left behind by the previous run -- and a counter kept in RAM would be wrong about all of them. */
static inline uint32_t filestore_linux_used(filestore_linux_t *const fs) {
    if (fs == NULL || !fs->open)
        return 0u;
    uint32_t cursor = 0u, id = 0u, used = 0u;
    while (_filestore_linux_iterate(fs, &cursor, &id)) {
        uint32_t size = 0u;
        if (_filestore_linux_size(fs, id, &size))
            used += size;
    }
    return used;
}

// ------------------------------------------------------------------------------------------------------------------------

/* THE SMALLER OF THE TWO LIMITS, and both are real. The budget is what this store was told it may
   have; the filesystem's free space is what it can have whatever it was told, because the disk is
   shared with everything else on the box.
 *
 * Honest in a way the SPIFFS backend's cannot be -- statvfs counts blocks, not a log it has to
 * garbage-collect -- but still only an answer about this instant. The module uses it to refuse an
 * obviously impossible insert before taking a scratch id, and a write that fails anyway fails the
 * way every other store failure does. */
static inline uint32_t _filestore_linux_space(void *const ctx) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open)
        return 0u;
    uint32_t avail = 0xFFFFFFFFu;
    struct statvfs sb;
    if (statvfs(fs->dir, &sb) == 0) {
        const uint64_t free_bytes = (uint64_t)sb.f_bavail * (uint64_t)((sb.f_frsize != 0u) ? sb.f_frsize : sb.f_bsize);
        avail = (free_bytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)free_bytes;
    }
    if (fs->budget != 0u) {
        const uint32_t used = filestore_linux_used(fs);
        const uint32_t left = (used < fs->budget) ? (fs->budget - used) : 0u;
        if (left < avail)
            avail = left;
    }
    return avail;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _filestore_linux_have(void *const ctx, const uint32_t id, const uint16_t chunk) {
    uint32_t total = 0u, held = 0u;
    if (!_filestore_linux_stat(ctx, id, &total, &held))
        return false;
    /* From the length, which is the truth only while writes are an append: see the header. */
    return ((uint64_t)chunk + 1u) * D_FILESTORE_CHUNK_BYTES <= (uint64_t)held || ((uint64_t)chunk * D_FILESTORE_CHUNK_BYTES < (uint64_t)held && held == total);
}

// ------------------------------------------------------------------------------------------------------------------------

/* IS IT ALL THERE -- and if it is, the handle goes down so the bytes are on the disk rather than in
   stdio. The order matters and cost a bug: closing FIRST and asking afterwards always answers yes,
   because a closed file's length is the only size there is any longer and so `held == total`
   trivially. A half-written insert would have reported itself complete. The intended size lives
   only in RAM beside the open writer, so it has to be read while that writer is still open. */
static inline bool _filestore_linux_complete(void *const ctx, const uint32_t id) {
    filestore_linux_t *const fs = (filestore_linux_t *)ctx;
    if (fs == NULL || !fs->open)
        return false;
    uint32_t total = 0u, held = 0u;
    if (!_filestore_linux_stat(ctx, id, &total, &held) || held != total)
        return false;
    if (fs->wr != NULL && fs->wr_id == id) { /* complete, so the writer is finished with */
        (void)fflush(fs->wr);
        (void)fsync(fileno(fs->wr));
        _filestore_linux_wr_close(fs);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* `budget` bounds what content may occupy, in bytes; 0 means the filesystem's own free space, which
   is only the right answer for a directory on a volume of its own. Returns the vtable, or NULL if
   the directory could not be made or reached -- which a caller should report and carry on from, the
   same as a partition that would not mount. */
static inline const d_filestore_t *filestore_linux_open(filestore_linux_t *const fs, const char *const location, const uint32_t budget) {
    if (fs == NULL || location == NULL)
        return NULL;
    *fs = (filestore_linux_t){ 0 };
    if (snprintf(fs->dir, sizeof(fs->dir), "%s", location) >= (int)sizeof(fs->dir))
        return NULL;
    if (mkdir(fs->dir, 0700) != 0 && errno != EEXIST)
        return NULL;
    DIR *const probe = opendir(fs->dir);
    if (probe == NULL)
        return NULL; /* it exists and we still cannot read it: not a store */
    (void)closedir(probe);
    struct statfs sb;
    /* An unknown filesystem type counts as persistent: this is a warning, and refusing to run on
       anything unrecognised would be worse than the problem it guards against. */
    fs->persistent = (statfs(fs->dir, &sb) != 0) || (sb.f_type != _FILESTORE_LINUX_TMPFS_MAGIC && sb.f_type != _FILESTORE_LINUX_RAMFS_MAGIC);
    fs->budget = budget;
    fs->open = true;
    fs->backend = (const d_filestore_t){
        .ctx = fs,
        .create = _filestore_linux_create,
        .write = _filestore_linux_write,
        .read = _filestore_linux_read,
        .destroy = _filestore_linux_destroy,
        .rename = _filestore_linux_rename,
        .iterate = _filestore_linux_iterate,
        .stat = _filestore_linux_stat,
        .space = _filestore_linux_space,
        .meta_write = _filestore_linux_meta_write,
        .meta_read = _filestore_linux_meta_read,
        .have = _filestore_linux_have,
        .complete = _filestore_linux_complete,
    };
    return &fs->backend;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void filestore_linux_close(filestore_linux_t *const fs) {
    if (fs == NULL || !fs->open)
        return;
    _filestore_linux_wr_close(fs);
    _filestore_linux_rd_close(fs);
    _filestore_linux_walk_close(fs);
    fs->open = false;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline bool filestore_linux_persistent(const filestore_linux_t *const fs) {
    return fs != NULL && fs->open && fs->persistent;
}

// ------------------------------------------------------------------------------------------------------------------------

/* For the line an application logs at startup: what is here, and what it is allowed to grow to.
   `budget` comes back as the filesystem's free space when none was set, so a caller printing it
   does not have to special-case zero. */
static inline bool filestore_linux_info(filestore_linux_t *const fs, uint32_t *const used, uint32_t *const budget) {
    if (fs == NULL || !fs->open)
        return false;
    const uint32_t u = filestore_linux_used(fs);
    if (used != NULL)
        *used = u;
    if (budget != NULL)
        *budget = (fs->budget != 0u) ? fs->budget : (u + _filestore_linux_space(fs));
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_FILESTORE_LINUX_H */
