#ifndef IOTDATA_NODE_DOWN_H
#define IOTDATA_NODE_DOWN_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_down.h - flooding, holding and delivering a downstream frame.
//
// The design and its reasoning are in iotdata-specs/NOTES_DOWNSTREAM.md. In brief:
//
// A down frame (sequence == IOTDATA_SEQUENCE_DOWN, station == the TARGET) has no identity of its
// own -- its sequence is invariant, so two downs to one station differ only in their content. The
// key is therefore THE HASH OF THE WHOLE FRAME, which is exactly (station, sequence, variant,
// content) and gets broadcast right for free because the broadcast id is in those same bytes.
//
// FLOODED, NOT ROUTED, AND DEDUP IS WHAT STOPS IT. A node rebroadcasts each distinct frame exactly
// once and throws every copy that comes back, so propagation is bounded by the number of nodes
// rather than by a hop count -- just as well, since the header has no room for one. That single
// rebroadcast is also the first delivery attempt: it reaches every listening relay and any station
// awake at that moment.
//
// HELD IN A TABLE OF STATIONS, EACH WITH SLOTS. Broadcast is not a special case: it is a
// pseudo-station at index 0 whose id is IOTDATA_STATION_BROADCAST, holding slots like any other.
// A slot carries a hash, an arrival time, and a buffer OR NOT: a slot with no buffer is pure dedup
// memory, which is the common case and what bounds the cost of a wide flood.
//
// A NEW (station, content) IS A NEW DOWN. It never replaces an older one -- coexistence, not
// supersede -- because nothing here can know that a second command was meant to cancel the first.
//
// THERE IS NO UNILATERAL RESEND. Every transmission is either the opportunistic rebroadcast on
// arrival, or a targeted send when a window is known to be open. Nothing retries on a timer,
// because there is no acknowledgement anywhere on this path: a frame either went on the air or it
// did not, so there is no "lost" to detect and nothing a retry could be driven from. A command
// that misses is the ORIGINATOR's problem, and its remedy is a new frame -- which, being
// content-addressed, has to differ (see IOTDATA_NODE_TLV_DISCRIMINATOR).
//
// TWO DIFFERENCES ONLY, for broadcast: no window is ever announced for the broadcast id, so it is
// never delivered-and-discarded the way a unicast is; and when a station's window opens, the
// candidates are its own slots AND the broadcast slots, interleaved strictly by arrival time so a
// burst of unicast cannot starve a broadcast. A broadcast is kept after sending, with a per-station
// record of having been given it, and the expiry of THAT record is the cyclic resend.
//
// FRAMES LIVE IN A POOL (d_module_buffers.h) which the caller supplies -- a slot holds a
// REFERENCE, not a copy, so a held frame is delivered by handing the handle to a transmit queue
// rather than copying bytes, and held frames appear in the same accounting as every other frame.
// This is a LONG-TERM holder: size the pool for in-flight frames PLUS the slots here, or a table
// full of commands for sleeping nodes starves the traffic that would wake them.
//
// INCLUDE AFTER iotdata_node.h: the window rules read a RECEIVE advertisement, so this header now
// depends on that parsing rather than only on the framing.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

/* How many distinct targets can be tracked at once, including the broadcast pseudo-station. Wider
   than the stations a node can actually DELIVER to, deliberately: dup memory has to cover every
   target the flood mentions, delivery only covers neighbours we hear. */
#ifndef IOTDATA_NODE_DOWN_STATIONS
#define IOTDATA_NODE_DOWN_STATIONS 64
#endif

/* Concurrent downs per target, and with it the depth of that target's dup memory. Slots are cheap
   and BUFFERS ARE NOT: the pool bounds how many frames can actually be held at once, so raising
   this buys dedup depth -- which is what a content transfer needs, since a chunk response has to
   be recognised as a duplicate when several relays answer the same request -- without raising the
   number of frames in flight. */
#ifndef IOTDATA_NODE_DOWN_SLOTS
#define IOTDATA_NODE_DOWN_SLOTS 16
#endif

/* Broadcasts a station is remembered as having been given. Its size is the IMPLICIT repeat period:
   under broadcast pressure the oldest record is overwritten and the station becomes eligible for
   that broadcast again, sooner than the explicit interval below would allow. */
#ifndef IOTDATA_NODE_DOWN_SENT_SLOTS
#define IOTDATA_NODE_DOWN_SENT_SLOTS 4
#endif

/* How long a held unicast stays worth delivering. Compile-time values are STARTING points: firmware
   boots with them and calls the setters once its own configuration is loaded. */
#ifndef IOTDATA_NODE_DOWN_TTL_MS_DEFAULT
#define IOTDATA_NODE_DOWN_TTL_MS_DEFAULT (24UL * 60UL * 60UL * 1000UL) /* a day */
#endif

/* The same for a broadcast, longer: it is addressed to a population rather than to one node, so it
   has to outlive the sleep cycle of the slowest member of that population. */
#ifndef IOTDATA_NODE_DOWN_TTL_BCAST_MS_DEFAULT
#define IOTDATA_NODE_DOWN_TTL_BCAST_MS_DEFAULT (7UL * 24UL * 60UL * 60UL * 1000UL) /* a week */
#endif

/* The EXPLICIT cyclic resend: once this long has passed since a station was given a broadcast, it
   is eligible to be given it again. Broadcasts are important enough to want repeating. */
#ifndef IOTDATA_NODE_DOWN_REPEAT_MS_DEFAULT
#define IOTDATA_NODE_DOWN_REPEAT_MS_DEFAULT (5UL * 24UL * 60UL * 60UL * 1000UL) /* five days */
#endif

/* The ceiling, and it is not arbitrary. Times here are uint32 MILLISECONDS to match the transmit
   queue and the loop's clock, and that clock wraps every 49.7 days; ordering "before" from "after"
   across a wrap works over less than half of that. So no interval may exceed ~24.8 days. A site
   wanting longer needs a seconds-resolution clock, not a bigger constant -- and note these are
   UPTIME intervals, so a repeat longer than the time between reboots will rarely fire. */
#define IOTDATA_NODE_DOWN_TTL_MS_MAX (24UL * 24UL * 60UL * 60UL * 1000UL)

#if IOTDATA_NODE_DOWN_TTL_MS_DEFAULT > IOTDATA_NODE_DOWN_TTL_MS_MAX || IOTDATA_NODE_DOWN_TTL_BCAST_MS_DEFAULT > IOTDATA_NODE_DOWN_TTL_MS_MAX || IOTDATA_NODE_DOWN_REPEAT_MS_DEFAULT > IOTDATA_NODE_DOWN_TTL_MS_MAX
#error "a down interval exceeds what a wrapping 32-bit millisecond clock can order"
#endif

/* How often the tick actually scans. Against thresholds measured in hours or days, scanning every
   loop pass buys resolution nobody can use, so the tick costs one comparison per cycle and walks
   the table on this interval -- which is therefore the resolution of every expiry above. 0 scans
   every call, which is what a test driving expiry directly asks for. */
#ifndef IOTDATA_NODE_DOWN_SCAN_MS_DEFAULT
#define IOTDATA_NODE_DOWN_SCAN_MS_DEFAULT (15UL * 1000UL)
#endif

/* Which TLV type a held frame is assumed to carry, for the RECEIVE types mask. A slot does not
   record what is inside its frame -- nothing here decodes one -- so the gate is on the type that
   downstream traffic overwhelmingly is. Per-slot typing is a later refinement. */
#ifndef IOTDATA_NODE_DOWN_ASSUMED_TYPE
#define IOTDATA_NODE_DOWN_ASSUMED_TYPE IOTDATA_NODE_TLV_CONTROL
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint32_t hash;         /* of the whole frame; 0 = the slot is empty */
    uint32_t arrived_ms;   /* when this frame first reached us: the ordering key for delivery */
    buffer_handle_t frame; /* BUFFER_NONE when only the dup is being tracked */
} iotdata_node_down_slot_t;

typedef struct {
    uint32_t hash;    /* a broadcast this station has been given; 0 = empty */
    uint32_t sent_ms; /* when, so the record can expire and let it be given again */
} iotdata_node_down_sent_t;

typedef struct {
    uint16_t station;      /* 0 = unused (station 0 is never assignable); [0] is always broadcast */
    uint32_t down_last_ms; /* last arrival for this target, for eviction ordering */
    bool window_seen;      /* have we ever snooped a RECEIVE from it: whether we can deliver at all */
    uint32_t window_ms;    /* what it advertises, or the system default */
    uint32_t window_at_ms; /* when we last saw it open one */
    iotdata_node_down_slot_t slot[IOTDATA_NODE_DOWN_SLOTS];
    iotdata_node_down_sent_t sent[IOTDATA_NODE_DOWN_SENT_SLOTS];
} iotdata_node_down_station_t;

typedef struct {
    buffer_pool_t *pool;
    iotdata_node_down_station_t station[IOTDATA_NODE_DOWN_STATIONS];
    int count; /* entries in use, [0] included */
    uint32_t ttl_ms, ttl_bcast_ms, repeat_ms;
    uint32_t scan_ms, scan_last_ms;
    uint32_t stat_stored, stat_tracked, stat_dup, stat_delivered, stat_delivered_bcast, stat_expired, stat_evicted_slot, stat_evicted_station;
} iotdata_node_down_t;

/* What offering a frame decided. Everything except DUP means "put it on the air". */
typedef enum {
    IOTDATA_NODE_DOWN_DUP = 0, /* seen before: throw it, and stay quiet -- that silence bounds the flood */
    IOTDATA_NODE_DOWN_STORED,  /* new, and held for a window we expect to see */
    IOTDATA_NODE_DOWN_TRACKED, /* new, dup remembered but not held: unreachable, or already delivered */
} iotdata_node_down_ev_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_down_transmit(const iotdata_node_down_ev_t ev) {
    return ev != IOTDATA_NODE_DOWN_DUP;
}

static inline const char *iotdata_node_down_ev_name(const iotdata_node_down_ev_t ev) {
    switch (ev) {
    case IOTDATA_NODE_DOWN_STORED:
        return "stored";
    case IOTDATA_NODE_DOWN_TRACKED:
        return "tracked";
    case IOTDATA_NODE_DOWN_DUP:
    default:
        return "duplicate";
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* FNV-1a over the whole frame. 32 bits against a few hundred live slots is a ~10^-5 chance of a
   collision, and a collision silently drops one command; that is the accepted trade for 4 bytes a
   slot. Never returns 0, because 0 is how an empty slot says so. */
static inline uint32_t iotdata_node_down_hash(const uint8_t *const buf, const size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)buf[i];
        h *= 16777619u;
    }
    return h == 0u ? 1u : h;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT WE HAVE ALREADY ACTED ON
//
// A down frame addressed to US arrives once per PATH: every gateway that holds it sends it, and
// every relay that heard it passes it on. Acting on each copy answers it each time -- one
// `conf-set` came back as four config reports. The bytes are identical whichever way a copy came,
// because a down frame names its TARGET and carries no sender, so identical bytes inside a short
// window are one command.
//
// THE SAME RULE THE HOLD TABLE ALREADY APPLIES, for the frames that are NOT ours. This is it for
// the ones that are, and it lives here rather than in an application because every role needs it:
// the relay had it, the gateway did not, and the gateway therefore refused to act on a BROADCAST
// at all -- it could not tell another node's broadcast from the echo of its own. With this, it
// can: a node RECORDS ITS OWN ORIGINATIONS here, so the echo is a repeat by construction and
// broadcast stops being a special case. One model, every node.
//
// A DELIBERATE REPEAT AFTER THE WINDOW IS A NEW COMMAND and is acted on, which is what makes the
// window short: re-sending something because it seemed not to arrive has to work.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_DOWN_ACTED_WINDOW_MS
#define IOTDATA_NODE_DOWN_ACTED_WINDOW_MS 30000u
#endif
#ifndef IOTDATA_NODE_DOWN_ACTED_SLOTS
#define IOTDATA_NODE_DOWN_ACTED_SLOTS 8
#endif

typedef struct {
    struct {
        uint32_t hash, at_ms;
    } slot[IOTDATA_NODE_DOWN_ACTED_SLOTS];
    uint32_t stat_repeats;
} iotdata_node_down_acted_t;

/* True if these exact bytes have been seen here inside the window -- and either way the frame is
   now recorded, so the NEXT copy is a repeat. Call it once per frame and act on the answer.
 *
 * ALSO THE WAY A NODE DISOWNS ITS OWN ORIGINATION: record a frame as it goes out and the copy that
   comes back off a relay is a repeat before anything can act on it. */
static inline bool iotdata_node_down_acted(iotdata_node_down_acted_t *const a, const uint8_t *const buf, const size_t len, const uint32_t now_ms) {
    if (a == NULL || buf == NULL || len == 0u)
        return false;
    const uint32_t hash = iotdata_node_down_hash(buf, len); /* never 0, so an empty slot never matches */
    int oldest = 0;
    for (int i = 0; i < IOTDATA_NODE_DOWN_ACTED_SLOTS; i++) {
        if (a->slot[i].hash == hash && (now_ms - a->slot[i].at_ms) < (uint32_t)IOTDATA_NODE_DOWN_ACTED_WINDOW_MS) {
            a->stat_repeats++;
            return true;
        }
        if ((now_ms - a->slot[i].at_ms) > (now_ms - a->slot[oldest].at_ms))
            oldest = i;
    }
    a->slot[oldest].hash = hash;
    a->slot[oldest].at_ms = now_ms;
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_down_init(iotdata_node_down_t *const ds, buffer_pool_t *const pool) {
    *ds = (iotdata_node_down_t){ 0 };
    ds->pool = pool;
    ds->ttl_ms = IOTDATA_NODE_DOWN_TTL_MS_DEFAULT;
    ds->ttl_bcast_ms = IOTDATA_NODE_DOWN_TTL_BCAST_MS_DEFAULT;
    ds->repeat_ms = IOTDATA_NODE_DOWN_REPEAT_MS_DEFAULT;
    ds->scan_ms = IOTDATA_NODE_DOWN_SCAN_MS_DEFAULT;
    for (int i = 0; i < IOTDATA_NODE_DOWN_STATIONS; i++)
        for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
            ds->station[i].slot[j].frame = BUFFER_NONE;
    /* [0] is the broadcast pseudo-station, created here and never evicted: that is the whole of
       the "never evict broadcast" rule, and it makes its lookup a constant. */
    ds->station[0].station = IOTDATA_STATION_BROADCAST;
    ds->count = 1;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_down_set_ttl_ms(iotdata_node_down_t *const ds, const uint32_t ttl_ms) {
    ds->ttl_ms = (ttl_ms > IOTDATA_NODE_DOWN_TTL_MS_MAX) ? (uint32_t)IOTDATA_NODE_DOWN_TTL_MS_MAX : ttl_ms;
}
static inline uint32_t iotdata_node_down_ttl_ms(const iotdata_node_down_t *const ds) {
    return ds->ttl_ms;
}
static inline void iotdata_node_down_set_ttl_bcast_ms(iotdata_node_down_t *const ds, const uint32_t ttl_ms) {
    ds->ttl_bcast_ms = (ttl_ms > IOTDATA_NODE_DOWN_TTL_MS_MAX) ? (uint32_t)IOTDATA_NODE_DOWN_TTL_MS_MAX : ttl_ms;
}
static inline uint32_t iotdata_node_down_ttl_bcast_ms(const iotdata_node_down_t *const ds) {
    return ds->ttl_bcast_ms;
}
static inline void iotdata_node_down_set_repeat_ms(iotdata_node_down_t *const ds, const uint32_t repeat_ms) {
    ds->repeat_ms = (repeat_ms > IOTDATA_NODE_DOWN_TTL_MS_MAX) ? (uint32_t)IOTDATA_NODE_DOWN_TTL_MS_MAX : repeat_ms;
}
static inline uint32_t iotdata_node_down_repeat_ms(const iotdata_node_down_t *const ds) {
    return ds->repeat_ms;
}
static inline void iotdata_node_down_set_scan_ms(iotdata_node_down_t *const ds, const uint32_t scan_ms) {
    ds->scan_ms = scan_ms;
}
static inline uint32_t iotdata_node_down_scan_ms(const iotdata_node_down_t *const ds) {
    return ds->scan_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Wrap-safe: the ceiling above guarantees an interval under half the clock's range, so an unsigned
   difference orders correctly across a wrap. */
static inline bool _iotdata_node_down_older(const uint32_t now_ms, const uint32_t then_ms, const uint32_t ttl_ms) {
    return ttl_ms != 0u && (uint32_t)(now_ms - then_ms) >= ttl_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_down_station_t *iotdata_node_down_station_find(iotdata_node_down_t *const ds, const uint16_t station) {
    if (station == IOTDATA_STATION_BROADCAST)
        return &ds->station[0];
    for (int i = 1; i < IOTDATA_NODE_DOWN_STATIONS; i++)
        if (ds->station[i].station == station)
            return &ds->station[i];
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint32_t iotdata_node_down_ttl_of(const iotdata_node_down_t *const ds, const iotdata_node_down_station_t *const st) {
    return (st->station == IOTDATA_STATION_BROADCAST) ? ds->ttl_bcast_ms : ds->ttl_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_down_slot_release(iotdata_node_down_t *const ds, iotdata_node_down_slot_t *const sl) {
    if (sl->frame != BUFFER_NONE) {
        buffer_unref(ds->pool, sl->frame);
        sl->frame = BUFFER_NONE;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_down_slot_clear(iotdata_node_down_t *const ds, iotdata_node_down_slot_t *const sl) {
    _iotdata_node_down_slot_release(ds, sl);
    sl->hash = 0u;
    sl->arrived_ms = 0u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Does this station hold anything we could still put on the air for it? */
static inline bool iotdata_node_down_holds(iotdata_node_down_t *const ds, const uint16_t station) {
    const iotdata_node_down_station_t *const st = iotdata_node_down_station_find(ds, station);
    if (st != NULL)
        for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
            if (st->slot[j].frame != BUFFER_NONE)
                return true;
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Is a window open for this station right now, and how much of it is left? */
static inline uint32_t iotdata_node_down_window_remaining_ms(const iotdata_node_down_station_t *const st, const uint32_t now_ms) {
    if (!st->window_seen)
        return 0u;
    const uint32_t elapsed_ms = (uint32_t)(now_ms - st->window_at_ms);
    return (elapsed_ms < st->window_ms) ? (st->window_ms - elapsed_ms) : 0u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* An entry to reuse when the table is full: one with nothing left to send first, then the one whose
   last down is oldest. Never index 0. Losing an entry loses its dup memory as well as its frames,
   which narrows flood suppression under station pressure -- accepted, not a bug. */
static inline iotdata_node_down_station_t *_iotdata_node_down_station_evict(iotdata_node_down_t *const ds) {
    int best = -1;
    bool best_idle = false;
    for (int i = 1; i < IOTDATA_NODE_DOWN_STATIONS; i++) {
        const iotdata_node_down_station_t *const st = &ds->station[i];
        bool idle = true;
        for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS && idle; j++)
            if (st->slot[j].frame != BUFFER_NONE)
                idle = false;
        if (best < 0 || (idle && !best_idle) || (idle == best_idle && (int32_t)(st->down_last_ms - ds->station[best].down_last_ms) < 0)) {
            best = i;
            best_idle = idle;
        }
    }
    iotdata_node_down_station_t *const st = &ds->station[best];
    for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++) {
        _iotdata_node_down_slot_clear(ds, &st->slot[j]);
        st->slot[j].frame = BUFFER_NONE;
    }
    ds->stat_evicted_station++;
    return st;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_down_station_t *_iotdata_node_down_station_obtain(iotdata_node_down_t *const ds, const uint16_t station) {
    iotdata_node_down_station_t *st = iotdata_node_down_station_find(ds, station);
    if (st == NULL) {
        for (int i = 1; i < IOTDATA_NODE_DOWN_STATIONS; i++)
            if (ds->station[i].station == 0u) {
                ds->station[i].station = station;
                ds->count++;
                return &ds->station[i];
            }
        st = _iotdata_node_down_station_evict(ds);
        st->station = station;
    }
    return st;
}

static inline iotdata_node_down_slot_t *_iotdata_node_down_slot_obtain(iotdata_node_down_t *const ds, iotdata_node_down_station_t *const st) {
    int best = 0;
    for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++) {
        if (st->slot[j].hash == 0u)
            return &st->slot[j];
        if ((int32_t)(st->slot[j].arrived_ms - st->slot[best].arrived_ms) < 0)
            best = j;
    }
    ds->stat_evicted_slot++;
    _iotdata_node_down_slot_clear(ds, &st->slot[best]);
    return &st->slot[best];
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * A down frame arrived. Hash it; a hash we already hold for that target is a copy of our own flood
 * coming back, so throw it and say nothing. Otherwise record it and tell the caller to rebroadcast.
 *
 * Whether the BUFFER is kept is the only conditional part, and both of the cases that decline it
 * are cases where holding it could not help: a station we have never heard announce a window is
 * one we cannot deliver to at all, and a station whose window is open right now with room for the
 * frame has just been delivered to by the rebroadcast itself. A broadcast is never presumed
 * delivered that way -- there is no window for the broadcast id.
 */
static inline iotdata_node_down_ev_t iotdata_node_down_offer(iotdata_node_down_t *const ds, const uint16_t target, const uint8_t *const buf, const size_t len, const buffer_handle_t frame, const uint32_t bps, const uint32_t now_ms) {

    const uint32_t hash = iotdata_node_down_hash(buf, len);
    iotdata_node_down_station_t *st = iotdata_node_down_station_find(ds, target);
    if (st != NULL)
        for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
            if (st->slot[j].hash == hash) {
                ds->stat_dup++;
                return IOTDATA_NODE_DOWN_DUP;
            }

    if (st == NULL)
        st = _iotdata_node_down_station_obtain(ds, target);

    bool keep = buffer_valid(ds->pool, frame) && len > 0;
    if (keep && target != IOTDATA_STATION_BROADCAST) {
        const uint32_t remaining = iotdata_node_down_window_remaining_ms(st, now_ms);
        if (!st->window_seen)
            keep = false; /* unreachable from here: somebody else is its witness */
        else if (remaining > 0u && iotdata_node_receive_fits_ms(len, bps, remaining))
            keep = false; /* its window is open and the rebroadcast fits inside it */
    }

    iotdata_node_down_slot_t *const sl = _iotdata_node_down_slot_obtain(ds, st);
    sl->hash = hash;
    sl->arrived_ms = now_ms;
    sl->frame = keep ? buffer_ref(ds->pool, frame) : BUFFER_NONE;
    st->down_last_ms = now_ms;
    /*
     * THE BUFFER'S LENGTH IS SET FROM THE ONE WE WERE GIVEN, and that is this table's job rather
     * than politeness. A slot keeps no length of its own -- it keeps a handle -- so the delivery
     * path reads buffer_len() back out of the pool. That made the length TWO sources of truth: the
     * `len` handed in here, and whatever the caller had recorded on the handle.
     *
     * They disagreed, and it cost every held frame the gateway ever originated. node_send_down()
     * encoded into a pooled buffer, passed the length as this argument, and never called
     * buffer_set_len -- so the immediate transmission went out at the right size and the HELD copy
     * came back out at zero, was handed to the radio, refused for being empty, and marked
     * delivered. A command to a node that was not listening was silently dropped by the one
     * mechanism that exists to stop exactly that, and it went unnoticed because a relay floods the
     * same frame, holds it with a length its own receive path had set, and delivered it instead.
     *
     * Setting it here makes the two agree by construction: whoever fills a buffer may still record
     * the length (and should), but it can no longer be the only line that matters.
     */
    if (keep)
        buffer_set_len(ds->pool, frame, (uint16_t)len);

    if (keep)
        ds->stat_stored++;
    else
        ds->stat_tracked++;
    return keep ? IOTDATA_NODE_DOWN_STORED : IOTDATA_NODE_DOWN_TRACKED;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool _iotdata_node_down_sent_recently(const iotdata_node_down_t *const ds, const iotdata_node_down_station_t *const st, const uint32_t hash, const uint32_t now_ms) {
    for (int k = 0; k < IOTDATA_NODE_DOWN_SENT_SLOTS; k++)
        if (st->sent[k].hash == hash)
            return !_iotdata_node_down_older(now_ms, st->sent[k].sent_ms, ds->repeat_ms);
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_down_sent_record(iotdata_node_down_station_t *const st, const uint32_t hash, const uint32_t now_ms) {
    int best = 0;
    for (int k = 0; k < IOTDATA_NODE_DOWN_SENT_SLOTS; k++) {
        if (st->sent[k].hash == hash || st->sent[k].hash == 0u) {
            best = k;
            break;
        }
        if ((int32_t)(st->sent[k].sent_ms - st->sent[best].sent_ms) < 0)
            best = k;
    }
    st->sent[best].hash = hash;
    st->sent[best].sent_ms = now_ms;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * A station announced a receive window. Record it, then hand back ONE frame to put on the air --
 * the oldest by arrival across that station's own slots and the broadcast slots, skipping any
 * broadcast it has already been given, and skipping anything that will not fit the window's
 * remaining time.
 *
 * The returned handle carries a reference THE CALLER OWNS: submit it and release it, exactly as
 * with tx_alloc. The table has already done its own bookkeeping -- a unicast frame is released
 * here and its hash kept, a broadcast frame is kept and this station recorded as having had it.
 */
static inline buffer_handle_t iotdata_node_down_window(iotdata_node_down_t *const ds, const uint16_t station, const iotdata_node_receive_t *const rx, const uint32_t bps, const uint32_t now_ms, size_t *const len_out) {

    if (station == IOTDATA_STATION_BROADCAST || station == 0u)
        return BUFFER_NONE; /* nobody transmits as either, so neither can announce */

    iotdata_node_down_station_t *const st = _iotdata_node_down_station_obtain(ds, station);
    st->window_seen = true;
    st->window_ms = (uint32_t)iotdata_node_receive_window_s(rx) * 1000u; /* the one conversion: seconds arrive, milliseconds are kept */
    /* THE STAMP IS WHEN THE WINDOW OPENS, not when we heard about it. A stated DELAY_S puts that in
       the future, and iotdata_node_down_window_remaining_ms() then answers 0 until it arrives, so a
       frame is HELD instead of being transmitted at a node that is not listening yet. The other half
       -- waking at that instant to send, rather than only on the next snoop -- is not built, so a
       delayed window currently costs a delivery rather than earning one. */
    st->window_at_ms = now_ms + iotdata_node_receive_delay_s(rx) * 1000u;

    if (!iotdata_node_receive_accepts(rx, IOTDATA_NODE_DOWN_ASSUMED_TYPE))
        return BUFFER_NONE;

    /* Oldest first, across both sources, taking the first that fits: a frame too big for what is
       left of the window must not block a smaller one behind it.
     *
     * SET ASIDE, NOT ALTERED. A candidate that does not fit is masked off for THIS CALL ONLY, which
       is the only way the search can terminate: it was previously excluded by making the frame look
       newly arrived (and a broadcast, by recording it as sent). Neither actually removed it from
       candidacy -- a lone frame too big for the window was picked, re-stamped and picked again,
       forever -- and both lied about the queue afterwards: a frame that did not fit lost its place
       in arrival order, and a broadcast that did not fit went unsent for a whole repeat interval.
       Nothing here mutates now, so what did not fit this window keeps its place for the next.
     *
     * ONE FLAG PER SLOT, not a bitmask. A uint32_t bitmask was the first shape of this and it was
       wrong: IOTDATA_NODE_DOWN_SLOTS is the application's, and the gateway sets it to 128 --
       so a mask silently addresses only the first 32 of them and the search stops terminating
       again for anything above that. Sized off the macro, it cannot be outgrown. */
    bool aside_station[IOTDATA_NODE_DOWN_SLOTS] = { false }, aside_bcast[IOTDATA_NODE_DOWN_SLOTS] = { false };
    for (;;) {
        iotdata_node_down_station_t *from = NULL;
        iotdata_node_down_slot_t *best = NULL;
        int best_j = -1;
        for (int pass = 0; pass < 2; pass++) {
            iotdata_node_down_station_t *const src = (pass == 0) ? st : &ds->station[0];
            const bool *const aside = (pass == 0) ? aside_station : aside_bcast;
            for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++) {
                iotdata_node_down_slot_t *const sl = &src->slot[j];
                if (sl->frame != BUFFER_NONE && !aside[j])
                    if (!(pass == 1 && _iotdata_node_down_sent_recently(ds, st, sl->hash, now_ms)))
                        if (best == NULL || (int32_t)(sl->arrived_ms - best->arrived_ms) < 0) {
                            best = sl;
                            best_j = j;
                            from = src;
                        }
            }
        }
        if (best == NULL)
            return BUFFER_NONE; /* nothing left that fits -- including a window that has not opened yet */

        const size_t len = buffer_len(ds->pool, best->frame);
        if (iotdata_node_receive_fits_ms(len, bps, iotdata_node_down_window_remaining_ms(st, now_ms))) {
            const buffer_handle_t out = buffer_ref(ds->pool, best->frame);
            if (len_out != NULL)
                *len_out = len;
            if (from == &ds->station[0]) {
                _iotdata_node_down_sent_record(st, best->hash, now_ms); /* kept: other stations still want it */
                ds->stat_delivered_bcast++;
            } else {
                _iotdata_node_down_slot_release(ds, best); /* sent once; the hash stays behind */
                ds->stat_delivered++;
            }
            return out;
        } else {
            if (from == &ds->station[0])
                aside_bcast[best_j] = true;
            else
                aside_station[best_j] = true;
        }
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * Expiry, driven from the caller's loop. Nothing here expires on its own, so a table that is never
 * ticked never lets anything go. Returns how many slots were discarded.
 */
static inline int iotdata_node_down_tick(iotdata_node_down_t *const ds, const uint32_t now_ms) {

    if (ds->scan_ms != 0u && ds->scan_last_ms != 0u && (uint32_t)(now_ms - ds->scan_last_ms) < ds->scan_ms)
        return 0;
    ds->scan_last_ms = (now_ms == 0u) ? 1u : now_ms;

    int expired = 0;
    for (int i = 0; i < IOTDATA_NODE_DOWN_STATIONS; i++) {
        iotdata_node_down_station_t *const st = &ds->station[i];
        if (st->station != 0u) {
            const uint32_t ttl_ms = iotdata_node_down_ttl_of(ds, st);
            bool empty = true;
            for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
                if (st->slot[j].hash != 0u) {
                    if (_iotdata_node_down_older(now_ms, st->slot[j].arrived_ms, ttl_ms)) {
                        _iotdata_node_down_slot_clear(ds, &st->slot[j]);
                        ds->stat_expired++;
                        expired++;
                    } else
                        empty = false;
                }
            /* the record of having given a broadcast to this station expires too, and THAT is what
            makes the broadcast eligible again: the cyclic resend, explicit half */
            for (int k = 0; k < IOTDATA_NODE_DOWN_SENT_SLOTS; k++)
                if (st->sent[k].hash != 0u && _iotdata_node_down_older(now_ms, st->sent[k].sent_ms, ds->repeat_ms))
                    st->sent[k].hash = 0u;
            /* an entry with nothing held and nothing known is not worth an entry; a window we have
            snooped IS worth keeping, since it is what makes the station deliverable at all */
            if (empty && !st->window_seen && i != 0) {
                st->station = 0u;
                ds->count--;
            }
        }
    }
    return expired;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_down_clear(iotdata_node_down_t *const ds) {
    int n = 0;
    for (int i = 0; i < IOTDATA_NODE_DOWN_STATIONS; i++) {
        for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
            if (ds->station[i].slot[j].hash != 0u) {
                _iotdata_node_down_slot_clear(ds, &ds->station[i].slot[j]);
                n++;
            }
        for (int k = 0; k < IOTDATA_NODE_DOWN_SENT_SLOTS; k++)
            ds->station[i].sent[k].hash = 0u;
        if (i != 0)
            ds->station[i].station = 0u;
    }
    ds->count = 1;
    return n;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* For a report that wants to show what is held for a station it is already printing. */
static inline int iotdata_node_down_station_count(const iotdata_node_down_t *const ds, const uint16_t station, int *const out_tracked) {
    int held = 0, tracked = 0;
    /* its own scan rather than the finder: a const table cannot hand back a mutable entry */
    for (int i = 0; i < IOTDATA_NODE_DOWN_STATIONS; i++) {
        const iotdata_node_down_station_t *const st = &ds->station[i];
        if (st->station == station) {
            for (int j = 0; j < IOTDATA_NODE_DOWN_SLOTS; j++)
                if (st->slot[j].hash != 0u) {
                    tracked++;
                    if (st->slot[j].frame != BUFFER_NONE)
                        held++;
                }
            break;
        }
    }
    if (out_tracked != NULL)
        *out_tracked = tracked;
    return held;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The window this station last advertised, or 0 if we have never snooped one -- which is the same
   question as "can we deliver to it at all". Const, so a report can ask. */
static inline uint32_t iotdata_node_down_station_window_ms(const iotdata_node_down_t *const ds, const uint16_t station) {
    for (int i = 0; i < IOTDATA_NODE_DOWN_STATIONS; i++)
        if (ds->station[i].station == station)
            return ds->station[i].window_seen ? ds->station[i].window_ms : 0u;
    return 0u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_DOWN_H */
