#ifndef IOTDATA_NODE_CONTENT_H
#define IOTDATA_NODE_CONTENT_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_content.h - the content store: what is held, who owns it, and how something new gets
// in. One module, one code path, a BACKEND per node (NOTES_CONTENT §9).
//
// WHAT THIS IS NOT, YET. Nothing here touches a radio. The CONTENT TLVs -- MANIFEST, RANGES, CHUNKS
// -- their codecs, the have-bitmap and the requester are all unbuilt; this is the half underneath
// them, driven from a console, and it is deliberately the half that the wire path will reuse rather
// than replace. The seam below is §9's, the id space is §4's, and the integrity algorithm is §7's,
// so when the TLVs land they announce into the same entry point instead of growing a second one.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// ONE ENTRY POINT, THREE CALLERS
//
// iotdata_node_content_announce() runs the claim loop over one id. Everything that can produce a
// new file calls it and nothing else does:
//
//     begin()              once per boot, for every file discovered in the store
//     a finished insert    from the tick, after the checksum passed and the rename landed
//     the wire path        later, when a transfer completes and verifies
//
// That is the shape the control vocabulary has for commands, and for the same reason: the console
// path and the wire path must not be two implementations of "a file arrived".
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// OWNERSHIP IS DECIDED AT BOOT AND PERSISTS NOWHERE
//
// The store holds BYTES. Nothing records who owns what, and on a cold boot every file is offered to
// the consumers again. Two things fall out of that, both wanted:
//
//   a file can be FACTORY-FLASHED into the store and will be picked up by whatever consumer claims
//   it, with no provisioning step -- a calibration blob, say, or firmware for a soft-loaded sensor
//
//   a consumer that must act on a file EVERY cold boot simply keeps claiming it, which needs no
//   "re-run me" flag anywhere
//
// A whole class of bug goes with it: there is no stored claim to disagree with the store, no
// migration when a consumer is added or removed, and nothing to repair after an unclean restart.
// The cost is that an interrupted piece of work restarts from the beginning, which is cheap against
// what it replaces.
//
// WHEN THE WIRE PATH ARRIVES it will need state that this does not: a transfer in flight has to
// survive a reboot, so "this file is in transit, and it is mine" becomes real persisted state owned
// by whatever module runs the transfer. That is a reason to put it there and not here.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// SCRATCH IDS ARE 0x000xxx, AND THEY DIE AT STARTUP
//
// §4 makes a content-id whose top 12 bits are 0x000 or 0xFFF MALFORMED BY CONSTRUCTION -- the
// recommended allocation is (station << 12) | counter, station 0 is unassignable and 0xFFF is
// broadcast. So 0x000xxx can never be advertised, requested or answered, which makes it exactly the
// right place for a file that is being built and is not yet anything.
//
// begin() DESTROYS every scratch file it finds, after offering them. A scratch file has only
// temporal existence: if a reboot interrupted whatever was producing it, that thing starts again
// from its source, and a half-built file with nobody to finish it is waste rather than progress --
// and quite possibly the reason for the reboot.
//
// They are offered FIRST, though, and that is the hook rather than an inconsistency: a consumer that
// genuinely can resume -- the content assembly step, when it exists -- claims its own scratch and
// keeps it. The console insert deliberately does NOT, which is what makes an interrupted upload
// disappear by the general rule instead of by a special case.
//
// This is also why the insert writes to scratch and renames on success: a file at a real id is one
// that passed its checksum, so "everything not scratch is legitimate" actually holds rather than
// nearly holding.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ONE ACTIVE JOB RULE
//
// At most one insert OR one working consumer at a time, and the second is refused with a reason.
// This is a single-task cooperative loop on the device -- no content task -- so two jobs would mean
// two sets of open handles and two claims on the same per-tick budget, for no use case anybody has
// asked for. One `if` buys that.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONTENT_CONSUMERS_MAX
#define IOTDATA_NODE_CONTENT_CONSUMERS_MAX 4
#endif

/* Ownership records, not files: an unclaimed file needs none, so this bounds how many consumers'
   worth of claims are live rather than how much the store holds. */
#ifndef IOTDATA_NODE_CONTENT_OWNED_MAX
#define IOTDATA_NODE_CONTENT_OWNED_MAX 8
#endif

/* An insert with nothing arriving is abandoned and its scratch destroyed. Engineering dimension,
   not a site preference -- a stalled upload is a stalled cable. */
#ifndef IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S
#define IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S 30u
#endif

/* How often an owner that said "keep" is asked again. See WHY ONLY THE OWNER, below. */
#ifndef IOTDATA_NODE_CONTENT_REOFFER_S
#define IOTDATA_NODE_CONTENT_REOFFER_S 60u
#endif

#ifndef IOTDATA_NODE_CONTENT_DESCRIBE_MAX
#define IOTDATA_NODE_CONTENT_DESCRIBE_MAX 96
#endif

/* How many ids begin() will snapshot in one pass. It takes a COPY because a backend's iterate()
   cannot be expected to survive the destroy() that reaping scratch does mid-walk. */
#ifndef IOTDATA_NODE_CONTENT_SCAN_MAX
#define IOTDATA_NODE_CONTENT_SCAN_MAX 16
#endif

/* How much of a file is read at a time when summing it back. A stack buffer in one function, so
   bigger is cheap; past a flash page it buys nothing. */
#ifndef IOTDATA_NODE_CONTENT_BLOCK_BYTES
#define IOTDATA_NODE_CONTENT_BLOCK_BYTES 256u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE BACKEND LIVES IN device/d_module_filestore.h, as d_filestore_t
//
// It moved there because it had to be includable without this header: a d_ module must come before
// any iotdata header, and the three filestores that implement the seam were reaching back through
// the whole framework to get at a struct of function pointers that depends on none of it. The
// dependency runs one way now -- this module consumes the interface, the interface knows nothing
// about content -- and the reasoning about what is in the seam and why lives beside it.
//
// This header does not include it. Nothing here does: an application, or iotdata_node_platform.h on
// its behalf, names its includes explicitly and in order, and every d_ module is already in by the
// time the node framework is read.
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHERE ITS WORDS GO
//
// The same seam the recorder has, and for the same reason: `content list` typed at a console must
// answer the person who typed it, while the identical command arriving over the air has nobody to
// answer and belongs in the log. One redirectable emit, set around a console dispatch and put back
// afterwards, rather than every function taking a sink it almost never uses.
//
// A LINE AT A TIME and not a printf: the formatting belongs to whoever has a buffer to do it in,
// and keeping varargs out of here is what lets this module build on a host with no log at all.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef void (*iotdata_node_content_emit_fn)(const char *line);

static iotdata_node_content_emit_fn _iotdata_node_content_emit = NULL;

static inline iotdata_node_content_emit_fn iotdata_node_content_emit_set(const iotdata_node_content_emit_fn fn) {
    const iotdata_node_content_emit_fn was = _iotdata_node_content_emit;
    _iotdata_node_content_emit = fn;
    return was;
}

static inline void _iotdata_node_content_say(const char *const line) {
    if (_iotdata_node_content_emit != NULL && line != NULL)
        _iotdata_node_content_emit(line);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT A CONSUMER SAYS, AS BITS
//
// One type for claim() and for work(), because the states a consumer can be in are the same either
// way and two enums would need translating between them:
//
//     0                     not mine -- leave it where it is, it simply sits there
//     MINE                  mine, resident, nothing to do now. I may be asked again
//     MINE | WORK           mine, and tick me
//     MINE | DELETE         mine, and I am finished with it
//     MINE | DELETE | REBOOT   ... and the node must restart
//     DELETE                not mine in any useful sense, and should not be kept
//
// There is deliberately no "mine but not now". It is MINE: an owner that said keep is asked again
// on a cadence, which is the same answer and one state fewer.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef uint8_t iotdata_node_content_result_t;

#define IOTDATA_NODE_CONTENT_R_NONE   0x00u
#define IOTDATA_NODE_CONTENT_R_MINE   0x01u
#define IOTDATA_NODE_CONTENT_R_DELETE 0x02u
#define IOTDATA_NODE_CONTENT_R_WORK   0x04u
#define IOTDATA_NODE_CONTENT_R_REBOOT 0x08u

typedef struct {
    const char *name; /* for a listing and a log line; not an identifier */
    void *ctx;
    /* Offered a file. Cheap checks only -- a header read at most; anything expensive belongs in
       work(). Called again on the re-offer cadence while the answer stays a bare MINE. */
    iotdata_node_content_result_t (*claim)(void *ctx, uint32_t id, uint32_t size);
    /* Ticked while the last answer carried WORK. Must do a BOUNDED piece and return; the loop this
       runs in has a radio to service. */
    iotdata_node_content_result_t (*work)(void *ctx, uint32_t id);
    /* For `content inspect`: one line about what this actually is. Optional. */
    int (*describe)(void *ctx, uint32_t id, char *out, size_t size);
} iotdata_node_content_consumer_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    IOTDATA_NODE_CONTENT_EVENT_NONE = 0,
    IOTDATA_NODE_CONTENT_EVENT_ARRIVED,         /* an insert finished, verified and was announced */
    IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED,   /* ... or did not, and its scratch is gone        */
    IOTDATA_NODE_CONTENT_EVENT_WORK_DONE,       /* an owner finished what it was ticking for      */
    IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED, /* ... and said the node must restart             */
} iotdata_node_content_event_t;

typedef enum {
    IOTDATA_NODE_CONTENT_OK = 0,
    IOTDATA_NODE_CONTENT_ERR_STATE,    /* something else is in progress, or nothing is       */
    IOTDATA_NODE_CONTENT_ERR_ID,       /* malformed by construction -- see SCRATCH IDS above */
    IOTDATA_NODE_CONTENT_ERR_SIZE,     /* zero, or more than the backend says it has room for */
    IOTDATA_NODE_CONTENT_ERR_OFFSET,   /* not the next bytes: a line went missing             */
    IOTDATA_NODE_CONTENT_ERR_STORE,    /* the backend refused                                 */
    IOTDATA_NODE_CONTENT_ERR_CHECKSUM, /* the right number of the wrong bytes                 */
    IOTDATA_NODE_CONTENT_ERR_TIMEOUT,  /* nothing arrived for long enough                     */
    IOTDATA_NODE_CONTENT_ERR_FULL,     /* no room in the ownership table                      */
    /* APPENDED, and that is the rule for this enum: a value reaches a log and will reach a record,
       so a new one goes on the END rather than in the middle where it renumbers the rest. */
    IOTDATA_NODE_CONTENT_ERR_ABSENT,  /* a well-formed id that nothing is stored under       */
    IOTDATA_NODE_CONTENT_ERR_STATION, /* §4: a well-formed id belonging to another station    */
} iotdata_node_content_status_t;

static inline const char *iotdata_node_content_status_name(const iotdata_node_content_status_t s) {
    switch (s) {
    case IOTDATA_NODE_CONTENT_OK:
        return "ok";
    case IOTDATA_NODE_CONTENT_ERR_STATE:
        return "wrong state";
    case IOTDATA_NODE_CONTENT_ERR_ID:
        return "malformed content id";
    case IOTDATA_NODE_CONTENT_ERR_SIZE:
        return "size is zero or will not fit";
    case IOTDATA_NODE_CONTENT_ERR_OFFSET:
        return "not the bytes expected next";
    case IOTDATA_NODE_CONTENT_ERR_STORE:
        return "the store refused";
    case IOTDATA_NODE_CONTENT_ERR_CHECKSUM:
        return "checksum mismatch";
    case IOTDATA_NODE_CONTENT_ERR_TIMEOUT:
        return "timed out";
    case IOTDATA_NODE_CONTENT_ERR_FULL:
        return "no room to own another";
    case IOTDATA_NODE_CONTENT_ERR_ABSENT:
        return "no such content id";
    case IOTDATA_NODE_CONTENT_ERR_STATION:
        return "that id belongs to another station";
    default:
        break;
    }
    return "?";
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONTENT_OWNER_NONE 0xFFu

typedef struct {
    uint32_t id;
    uint8_t owner; /* index into consumer[] */
    bool working;  /* the owner's last answer carried WORK */
    uint32_t asked_ms;
} iotdata_node_content_owned_t;

typedef struct {
    bool active;
    bool ready;  /* every declared byte is in: the tick verifies and lands it */
    bool failed; /* refuse the rest; the tick destroys the scratch and reports */
    iotdata_node_content_status_t why;
    uint32_t id, scratch, size, offset;
    uint64_t crc_want, crc_have;
    uint32_t started_ms, last_ms;
} iotdata_node_content_insert_t;

typedef struct {
    const d_filestore_t *backend;
    const iotdata_node_content_consumer_t *consumer[IOTDATA_NODE_CONTENT_CONSUMERS_MAX];
    uint8_t consumers;
    iotdata_node_content_owned_t owned[IOTDATA_NODE_CONTENT_OWNED_MAX];
    uint8_t owns;
    iotdata_node_content_insert_t rx;
    bool begun;
    /* counters, for `content stat` and for a test to assert on rather than infer */
    uint32_t stat_announced, stat_claimed, stat_deleted, stat_inserted, stat_insert_failed, stat_scratch_reaped, stat_reoffers;
    /* Both of these mean the firmware is misdimensioned rather than that something went wrong, so
       they are counted and visible instead of logged once and lost. */
    uint32_t stat_own_full, stat_scan_full;
} iotdata_node_content_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// IDS
// -----------------------------------------------------------------------------------------------------------------------------------------

/* §4: the recommended allocation is (station << 12) | counter, which makes the top 12 bits a
   station -- and station 0 is unassignable while 0xFFF is broadcast. So both are free for local
   meanings, and neither can ever appear on the wire. */
static inline bool iotdata_node_content_id_is_scratch(const uint32_t id) {
    return (id >> 12) == 0x000u;
}
static inline bool iotdata_node_content_id_is_valid(const uint32_t id) {
    const uint32_t station = id >> 12;
    return id <= 0xFFFFFFu && station != 0x000u && station != 0xFFFu;
}
static inline uint32_t iotdata_node_content_id_scratch(const uint32_t n) {
    return n & 0xFFFu; /* 0x000001 .. 0x000FFF; 0 is not one */
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE TABLE OF OWNERS
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_owned_t *_iotdata_node_content_owner_of(iotdata_node_content_t *const c, const uint32_t id) {
    for (uint8_t i = 0; i < c->owns; i++)
        if (c->owned[i].id == id)
            return &c->owned[i];
    return NULL;
}

/* Order carries no meaning here, so a removal moves the last entry down rather than shuffling. */
static inline void _iotdata_node_content_disown(iotdata_node_content_t *const c, const uint32_t id) {
    for (uint8_t i = 0; i < c->owns; i++)
        if (c->owned[i].id == id) {
            c->owned[i] = c->owned[c->owns - 1u];
            c->owns--;
            return;
        }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// ANNOUNCE -- the one entry point
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Offer one id to the consumers, FIRST CLAIMANT WINS, and act on what it says. Returns the answer,
   so a caller can see that nobody wanted it (0) without asking again.
   A fresh offer SUPERSEDES any previous claim on the same id, because that is what the cases
   demand: a re-inserted id is new bytes under an old name (§3's rule, applied locally), and a
   re-announce after work finished should not find a stale record of the work. */
static inline iotdata_node_content_result_t iotdata_node_content_announce(iotdata_node_content_t *const c, const uint32_t id, const uint32_t now_ms) {
    if (c == NULL || c->backend == NULL || c->backend->stat == NULL)
        return IOTDATA_NODE_CONTENT_R_NONE;
    uint32_t total = 0u, held = 0u;
    if (!c->backend->stat(c->backend->ctx, id, &total, &held))
        return IOTDATA_NODE_CONTENT_R_NONE; /* not there: announcing it would be a lie */
    c->stat_announced++;
    _iotdata_node_content_disown(c, id);
    for (uint8_t i = 0; i < c->consumers; i++) {
        const iotdata_node_content_consumer_t *const k = c->consumer[i];
        if (k != NULL && k->claim != NULL) {
            const iotdata_node_content_result_t r = k->claim(k->ctx, id, total);
            if ((r & (IOTDATA_NODE_CONTENT_R_MINE | IOTDATA_NODE_CONTENT_R_DELETE))) {
                if ((r & IOTDATA_NODE_CONTENT_R_DELETE) != 0u) {
                    /* Finished with it, or refusing it outright. Either way it goes, and no ownership is
                    recorded for a file that is about to stop existing. */
                    c->backend->destroy(c->backend->ctx, id);
                    c->stat_deleted++;
                    if ((r & IOTDATA_NODE_CONTENT_R_MINE) != 0u)
                        c->stat_claimed++;
                } else {
                    c->stat_claimed++;
                    if (c->owns < IOTDATA_NODE_CONTENT_OWNED_MAX) {
                        c->owned[c->owns].id = id;
                        c->owned[c->owns].owner = i;
                        c->owned[c->owns].working = ((r & IOTDATA_NODE_CONTENT_R_WORK) != 0u);
                        c->owned[c->owns].asked_ms = now_ms;
                        c->owns++;
                    } else
                        c->stat_own_full++;
                }
                return r;
            }
        }
    }
    return IOTDATA_NODE_CONTENT_R_NONE; /* nobody wanted it, so it simply sits there */
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// LIFECYCLE
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_content_init(iotdata_node_content_t *const c, const d_filestore_t *const backend) {
    if (c == NULL)
        return;
    *c = (iotdata_node_content_t){ 0 };
    c->backend = backend;
}

static inline bool iotdata_node_content_register(iotdata_node_content_t *const c, const iotdata_node_content_consumer_t *const k) {
    if (c == NULL || k == NULL || c->consumers >= IOTDATA_NODE_CONTENT_CONSUMERS_MAX)
        return false;
    c->consumer[c->consumers++] = k;
    return true;
}

/* Everything the store holds, offered to whoever wants it -- and the scratch reaped. Returns how
   many ids were announced, or -1 if there is no usable backend.
   FOUR PASSES, over a SNAPSHOT of the ids, which is not fussiness: a backend's iterate() cannot be
   expected to survive a destroy() mid-walk (a directory handle certainly would not), and the scratch
   must be OFFERED before any of it is reaped or a resumer never gets the chance. */
static inline int iotdata_node_content_begin(iotdata_node_content_t *const c, const uint32_t now_ms) {
    if (c == NULL || c->backend == NULL || c->backend->iterate == NULL || c->backend->stat == NULL)
        return -1;
    c->begun = true;

    uint32_t ids[IOTDATA_NODE_CONTENT_SCAN_MAX];
    uint8_t n = 0;
    uint32_t cursor = 0u, id = 0u;
    while (c->backend->iterate(c->backend->ctx, &cursor, &id)) {
        if (n >= IOTDATA_NODE_CONTENT_SCAN_MAX) {
            c->stat_scan_full++;
            break;
        }
        ids[n++] = id;
    }

    int announced = 0;
    for (uint8_t i = 0; i < n; i++) /* 1: scratch, offered -- the only chance a resumer gets */
        if (iotdata_node_content_id_is_scratch(ids[i])) {
            (void)iotdata_node_content_announce(c, ids[i], now_ms);
            announced++;
        }
    for (uint8_t i = 0; i < n; i++) /* 2: scratch nobody took, reaped */
        if (iotdata_node_content_id_is_scratch(ids[i]) && _iotdata_node_content_owner_of(c, ids[i]) == NULL) {
            c->backend->destroy(c->backend->ctx, ids[i]);
            c->stat_scratch_reaped++;
        }
    for (uint8_t i = 0; i < n; i++) /* 3: everything with a real id */
        if (!iotdata_node_content_id_is_scratch(ids[i])) {
            (void)iotdata_node_content_announce(c, ids[i], now_ms);
            announced++;
        }
    return announced;
}

/* Stops the module. An insert in flight loses its scratch, and the ownership table is cleared --
   which costs nothing to rebuild, because it was never persisted in the first place. */
static inline void iotdata_node_content_end(iotdata_node_content_t *const c) {
    if (c == NULL)
        return;
    if (c->rx.active && c->backend != NULL)
        c->backend->destroy(c->backend->ctx, c->rx.scratch);
    c->rx = (iotdata_node_content_insert_t){ 0 };
    c->owns = 0u;
    c->begun = false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// INSERT -- staged as scratch, renamed on proof
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool _iotdata_node_content_busy(const iotdata_node_content_t *const c) {
    for (uint8_t i = 0; i < c->owns; i++)
        if (c->owned[i].working)
            return true;
    return false;
}

/* Declare what is coming: where it will land, how many bytes, and what they must hash to. The bytes
   go to a scratch id and only become `id` once the checksum agrees (see SCRATCH IDS). */
static inline iotdata_node_content_status_t iotdata_node_content_insert_begin(iotdata_node_content_t *const c, const uint32_t id, const uint32_t size, const uint64_t crc, const uint32_t now_ms) {
    if (c == NULL || c->backend == NULL || c->backend->create == NULL || c->backend->stat == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (c->rx.active || _iotdata_node_content_busy(c)) /* THE ONE ACTIVE JOB RULE */
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (!iotdata_node_content_id_is_valid(id))
        return IOTDATA_NODE_CONTENT_ERR_ID;
    if (size == 0u)
        return IOTDATA_NODE_CONTENT_ERR_SIZE;
    if (c->backend->space != NULL && c->backend->space(c->backend->ctx) < size)
        return IOTDATA_NODE_CONTENT_ERR_SIZE;

    uint32_t scratch = 0u, total = 0u, held = 0u;
    for (uint32_t probe = 1u; probe <= 0xFFFu; probe++)
        if (!c->backend->stat(c->backend->ctx, probe, &total, &held)) {
            scratch = probe;
            break;
        }
    if (scratch == 0u || !c->backend->create(c->backend->ctx, scratch, size))
        return IOTDATA_NODE_CONTENT_ERR_STORE;

    c->rx = (const iotdata_node_content_insert_t){
        .active = true,
        .id = id,
        .scratch = scratch,
        .size = size,
        .crc_want = crc,
        .crc_have = _IOTDATA_CRC64_INIT,
        .started_ms = now_ms,
        .last_ms = now_ms,
    };
    return IOTDATA_NODE_CONTENT_OK;
}

/* The next run of bytes, at the offset they belong at. `off` is checked rather than trusted: it is
   the only thing standing between a dropped line and a file that is the right length and the wrong
   content, and catching it here means the checksum is a backstop instead of the sole defence.
//
   A REFUSAL STICKS. Once this has said no, the insert is doomed and every further write is refused,
   so the caller can keep feeding to the declared end -- which is what a console does, because the
   alternative is interpreting the remaining data as commands. The tick clears up. */
static inline iotdata_node_content_status_t iotdata_node_content_insert_write(iotdata_node_content_t *const c, const uint32_t off, const void *const buf, const size_t len, const uint32_t now_ms) {
    if (c == NULL || !c->rx.active || c->rx.ready)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (c->rx.failed)
        return c->rx.why;
    if (off != c->rx.offset) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_OFFSET;
        return c->rx.why;
    }
    c->rx.last_ms = now_ms;
    if (len == 0u || buf == NULL)
        return IOTDATA_NODE_CONTENT_OK;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)c->rx.size) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_SIZE;
        return c->rx.why;
    }
    if (c->backend->write(c->backend->ctx, c->rx.scratch, off, buf, len) != (int)len) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_STORE;
        return c->rx.why;
    }
    c->rx.crc_have = _iotdata_crc64(buf, len, c->rx.crc_have);
    c->rx.offset += (uint32_t)len;
    if (c->rx.offset == c->rx.size)
        c->rx.ready = true;
    return IOTDATA_NODE_CONTENT_OK;
}

static inline void iotdata_node_content_insert_abort(iotdata_node_content_t *const c, const iotdata_node_content_status_t why) {
    if (c != NULL && c->rx.active && !c->rx.failed) {
        c->rx.failed = true;
        c->rx.why = (why == IOTDATA_NODE_CONTENT_OK) ? IOTDATA_NODE_CONTENT_ERR_STATE : why;
    }
}

static inline bool iotdata_node_content_insert_active(const iotdata_node_content_t *const c) {
    return c != NULL && c->rx.active;
}

static inline void iotdata_node_content_insert_progress(const iotdata_node_content_t *const c, uint32_t *const id, uint32_t *const have, uint32_t *const want) {
    if (id != NULL)
        *id = (c != NULL) ? c->rx.id : 0u;
    if (have != NULL)
        *have = (c != NULL) ? c->rx.offset : 0u;
    if (want != NULL)
        *want = (c != NULL) ? c->rx.size : 0u;
}

static inline iotdata_node_content_status_t iotdata_node_content_insert_why(const iotdata_node_content_t *const c) {
    return (c != NULL) ? c->rx.why : IOTDATA_NODE_CONTENT_ERR_STATE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE TICK
//
// In strict order, and at most one thing per pass: resolve an insert, give a worker a slice, then
// the re-offer cadence. An insert in flight short-circuits the rest -- THE ONE ACTIVE JOB RULE is
// enforced here as well as at insert_begin(), because a worker could otherwise be started by an
// announce that an insert itself triggered.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_event_t _iotdata_node_content_tick_insert(iotdata_node_content_t *const c, const uint32_t now_ms, bool *const resolved) {
    *resolved = false;
    if (!c->rx.failed && !c->rx.ready && (now_ms - c->rx.last_ms) >= (uint32_t)IOTDATA_NODE_CONTENT_INSERT_TIMEOUT_S * 1000u) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_TIMEOUT;
    }
    if (c->rx.failed) {
        c->backend->destroy(c->backend->ctx, c->rx.scratch);
        const iotdata_node_content_status_t why = c->rx.why;
        c->rx = (iotdata_node_content_insert_t){ 0 };
        c->rx.why = why; /* kept so insert_why() can still be asked after the event */
        c->stat_insert_failed++;
        *resolved = true;
        return IOTDATA_NODE_CONTENT_EVENT_INSERT_FAILED;
    }
    if (!c->rx.ready)
        return IOTDATA_NODE_CONTENT_EVENT_NONE; /* still arriving */

    if (c->rx.crc_have != c->rx.crc_want) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_CHECKSUM;
        return _iotdata_node_content_tick_insert(c, now_ms, resolved); /* one destruction path */
    }
    /* §3's supersede rule, locally: new bytes under an old id replace what was there. Destroyed
       HERE and not at insert_begin, so a failed upload never costs a good file. */
    uint32_t total = 0u, held = 0u;
    if (c->backend->stat(c->backend->ctx, c->rx.id, &total, &held))
        c->backend->destroy(c->backend->ctx, c->rx.id);
    if (c->backend->rename == NULL || !c->backend->rename(c->backend->ctx, c->rx.scratch, c->rx.id)) {
        c->rx.failed = true;
        c->rx.why = IOTDATA_NODE_CONTENT_ERR_STORE;
        return _iotdata_node_content_tick_insert(c, now_ms, resolved);
    }
    if (c->backend->complete != NULL)
        (void)c->backend->complete(c->backend->ctx, c->rx.id);

    const uint32_t landed = c->rx.id;
    c->rx = (iotdata_node_content_insert_t){ 0 };
    c->stat_inserted++;
    *resolved = true;
    (void)iotdata_node_content_announce(c, landed, now_ms); /* the one entry point, second caller */
    return IOTDATA_NODE_CONTENT_EVENT_ARRIVED;
}

/* ASK THE OWNER AGAIN AT THE NEXT TICK, rather than at the next cadence. The re-offer runs every
   IOTDATA_NODE_CONTENT_REOFFER_S because an owner that said "keep it" is usually waiting on nothing
   in particular -- but when something has changed that the owner asked for, a minute of nothing is
   indistinguishable from the command having been ignored. An operator who arms a firmware bundle by
   hand should see it taken, not wonder. Returns false if the id is not owned, which is the honest
   answer rather than a silent no-op. */
static inline bool iotdata_node_content_reoffer(iotdata_node_content_t *const c, const uint32_t id) {
    if (c == NULL)
        return false;
    for (uint8_t i = 0; i < c->owns; i++)
        if (c->owned[i].id == id && !c->owned[i].working) {
            c->owned[i].asked_ms -= (uint32_t)IOTDATA_NODE_CONTENT_REOFFER_S * 1000u;
            return true;
        }
    return false;
}

static inline iotdata_node_content_event_t iotdata_node_content_tick(iotdata_node_content_t *const c, const uint32_t now_ms) {
    if (c == NULL || c->backend == NULL || !c->begun)
        return IOTDATA_NODE_CONTENT_EVENT_NONE;

    if (c->rx.active) {
        bool resolved = false;
        const iotdata_node_content_event_t ev = _iotdata_node_content_tick_insert(c, now_ms, &resolved);
        if (resolved || !c->rx.active)
            return ev;
        return IOTDATA_NODE_CONTENT_EVENT_NONE; /* an upload owns the budget while it runs */
    }

    /* One worker, one bounded slice, and we return either way -- a second worker in the same pass
       would be the thing the budget exists to prevent. */
    for (uint8_t i = 0; i < c->owns; i++)
        if (c->owned[i].working) {
            const iotdata_node_content_consumer_t *const k = c->consumer[c->owned[i].owner];
            const uint32_t id = c->owned[i].id;
            if (k == NULL || k->work == NULL) {
                /* It asked to be ticked and has nothing to tick with. Stop asking rather than spin. */
                c->owned[i].working = false;
                return IOTDATA_NODE_CONTENT_EVENT_NONE;
            }
            const iotdata_node_content_result_t r = k->work(k->ctx, id);
            if ((r & IOTDATA_NODE_CONTENT_R_WORK) != 0u) {
                c->owned[i].asked_ms = now_ms;
                return IOTDATA_NODE_CONTENT_EVENT_NONE; /* still going; come back next pass */
            }
            c->owned[i].working = false;
            c->owned[i].asked_ms = now_ms;
            if ((r & IOTDATA_NODE_CONTENT_R_DELETE) != 0u) {
                c->backend->destroy(c->backend->ctx, id);
                c->stat_deleted++;
                _iotdata_node_content_disown(c, id);
            } else if ((r & IOTDATA_NODE_CONTENT_R_MINE) == 0u)
                _iotdata_node_content_disown(c, id); /* handed it back */
            return ((r & IOTDATA_NODE_CONTENT_R_REBOOT) != 0u) ? IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED : IOTDATA_NODE_CONTENT_EVENT_WORK_DONE;
        }

    /* THE RE-OFFER, and WHY ONLY THE OWNER. A bare MINE means "keep it, and I may be asked again",
       which is the state that replaced a separate "not now" -- so the question to re-ask is the
       owner's, not the whole loop's. Re-running every consumer would also mean every consumer's
       claim() reading every file on a timer, and a claim is allowed to read a header.
       An UNOWNED file is never re-offered: nobody wanted it, and nothing has changed since. */
    for (uint8_t i = 0; i < c->owns; i++)
        if (!c->owned[i].working && (now_ms - c->owned[i].asked_ms) >= (uint32_t)IOTDATA_NODE_CONTENT_REOFFER_S * 1000u) {
            const iotdata_node_content_consumer_t *const k = c->consumer[c->owned[i].owner];
            const uint32_t id = c->owned[i].id;
            uint32_t total = 0u, held = 0u;
            c->owned[i].asked_ms = now_ms;
            c->stat_reoffers++;
            if (k != NULL && k->claim != NULL && c->backend->stat(c->backend->ctx, id, &total, &held)) {
                const iotdata_node_content_result_t r = k->claim(k->ctx, id, total);
                if ((r & IOTDATA_NODE_CONTENT_R_DELETE) != 0u) {
                    c->backend->destroy(c->backend->ctx, id);
                    c->stat_deleted++;
                    _iotdata_node_content_disown(c, id);
                    return ((r & IOTDATA_NODE_CONTENT_R_REBOOT) != 0u) ? IOTDATA_NODE_CONTENT_EVENT_REBOOT_REQUIRED : IOTDATA_NODE_CONTENT_EVENT_WORK_DONE;
                }
                if ((r & IOTDATA_NODE_CONTENT_R_MINE) == 0u)
                    _iotdata_node_content_disown(c, id);
                else
                    c->owned[i].working = ((r & IOTDATA_NODE_CONTENT_R_WORK) != 0u);
            } else
                _iotdata_node_content_disown(c, id);
            return IOTDATA_NODE_CONTENT_EVENT_NONE;
        }
    return IOTDATA_NODE_CONTENT_EVENT_NONE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT IS HERE -- list, delete, inspect
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_content_iterate(iotdata_node_content_t *const c, uint32_t *const cursor, uint32_t *const id, uint32_t *const total, uint32_t *const held, const char **const owner) {
    if (c == NULL || c->backend == NULL || c->backend->iterate == NULL || cursor == NULL || id == NULL)
        return false;
    if (!c->backend->iterate(c->backend->ctx, cursor, id))
        return false;
    uint32_t t = 0u, h = 0u;
    if (c->backend->stat != NULL)
        (void)c->backend->stat(c->backend->ctx, *id, &t, &h);
    if (total != NULL)
        *total = t;
    if (held != NULL)
        *held = h;
    if (owner != NULL) {
        const iotdata_node_content_owned_t *const o = _iotdata_node_content_owner_of(c, *id);
        *owner = (o != NULL && c->consumer[o->owner] != NULL) ? c->consumer[o->owner]->name : NULL;
    }
    return true;
}

static inline uint32_t iotdata_node_content_count(iotdata_node_content_t *const c) {
    uint32_t n = 0u, cursor = 0u, id = 0u;
    if (c == NULL || c->backend == NULL || c->backend->iterate == NULL)
        return 0u;
    while (c->backend->iterate(c->backend->ctx, &cursor, &id))
        n++;
    return n;
}

/* What the store will say about one file and about itself, without a caller reaching through to the
   backend to ask. Thin on purpose: the point is that the backend stays the module's, so a second
   backend cannot be reached around in one place and not another. */
static inline bool iotdata_node_content_stat(iotdata_node_content_t *const c, const uint32_t id, uint32_t *const total, uint32_t *const held) {
    if (c == NULL || c->backend == NULL || c->backend->stat == NULL)
        return false;
    return c->backend->stat(c->backend->ctx, id, total, held);
}

static inline int iotdata_node_content_read(iotdata_node_content_t *const c, const uint32_t id, const uint32_t off, void *const buf, const size_t len) {
    if (c == NULL || c->backend == NULL || c->backend->read == NULL)
        return -1;
    return c->backend->read(c->backend->ctx, id, off, buf, len);
}

static inline uint32_t iotdata_node_content_space(iotdata_node_content_t *const c) {
    if (c == NULL || c->backend == NULL || c->backend->space == NULL)
        return 0u;
    return c->backend->space(c->backend->ctx);
}

static inline iotdata_node_content_status_t iotdata_node_content_delete(iotdata_node_content_t *const c, const uint32_t id) {
    if (c == NULL || c->backend == NULL || c->backend->stat == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (c->rx.active && c->rx.id == id)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    const iotdata_node_content_owned_t *const o = _iotdata_node_content_owner_of(c, id);
    if (o != NULL && o->working)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    uint32_t total = 0u, held = 0u;
    if (!c->backend->stat(c->backend->ctx, id, &total, &held))
        return IOTDATA_NODE_CONTENT_ERR_ABSENT;
    c->backend->destroy(c->backend->ctx, id);
    _iotdata_node_content_disown(c, id);
    c->stat_deleted++;
    return IOTDATA_NODE_CONTENT_OK;
}

/* WHO OWNS IT, by name, or NULL for nobody -- the single-id form of what iterate() hands back per
   row. Separate from describe() because the two answer different questions and describe() cannot
   answer this one: it returns 0 both for a file nobody owns and for a file whose owner has nothing
   to say about it, and a caller that read that as "unowned" would report an owned file as an
   orphan. */
static inline const char *iotdata_node_content_owner(iotdata_node_content_t *const c, const uint32_t id) {
    if (c == NULL)
        return NULL;
    const iotdata_node_content_owned_t *const o = _iotdata_node_content_owner_of(c, id);
    if (o == NULL)
        return NULL;
    const iotdata_node_content_consumer_t *const k = c->consumer[o->owner];
    return (k != NULL) ? k->name : NULL;
}

/* One line saying what a file actually IS, which only a consumer can know.
   An OWNED file is described by its owner and nobody else: there is one opinion about who a file
   belongs to and this does not invent a second. An UNOWNED one is offered round until somebody can
   say something, which is what makes `inspect` useful on a file that arrived for a consumer this
   build does not have. Returns the length written, 0 if nobody could say anything, negative on a
   bad call. */
static inline int iotdata_node_content_describe(iotdata_node_content_t *const c, const uint32_t id, char *const out, const size_t size) {
    if (c == NULL || out == NULL || size == 0u)
        return -1;
    out[0] = '\0';
    const iotdata_node_content_owned_t *const o = _iotdata_node_content_owner_of(c, id);
    if (o != NULL) {
        const iotdata_node_content_consumer_t *const k = c->consumer[o->owner];
        if (k != NULL && k->describe != NULL) {
            const int n = k->describe(k->ctx, id, out, size);
            if (n > 0)
                return n;
        }
        return 0;
    }
    for (uint8_t i = 0; i < c->consumers; i++) {
        const iotdata_node_content_consumer_t *const k = c->consumer[i];
        if (k != NULL && k->describe != NULL) {
            const int n = k->describe(k->ctx, id, out, size);
            if (n > 0)
                return n;
            out[0] = '\0';
        }
    }
    return 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING IT BACK AND SUMMING IT
//
// §7's gross hash, computed from the store rather than from the bytes as they arrived. The insert
// already sums what it is given, so this is the OTHER question: not "did I receive what was sent"
// but "is what I am holding still what I received". A flash page that has decayed, a backend that
// wrote somewhere it should not have, a file planted at the factory with no transfer behind it at
// all -- an incremental sum answers none of them.
//
// SYNCHRONOUS, and the one thing here that is. It reads the whole file in one call, which for a
// megabyte on SPIFFS is a visible fraction of a second with the radio unattended. That is a price
// a typed command may pay and a tick may not, so nothing ticks this: a caller that needs it
// bounded is the caller that should make it a job, the way the console download is one.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_status_t iotdata_node_content_checksum(iotdata_node_content_t *const c, const uint32_t id, uint64_t *const crc, uint32_t *const len) {
    if (crc != NULL)
        *crc = _IOTDATA_CRC64_INIT;
    if (len != NULL)
        *len = 0u;
    if (c == NULL || c->backend == NULL || c->backend->read == NULL || c->backend->stat == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    uint32_t total = 0u, held = 0u;
    if (!c->backend->stat(c->backend->ctx, id, &total, &held))
        return IOTDATA_NODE_CONTENT_ERR_ABSENT;
    uint64_t sum = _IOTDATA_CRC64_INIT;
    uint8_t buf[IOTDATA_NODE_CONTENT_BLOCK_BYTES];
    for (uint32_t off = 0u; off < held;) {
        const uint32_t want = ((held - off) < (uint32_t)sizeof(buf)) ? (held - off) : (uint32_t)sizeof(buf);
        if (c->backend->read(c->backend->ctx, id, off, buf, want) != (int)want)
            return IOTDATA_NODE_CONTENT_ERR_STORE;
        sum = _iotdata_crc64(buf, want, sum);
        off += want;
    }
    if (crc != NULL)
        *crc = sum;
    if (len != NULL)
        *len = held;
    /* An incomplete file sums fine and is still incomplete, so say which it was: a caller that
       treated a partial sum as a verdict would accept half an image. */
    return (held == total) ? IOTDATA_NODE_CONTENT_OK : IOTDATA_NODE_CONTENT_ERR_SIZE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_H */
