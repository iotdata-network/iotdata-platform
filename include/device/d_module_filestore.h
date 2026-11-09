#ifndef D_MODULE_FILESTORE_H
#define D_MODULE_FILESTORE_H

// ------------------------------------------------------------------------------------------------------------------------
// d_module_filestore.h -- the interface every filestore implements, and nothing else.
//
// WHY IT IS A HEADER OF ITS OWN. The three implementations -- d_module_filestore_esp32.h (SPIFFS),
// _linux.h (a directory) and _ram.h (the host harness's) -- used to reach for this struct by
// including iotdata_node_content.h, which is the wrong direction: a d_ module must be includable
// before any iotdata header, and one that includes a node header drags the whole framework in ahead
// of the application's own choices. The struct itself never needed any of it. It is function
// pointers and integers, so it lives here, depends on nothing but the standard headers, and can be
// included first like every other d_ module.
//
// The content module is the only CONSUMER of this interface, and that is a one-way dependency: this
// header knows nothing about content, ids, inserts or consumers, and is not the place to put
// anything that does.
// ------------------------------------------------------------------------------------------------------------------------
// §9'S SEAM, PLUS A CONTEXT AND THREE ADDITIONS
//
// §9 wrote these as bare functions. They take a ctx here, because every other store in this tree
// does (datastore_t is passed explicitly to every datastore_* call) and because the host harness
// wants two of them at once -- one that works and one that fails -- to reach the error paths.
//
// iterate() and stat() are new: §9's have() answers per CHUNK, and a listing wants bytes. rename()
// is what makes the scratch-then-rename insert atomic.
//
// have() and complete() are the wire path's, and the content module does not call them yet. They
// stay in the seam because it is §9's contract: a store written now against the whole thing does
// not change shape when the transfer lands, and a store is the one thing here there will be four of.
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §4: "32-byte chunks, system-wide, with no negotiation", which fixes the u16 chunk index's ceiling
   at 2 MiB. NOT a configurable -- it is the one number every node on the mesh has to agree on
   without being told, and the only thing it is used for is giving have() a meaning. It sits with
   the seam because have() is the only caller there is. */
#define D_FILESTORE_CHUNK_BYTES 32u

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    void *ctx;

    /* bytes. read and write return the count transferred, or negative. A store must be able to
       read back whatever it can write -- verification depends on it (§9). */
    bool (*create)(void *ctx, uint32_t id, uint32_t total);
    int (*write)(void *ctx, uint32_t id, uint32_t off, const void *buf, size_t len);
    int (*read)(void *ctx, uint32_t id, uint32_t off, void *buf, size_t len);
    void (*destroy)(void *ctx, uint32_t id);
    bool (*rename)(void *ctx, uint32_t from, uint32_t to);

    /* what is here. iterate() walks from *cursor == 0 and returns false when done; the consumer
       never holds a cursor across a tick, so a store need not keep a directory handle open. */
    bool (*iterate)(void *ctx, uint32_t *cursor, uint32_t *id);
    bool (*stat)(void *ctx, uint32_t id, uint32_t *total, uint32_t *held);
    uint32_t (*space)(void *ctx);

    /* the wire path's, unused so far */
    bool (*have)(void *ctx, uint32_t id, uint16_t chunk);
    bool (*complete)(void *ctx, uint32_t id);
    int (*meta_write)(void *ctx, uint32_t id, const void *buf, size_t len);
    int (*meta_read)(void *ctx, uint32_t id, void *buf, size_t len);
} d_filestore_t;

#define D_FILESTORE_META_MAX 96u

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_FILESTORE_H */
