#ifndef IOTDATA_NODE_DIAGNOSTICS_H
#define IOTDATA_NODE_DIAGNOSTICS_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_diagnostics.h - what a node RECORDED: the single source of truth for DIAGNOSTICS.
//
// THE ADAPTER binds the blackbox recorder (iotdata-depend/blackbox) to iotdata: the backend for
// this platform, the clock, and the one record type every node shares -- a lifecycle event. That
// part is unchanged; only the file name is.
//
// THE RECORDER is the scaffolding each app used to carry itself: a pool, a handle, a config, a
// did-it-start flag, start/event/flush, a tick, the node's DIAGNOSTICS and CONTROL hooks, and the
// console words. Three apps had ~60 near-identical lines of it, and they had drifted -- one flushed
// on a timer and two by hand, two declared the blackbox capability in VERSION and one did not, and
// none of them guarded a handle whose backend had failed to initialise. Turning the recorder on is
// now one #define.
//
//     #define IOTDATA_NODE_DIAGNOSTICS 2            /* before including this header */
//     ...
//     iotdata_node_diagnostics_begin(reason, cold); /* once, at startup */
//     iotdata_node_diagnostics_tick(now_ms);        /* each loop pass; flushes on its own schedule */
//
// and the node's hooks are iotdata_node_diagnostics_pull / _control / _control_actions, passed
// straight to node_begin or into an iotdata_node_params_t.
//
// EVERY app-facing call COMPILES TO NOTHING when IOTDATA_NODE_DIAGNOSTICS is 0. They are functions
// rather than macros so that the disabled build still type-checks its call sites -- a macro that
// expands to ((void)0) accepts arguments that no longer exist. Nothing in an application needs an
// #if around it.
//
// A NULL BACKEND IS NOT A DISABLED RECORDER. blackbox_init leaves a NULL backend inside a non-NULL
// handle when it fails -- no `diag` partition, an unwritable path -- and everything downstream
// dereferences it. That is a boot fault, not a degradation, and it is guarded once here instead of
// at each of the ten places that reach the store.
//
// SINGLETON, ON PURPOSE. One recorder per board, because the records are the board's: a simulator
// standing up eight virtual sensors has one log, and a request to any of its stations reads it.
// Include this in the app's unity TU, as its predecessor required.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Off unless an application says otherwise. The value chooses how far a record survives:
     0  off, compiled out entirely
     1  RAM only    -- an RTC_NOINIT ring: survives deep sleep and a watchdog or panic reset, but
                       not a power cycle
     2  RAM backed  -- the ring is written through to the `diag` partition on esp32, or to a file
                       on a host, so records outlive a power loss
   A host application whose recorder is configured at RUN TIME -- the gateway, out of its config
   file -- still uses the recorder below: see iotdata_node_diagnostics_begin_conf. It used to be
   told to leave this at 0 and drive a handle of its own, which is how the gateway ended up with
   two recorders, one of them dormant, and a tick of its own that read the wall clock. */
#ifndef IOTDATA_NODE_DIAGNOSTICS
#define IOTDATA_NODE_DIAGNOSTICS 0
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ADAPTER: backend, sizes and clock
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * WHERE THE RECORDS GO, asked as a capability rather than as a platform. Level 1 is the ring alone;
 * level 2 writes it through to whatever durable store the platform in front of us has, and
 * d_platform_<platform>.h is what says which that is. An application that wants something else
 * defines BLACKBOX_PERSIST itself and none of this runs.
 */
#ifndef BLACKBOX_PERSIST
#if IOTDATA_NODE_DIAGNOSTICS >= 2
#if defined(D_PERSIST_STORE_PARTITION)
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_ESP_FLASH
#elif defined(D_PERSIST_STORE_FILE)
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_FILE
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE /* nowhere durable: the ring is all there is */
#endif
#else
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE /* the reset-surviving ring, and nothing behind it */
#endif
#endif

#ifndef IOTDATA_NODE_BLACKBOX_POOL_SZ
#ifdef D_PERSIST_POOL_SZ_DEFAULT
#define IOTDATA_NODE_BLACKBOX_POOL_SZ D_PERSIST_POOL_SZ_DEFAULT
#else
#define IOTDATA_NODE_BLACKBOX_POOL_SZ 2048u /* the smaller of the two: a ring that fits anywhere */
#endif
#endif

#ifndef BLACKBOX_CLOCK
#define BLACKBOX_CLOCK iotdata_node_blackbox_clock
#endif

/* stb-style: BLACKBOX_IMPLEMENTATION must be set BEFORE the first blackbox.h include. */
#ifdef IOTDATA_NODE_BLACKBOX_IMPLEMENTATION
#ifndef BLACKBOX_IMPLEMENTATION
#define BLACKBOX_IMPLEMENTATION
#endif
#endif
#include "blackbox.h"

/* -------- the one shared record: a lifecycle event -------------------------------------------- */
typedef enum {
    IOTDATA_NODE_BB_LC_BOOT = 0, /* cold boot / power-on                     */
    IOTDATA_NODE_BB_LC_START,    /* app started (post-init)                  */
    IOTDATA_NODE_BB_LC_STOP,     /* app stopping / shutdown                  */
    IOTDATA_NODE_BB_LC_SLEEP,    /* entering (deep) sleep                    */
    IOTDATA_NODE_BB_LC_WAKE,     /* woke from sleep                          */
    IOTDATA_NODE_BB_LC_RESET,    /* reset (reason in `reason`)               */
    IOTDATA_NODE_BB_LC_ERROR,    /* error / fault (code in `reason`)         */
} iotdata_node_bb_lc_event_t;

// @blackbox tag=LC
typedef struct {
    uint8_t event;
    uint8_t reason;
} iotdata_node_bb_lifecycle_t;

extern const blackbox_struct_config_t iotdata_node_blackbox_config_lifecycle;

/* -------- power: what is supplying this node, and what that supply is doing -------------------
 *
 * A node has RAILS, not "a power supply": a panel feeds a charger feeds a pack feeds a regulator
 * feeds the device, and each is a separate thing with separate facts. Every record below carries a
 * rail index, and PWS carries from_rail, so the log describes the actual graph rather than one
 * anonymous supply. Three records, by how often they have anything new to say:
 *
 *   PWS  what a rail IS, and how it is measured. Once per boot, and on change.
 *   PWD  one source-specific fact as key=value. The escape hatch: BMS topology today, per-cell
 *        voltages tomorrow, vendor detail forever -- no schema change, no length ceiling. A key
 *        that turns out permanent and load-bearing graduates to a field in PWS or PW.
 *   PWE  what a rail is DOING. The frequent one, and the event says why the line exists.
 *
 * Two kinds of rail, and the distinction decides which fields mean anything: a STORAGE source has
 * a volume (capacity_mah, pct), a FLOW source has only a rate (limit_mw, and pct is meaningless).
 * A REGULATED rail is neither -- it is an output, and what is interesting is what feeds it.
 *
 * Absent fields are EMPTY in the encoded line, not zero. A recorder that cannot say "not measured"
 * invites a reader to believe a zero, and most of these fields are unmeasurable on most hardware.
 * Initialise with IOTDATA_NODE_BB_POWER_EVENT_INIT / _SOURCE_INIT so a struct never claims 0V by omission. */

#define IOTDATA_NODE_BB_PW_NO_I32  INT32_MIN
#define IOTDATA_NODE_BB_PW_NO_U32  0u /* a capacity or a rating of zero is not a fact */
#define IOTDATA_NODE_BB_PW_NO_PCT  0xFFu
#define IOTDATA_NODE_BB_PW_NO_DEGC INT8_MIN
#define IOTDATA_NODE_BB_PW_NO_RAIL 0xFFu /* and, in from_rail, "nothing feeds this: it is a root" */

typedef enum {
    IOTDATA_NODE_BB_PW_SAMPLE = 0,    /* a periodic reading (emitted on change, see the note below) */
    IOTDATA_NODE_BB_PW_BOOT,          /* the first reading of this session                         */
    IOTDATA_NODE_BB_PW_LOW,           /* crossed into low                                          */
    IOTDATA_NODE_BB_PW_CRITICAL,      /* crossed into critical                                     */
    IOTDATA_NODE_BB_PW_MINIMUM,       /* a new lowest reading ever seen on this rail               */
    IOTDATA_NODE_BB_PW_BROWNOUT_PRE,  /* the last reading BEFORE a brownout: where the floor IS     */
    IOTDATA_NODE_BB_PW_CHARGE_START,  /* began taking charge                                       */
    IOTDATA_NODE_BB_PW_CHARGE_STOP,   /* stopped                                                   */
    IOTDATA_NODE_BB_PW_SOURCE_CHANGE, /* a rail appeared, vanished, or was swapped                 */
    IOTDATA_NODE_BB_PW_FAULT,         /* the source, or the measurement of it, is faulty           */
} iotdata_node_bb_pw_event_t;

typedef enum {
    IOTDATA_NODE_BB_PW_TYPE_UNKNOWN = 0,
    IOTDATA_NODE_BB_PW_TYPE_BATTERY,   /* storage */
    IOTDATA_NODE_BB_PW_TYPE_SUPERCAP,  /* storage */
    IOTDATA_NODE_BB_PW_TYPE_SOLAR,     /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_MAINS,     /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_USB,       /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_POE,       /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_GENERATOR, /* flow    */
    IOTDATA_NODE_BB_PW_TYPE_REGULATED, /* neither: an output rail, fed by from_rail */
} iotdata_node_bb_pw_type_t;

#define IOTDATA_NODE_BB_PW_FORM_DC 0
#define IOTDATA_NODE_BB_PW_FORM_AC 1

/* Chemistry of a storage rail. Its own numbering, with 0 = "not a chemistry": a caller maps its
   own battery type onto this rather than assuming the two agree. */
typedef enum {
    IOTDATA_NODE_BB_PW_CHEM_NONE = 0,
    IOTDATA_NODE_BB_PW_CHEM_LIION,
    IOTDATA_NODE_BB_PW_CHEM_LIPO,
    IOTDATA_NODE_BB_PW_CHEM_LIFEPO4,
    IOTDATA_NODE_BB_PW_CHEM_NIMH,
    IOTDATA_NODE_BB_PW_CHEM_LEAD,
    IOTDATA_NODE_BB_PW_CHEM_ALKALINE,
} iotdata_node_bb_pw_chem_t;

#define IOTDATA_NODE_BB_PW_FLAG_PRESENT     (1U << 0) /* a source is actually attached                */
#define IOTDATA_NODE_BB_PW_FLAG_CHARGING    (1U << 1)
#define IOTDATA_NODE_BB_PW_FLAG_DISCHARGING (1U << 2)
#define IOTDATA_NODE_BB_PW_FLAG_EXTERNAL    (1U << 3) /* running on something other than the storage  */
#define IOTDATA_NODE_BB_PW_FLAG_LOW         (1U << 4)
#define IOTDATA_NODE_BB_PW_FLAG_CRITICAL    (1U << 5)
#define IOTDATA_NODE_BB_PW_FLAG_LIMITED     (1U << 6) /* at the source's RATING, not at its capacity  */
#define IOTDATA_NODE_BB_PW_FLAG_FAULT       (1U << 7)

// @blackbox tag=PWE
typedef struct {
    uint8_t event; /* iotdata_node_bb_pw_event_t                                                     */
    uint8_t rail;
    int32_t mv; /* signed, and wide: negative rails exist and 230V mains is 230000              */
    int32_t ua; /* signed: + flowing INTO the load, - INTO the source (charging). uA to 2147A   */
    int32_t mw; /* signed, same convention. Kept although V*I: on AC it is NOT derivable        */
    uint8_t pct;
    int8_t degc;   /* the SOURCE's temperature: its capacity and impedance both move with it    */
    uint8_t flags; /* IOTDATA_NODE_BB_PW_FLAG_*                                                      */
} iotdata_node_bb_power_event_t;

// @blackbox tag=PWS
typedef struct {
    uint8_t rail;
    uint8_t from_rail; /* what feeds this one, or IOTDATA_NODE_BB_PW_NO_RAIL for a root */
    uint8_t type;      /* iotdata_node_bb_pw_type_t */
    uint8_t form;      /* IOTDATA_NODE_BB_PW_FORM_* */
    uint8_t hz;        /* AC only */
    uint8_t chem;      /* storage only */
    uint8_t cells;     /* storage only: cells in series */
    int32_t nominal_mv, min_mv, max_mv;
    uint32_t capacity_mah; /* STORAGE: the volume  */
    uint32_t limit_mw;     /* FLOW: the rating     */
    uint16_t ratio_x100;   /* how it is measured.. */
    int16_t offset_mv;     /* ..and its calibration, so the numbers are still trustworthy later */
} iotdata_node_bb_power_source_t;

// @blackbox tag=PWD
typedef struct {
    uint8_t rail;
    char key[16];
    char value[48]; /* last in the line, so it may contain commas */
} iotdata_node_bb_power_detail_t;

#define IOTDATA_NODE_BB_POWER_EVENT_INIT(rail_) \
    { .event = IOTDATA_NODE_BB_PW_SAMPLE, .rail = (rail_), .mv = IOTDATA_NODE_BB_PW_NO_I32, .ua = IOTDATA_NODE_BB_PW_NO_I32, .mw = IOTDATA_NODE_BB_PW_NO_I32, .pct = IOTDATA_NODE_BB_PW_NO_PCT, .degc = IOTDATA_NODE_BB_PW_NO_DEGC, .flags = 0 }

#define IOTDATA_NODE_BB_POWER_SOURCE_INIT(rail_, type_) \
    { .rail = (rail_), \
      .from_rail = IOTDATA_NODE_BB_PW_NO_RAIL, \
      .type = (uint8_t)(type_), \
      .form = IOTDATA_NODE_BB_PW_FORM_DC, \
      .hz = 0, \
      .chem = IOTDATA_NODE_BB_PW_CHEM_NONE, \
      .cells = 0, \
      .nominal_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .min_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .max_mv = IOTDATA_NODE_BB_PW_NO_I32, \
      .capacity_mah = IOTDATA_NODE_BB_PW_NO_U32, \
      .limit_mw = IOTDATA_NODE_BB_PW_NO_U32, \
      .ratio_x100 = 0, \
      .offset_mv = 0 }

extern const blackbox_struct_config_t iotdata_node_blackbox_config_power;
extern const blackbox_struct_config_t iotdata_node_blackbox_config_power_source;
extern const blackbox_struct_config_t iotdata_node_blackbox_config_power_detail;

/* iotdata_node_blackbox_clock (the BLACKBOX_CLOCK hook) is already forward-declared by blackbox.h. */

/* Seed the clock. Call ONCE before blackbox_init(). The clock's `seq` lives in the reset-surviving
 * domain, so this zeroes it on a cold start -- detected with a magic word, and see the
 * implementation for why a reset-reason test cannot do that job. iotdata_node_diagnostics_begin
 * calls it, so an app driving the singleton never needs to. */
void iotdata_node_blackbox_begin(void);

/* The alternative BLACKBOX_CLOCK: wall clock milliseconds, for records that will be read beside
   something else's log. An application chooses it with `#define BLACKBOX_CLOCK
   iotdata_node_blackbox_clock_wall` before this header; the default is the run-ordered one above,
   which needs no clock to be set and is the same on every platform.

   THE SUPPRESSION IS LOAD-BEARING. blackbox.h forward-declares whatever name BLACKBOX_CLOCK
   expands to, so in an application that selected THIS one, that declaration and this one are the
   same declaration twice and -Wredundant-decls refuses the build -- which is exactly how the
   gateway broke. The preprocessor cannot compare a macro against an identifier, so the duplicate
   cannot be avoided, only excused: for this one line, and for no other. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
int iotdata_node_blackbox_clock_wall(char *out, size_t n);
#pragma GCC diagnostic pop

/* Convenience: record a lifecycle event against any handle. */
static inline int iotdata_node_blackbox_lifecycle(blackbox_handle_t *h, iotdata_node_bb_lc_event_t ev, uint8_t reason) {
    return blackbox_insert(h, &iotdata_node_blackbox_config_lifecycle, &(const iotdata_node_bb_lifecycle_t){ (uint8_t)ev, reason });
}

/* Convenience: record a power state, a rail description, or one detail, against any handle. */
static inline int iotdata_node_blackbox_power(blackbox_handle_t *h, const iotdata_node_bb_power_event_t *const r) {
    return blackbox_insert(h, &iotdata_node_blackbox_config_power, r);
}

static inline int iotdata_node_blackbox_power_source(blackbox_handle_t *h, const iotdata_node_bb_power_source_t *const r) {
    return blackbox_insert(h, &iotdata_node_blackbox_config_power_source, r);
}

static inline int iotdata_node_blackbox_power_detail(blackbox_handle_t *h, const uint8_t rail, const char *const key, const char *const value) {
    iotdata_node_bb_power_detail_t r = { .rail = rail, .key = { 0 }, .value = { 0 } };
    (void)snprintf(r.key, sizeof(r.key), "%s", key != NULL ? key : "");
    (void)snprintf(r.value, sizeof(r.value), "%s", value != NULL ? value : "");
    return blackbox_insert(h, &iotdata_node_blackbox_config_power_detail, &r);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE RECORDER
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Where a line of output goes. The same seam the rest of the framework uses: nothing here logs on
   its own, the application says where a line belongs. It is a CURRENT emitter rather than an
   argument on every call because that is how it is actually used -- a node points it at the log
   and a console points it at itself for the length of one command, and a store dumped a chunk per
   loop pass outlives whichever call started it. */
typedef void (*iotdata_node_diagnostics_emit_fn)(const char *line);

/* A lifecycle event's `reason` is ONE BYTE, and an application usually has a wider counter to put
   in it. Narrow it through here rather than with a cast: a cast turns 300 failures into 44, which
   reads as a smaller problem than it is, where a pegged 255 reads as "at least this many". The old
   BLACKBOX_EVENT macro cast internally, which is why no call site had to think about it. */
static inline uint8_t iotdata_node_diagnostics_reason(const uint32_t count) {
    return (uint8_t)((count > 255u) ? 255u : count);
}

#if IOTDATA_NODE_DIAGNOSTICS

#ifndef IOTDATA_NODE_DIAGNOSTICS_PARTITION
#define IOTDATA_NODE_DIAGNOSTICS_PARTITION "diag" /* esp32: the partition label. host: the file path. */
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_FLUSH
#define IOTDATA_NODE_DIAGNOSTICS_FLUSH BLACKBOX_FLUSH_BATCH_TIME
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS
#define IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS 60000u /* only consulted by the BATCH_TIME policy */
#endif
/* Whether this node services DIAGNOSTICS_DUMP, which prints the store on the node's own console.
   ON by default -- every node has one, and a sensor on the bench over USB is exactly when you want
   it. The obligation that comes with it is iotdata_node_diagnostics_tick() every loop pass: a dump
   is drained a chunk at a time, so a node that advertises DUMP and never ticks accepts the command
   and then produces nothing. Turn it OFF on a node that cannot tick -- and it then disappears from
   the advertised list as well as the handler, so the node is not promising what it will not do. */
#ifndef IOTDATA_NODE_DIAGNOSTICS_DUMP
#define IOTDATA_NODE_DIAGNOSTICS_DUMP 1
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK
#define IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK 16 /* records per loop pass, so a dump cannot stall the loop */
#endif
#ifndef IOTDATA_NODE_DIAGNOSTICS_STAT_MAX
#define IOTDATA_NODE_DIAGNOSTICS_STAT_MAX 192
#endif
/* Room for the store's name when it is given at run time: a partition label is tiny, a path is
   not, and a path that was silently truncated would write the records to the wrong file. */
#ifndef IOTDATA_NODE_DIAGNOSTICS_PERSIST_MAX
#define IOTDATA_NODE_DIAGNOSTICS_PERSIST_MAX 256u
#endif
/* Whether a DEVICE records from boot. ON in a development build, OFF in a release one (D_BUILD_RELEASE):
   compiled in and advertised either way, but a field node does not record until it is told to --
   and until then the recorder is not STARTED at all, because on ESP_FLASH starting is a scan of the
   whole partition, and a node that deep sleeps starts at every wake. A host says for itself, through
   iotdata_node_diagnostics_begin_conf. */
#ifndef IOTDATA_NODE_DIAGNOSTICS_ENABLED
#if defined(D_BUILD_RELEASE) && D_BUILD_RELEASE
#define IOTDATA_NODE_DIAGNOSTICS_ENABLED false
#else
#define IOTDATA_NODE_DIAGNOSTICS_ENABLED true
#endif
#endif

/* RTC_NOINIT_ATTR, so the pool survives a reset and a panic is still readable afterwards. No
   platform test and no esp_attr.h include: the attribute comes from the includer's d_platform_*
   header, where it is the real thing on esp32 and nothing at all on a host -- which is exactly the
   difference, since a host that restarts has no RTC domain to survive in. */
static RTC_NOINIT_ATTR char _iotdata_node_diagnostics_pool[IOTDATA_NODE_BLACKBOX_POOL_SZ];
static blackbox_handle_t _iotdata_node_diagnostics_handle;
static bool _iotdata_node_diagnostics_ready = false;
static char _iotdata_node_diagnostics_line[IOTDATA_NODE_DIAGNOSTICS_STAT_MAX];
static char _iotdata_node_diagnostics_rec[BLACKBOX_LINE_MAX];
static uint32_t _iotdata_node_diagnostics_ticked_ms = 0;
static iotdata_node_diagnostics_emit_fn _iotdata_node_diagnostics_emit = NULL;
static struct {
    bool active;
    size_t cursor;
    int count;
} _iotdata_node_diagnostics_dump;

/* NOT const, because iotdata_node_diagnostics_begin_conf writes it before init. The handle keeps a
   POINTER to this, so it has to be here and not on anybody's stack. */
static blackbox_config_t _iotdata_node_diagnostics_config = {
    .pool = _iotdata_node_diagnostics_pool,
    .pool_sz = sizeof(_iotdata_node_diagnostics_pool),
    .flush = IOTDATA_NODE_DIAGNOSTICS_FLUSH,
    .flush_ms = IOTDATA_NODE_DIAGNOSTICS_FLUSH_MS,
    .persist_arg = IOTDATA_NODE_DIAGNOSTICS_PARTITION,
    .enabled = IOTDATA_NODE_DIAGNOSTICS_ENABLED,
};

/* And the store's name is copied in, for the same reason: the FILE backend keeps the POINTER it is
   given rather than a copy of the string, so a path composed on a caller's stack would dangle the
   moment that caller returned. Copying it here means a caller may compose one and forget it. */
static char _iotdata_node_diagnostics_persist[IOTDATA_NODE_DIAGNOSTICS_PERSIST_MAX];

/* True once the store exists. Everything below asks first, because a failed init leaves a handle
   whose backend is NULL, and reaching through it faults rather than doing nothing. */
/* Where output goes from now on; returns what it was, so a console can put it back. NULL discards,
   which is what a node with nowhere to print wants. */
static inline iotdata_node_diagnostics_emit_fn iotdata_node_diagnostics_emit_set(const iotdata_node_diagnostics_emit_fn fn) {
    const iotdata_node_diagnostics_emit_fn was = _iotdata_node_diagnostics_emit;
    _iotdata_node_diagnostics_emit = fn;
    return was;
}

static inline void _iotdata_node_diagnostics_say(const char *const line) {
    if (_iotdata_node_diagnostics_emit != NULL)
        _iotdata_node_diagnostics_emit(line);
}

static inline bool iotdata_node_diagnostics_ready(void) {
    return _iotdata_node_diagnostics_ready;
}

/* For records of an application's own kind: blackbox_insert(iotdata_node_diagnostics_handle(), ...).
   NULL when the recorder did not start, which a caller must check as it would any handle. */
static inline blackbox_handle_t *iotdata_node_diagnostics_handle(void) {
    return _iotdata_node_diagnostics_ready ? &_iotdata_node_diagnostics_handle : NULL;
}

/* Start the recorder and log why we are running. `cold` distinguishes a boot from a wake, which is
   the difference between the two lifecycle events a sleeping node alternates between. Returns
   false if the store could not be opened -- worth logging, but never fatal: a node with no
   recorder still answers a diagnostics request, emptily. */
static inline bool _iotdata_node_diagnostics_start(const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    iotdata_node_blackbox_begin(); /* seed the clock BEFORE init: see the note on the magic word */
    if ((_iotdata_node_diagnostics_ready = (blackbox_init(&_iotdata_node_diagnostics_handle, &_iotdata_node_diagnostics_config) == 0)))
        (void)iotdata_node_blackbox_lifecycle(&_iotdata_node_diagnostics_handle, ev, reason);
    return _iotdata_node_diagnostics_ready;
}

static inline bool iotdata_node_diagnostics_begin(const uint8_t reason, const bool cold) {
    if (!_iotdata_node_diagnostics_config.enabled)
        return true; /* off from boot, by choice, not failure: started only when asked to be -- see IOTDATA_NODE_DIAGNOSTICS_ENABLED */
    return _iotdata_node_diagnostics_start(cold ? IOTDATA_NODE_BB_LC_BOOT : IOTDATA_NODE_BB_LC_WAKE, reason);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// A RECORDER CONFIGURED AT RUN TIME, FOR A HOST THAT IS TOLD WHERE TO PUT IT
//
// A device's recorder is settled when the firmware is built: the partition is in the partition
// table, the pool is in RTC RAM, and the flush policy is a property of how that node sleeps. A
// host is not. The gateway is told its directory, its bounds and whether to record at all, out of
// the same config file as everything else, and none of that is knowable at compile time.
//
// SO THE CONFIG IS WRITTEN ONCE, HERE, BEFORE init -- not held by the caller. It used to be the
// caller's, which meant the caller also owned the handle, the pool and the tick, and a second
// recorder existed in parallel with this one. What it kept was a pool, a 512-byte path and a
// clock; what it got wrong was the clock. There is one recorder on a node, and this is how an
// application that cannot name its store at compile time still uses it.
//
// THE LIFECYCLE EVENT IS THE CALLER'S TO NAME, unlike iotdata_node_diagnostics_begin, which infers
// BOOT or WAKE from `cold`. Neither is right for a process: a gateway that restarts has not booted
// anything and has not woken from anything, it has STARTed, and the host it runs on may have been
// up for months. The caller knows which of those happened and this cannot.
//
// A TRUNCATED PATH IS A REFUSAL, not a shrug. Records written to a path that is nearly the one
// asked for are records nobody will look for, so an over-long one fails to start and says so --
// the same verdict as an unwritable directory, which is what it amounts to.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool enabled;            /* whether to record at all; blackbox_enable still moves it later    */
    const char *persist_arg; /* FILE: the path. ESP_FLASH: the partition label. Copied, not kept.  */
    blackbox_flush_t flush;
    uint32_t flush_ms;               /* BATCH_TIME only                                         */
    uint32_t max_records, max_bytes; /* 0 = unbounded on that axis                              */
    uint8_t generations;             /* FILE at max_bytes: how many rotated backups to keep     */
} iotdata_node_diagnostics_conf_t;

static inline bool iotdata_node_diagnostics_begin_conf(const iotdata_node_diagnostics_conf_t *const conf, const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    if (conf == NULL)
        return false;
    const char *const arg = (conf->persist_arg != NULL) ? conf->persist_arg : IOTDATA_NODE_DIAGNOSTICS_PARTITION;
    if (strlen(arg) >= sizeof(_iotdata_node_diagnostics_persist)) {
        _iotdata_node_diagnostics_ready = false;
        return false; /* see A TRUNCATED PATH IS A REFUSAL */
    }
    (void)snprintf(_iotdata_node_diagnostics_persist, sizeof(_iotdata_node_diagnostics_persist), "%s", arg);
    _iotdata_node_diagnostics_config.flush = conf->flush;
    _iotdata_node_diagnostics_config.flush_ms = conf->flush_ms;
    _iotdata_node_diagnostics_config.max_records = conf->max_records;
    _iotdata_node_diagnostics_config.max_bytes = conf->max_bytes;
    _iotdata_node_diagnostics_config.generations = conf->generations;
    _iotdata_node_diagnostics_config.persist_arg = _iotdata_node_diagnostics_persist;
    _iotdata_node_diagnostics_config.enabled = conf->enabled;
    return _iotdata_node_diagnostics_start(ev, reason);
}

/* The store's name as it was actually opened, for the line an application logs at startup. */
static inline const char *iotdata_node_diagnostics_persist_arg(void) {
    return _iotdata_node_diagnostics_config.persist_arg;
}

/* STOPPING, which only a host does. A device's recorder ends when the power does, and there is
   nothing to hand back; a process exits, and the backend it opened was a malloc. Records the event
   first, then flushes, so the last thing in the store is the fact that the store was closed
   properly -- which is how a reader tells a clean shutdown from the kill that left no such line. */
static inline void iotdata_node_diagnostics_end(const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    if (_iotdata_node_diagnostics_ready) {
        (void)iotdata_node_blackbox_lifecycle(&_iotdata_node_diagnostics_handle, ev, reason);
        (void)blackbox_flush(&_iotdata_node_diagnostics_handle);
        blackbox_deinit(&_iotdata_node_diagnostics_handle);
        _iotdata_node_diagnostics_ready = false;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
static inline void iotdata_node_diagnostics_event(const iotdata_node_bb_lc_event_t ev, const uint8_t reason) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_lifecycle(&_iotdata_node_diagnostics_handle, ev, reason);
}

static inline void iotdata_node_diagnostics_power(const iotdata_node_bb_power_event_t *const r) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power(&_iotdata_node_diagnostics_handle, r);
}

static inline void iotdata_node_diagnostics_power_source(const iotdata_node_bb_power_source_t *const r) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power_source(&_iotdata_node_diagnostics_handle, r);
}

static inline void iotdata_node_diagnostics_power_detail(const uint8_t rail, const char *const key, const char *const value) {
    if (_iotdata_node_diagnostics_ready)
        (void)iotdata_node_blackbox_power_detail(&_iotdata_node_diagnostics_handle, rail, key, value);
}

static inline void iotdata_node_diagnostics_flush(void) {
    if (_iotdata_node_diagnostics_ready)
        (void)blackbox_flush(&_iotdata_node_diagnostics_handle);
}

static inline void iotdata_node_diagnostics_enable(const bool on) {
    if (_iotdata_node_diagnostics_ready)
        blackbox_enable(&_iotdata_node_diagnostics_handle, on);
    else if (on) { /* off from boot and so never started: start it now, for the rest of this boot */
        _iotdata_node_diagnostics_config.enabled = true;
        (void)_iotdata_node_diagnostics_start(IOTDATA_NODE_BB_LC_START, 0);
    }
}

static inline void iotdata_node_diagnostics_clear(void) {
    if (_iotdata_node_diagnostics_ready) {
        _iotdata_node_diagnostics_dump.active = false; /* a dump in flight has a cursor about to go stale */
        blackbox_clear(&_iotdata_node_diagnostics_handle);
    }
}

static inline size_t iotdata_node_diagnostics_pull(size_t *const cursor, char *const out, const size_t outsize) {
    if (_iotdata_node_diagnostics_ready)
        if (blackbox_pull(&_iotdata_node_diagnostics_handle, cursor, out, outsize) > 0)
            return strlen(out);
    return 0;
}

static inline void _iotdata_node_diagnostics_tick(const uint32_t now_ms) {
    if (_iotdata_node_diagnostics_ready) {
        if (_iotdata_node_diagnostics_ticked_ms == 0)
            _iotdata_node_diagnostics_ticked_ms = now_ms;
        blackbox_tick(&_iotdata_node_diagnostics_handle, now_ms - _iotdata_node_diagnostics_ticked_ms);
        _iotdata_node_diagnostics_ticked_ms = now_ms;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING IT BACK
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_diagnostics_stat(void) {
    if (_iotdata_node_diagnostics_ready) {
        blackbox_status_t st;
        if (blackbox_status(&_iotdata_node_diagnostics_handle, &st)) {
            (void)blackbox_status_str(&st, BLACKBOX_STATUS_ALL, _iotdata_node_diagnostics_line, sizeof(_iotdata_node_diagnostics_line));
            _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_line);
        }
    } else
        _iotdata_node_diagnostics_say("diag: unavailable (recorder did not start)");
}

static inline void iotdata_node_diagnostics_dump_start(void) {
    if (_iotdata_node_diagnostics_ready) {
        iotdata_node_diagnostics_flush();
        _iotdata_node_diagnostics_dump.cursor = 0;
        _iotdata_node_diagnostics_dump.count = 0;
        _iotdata_node_diagnostics_dump.active = true;
        _iotdata_node_diagnostics_say("diag: dump begin");
    } else
        _iotdata_node_diagnostics_say("diag: unavailable (recorder did not start)");
}

static inline bool iotdata_node_diagnostics_pump(void) {
    if (_iotdata_node_diagnostics_ready && _iotdata_node_diagnostics_dump.active) {
        for (int i = 0; i < IOTDATA_NODE_DIAGNOSTICS_DUMP_CHUNK; i++) {
            if (blackbox_pull(&_iotdata_node_diagnostics_handle, &_iotdata_node_diagnostics_dump.cursor, _iotdata_node_diagnostics_rec, sizeof(_iotdata_node_diagnostics_rec)) <= 0) {
                (void)snprintf(_iotdata_node_diagnostics_line, sizeof(_iotdata_node_diagnostics_line), "diag: dump end (%d record(s))", _iotdata_node_diagnostics_dump.count);
                _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_line);
                _iotdata_node_diagnostics_dump.active = false;
                return false;
            }
            _iotdata_node_diagnostics_say(_iotdata_node_diagnostics_rec);
            _iotdata_node_diagnostics_dump.count++;
        }
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ONE CALL AN APP MAKES: ONCE A PASS, AND FIRST
//
// Everything above is reachable on its own, and an app should reach for none of it. This is the
// whole obligation: call it once per pass of the main loop, as the first thing in the pass, with
// the now_ms the rest of the pass is working to.
//
// IT IS ONE FUNCTION BECAUSE IT WAS TWO AND THAT WAS A TRAP. The clock-and-canary half and the
// dump-draining half have nothing to do with each other, so an app would wire up whichever one it
// happened to think of -- and two of them wired up only the drain, which meant the guard bands
// were never checked and `diag stat` reported uptime=0 for the life of the node. Neither one
// failed loudly, because neither one is load-bearing until the day it is. There is now nothing to
// half-remember.
//
// FIRST IN THE PASS, because the canary check is worth most before this pass's records are
// written rather than after, and because the clock a record is accounted against should be this
// pass's and not the last one's. The cost is that a `diag dump` typed at the console -- which is
// read later in the pass -- starts draining on the next one, which is milliseconds away.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_diagnostics_tick(const uint32_t now_ms) {
    _iotdata_node_diagnostics_tick(now_ms); /* the clock, and the pool's guard bands */
    (void)iotdata_node_diagnostics_pump();  /* and one chunk of a dump, if one is running */
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE NODE'S CONTROL KEYS
//
// What a device with a recorder accepts over the air, and the one handler for it. ENABLE and CLEAR
// are always here; DUMP is too unless IOTDATA_NODE_DIAGNOSTICS_DUMP is off, in which case it is absent
// from BOTH the advertised list and the handler -- a node that advertises a key it will not service
// sends whoever asked on a wild goose chase.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* (subject, action) PAIRS, which is the shape the inventory emits and the shape a command takes on
   the wire -- so advertising one and sending one cannot drift apart. */
/* THE PAIRS AS A MACRO, so an app can SPLICE them into an array of its own rather than copy them
   into one at run time. A node with two modules that both answer control keys has to advertise the
   union of them, and there is no way to concatenate two arrays at compile time in C -- so every
   such app had a mutable array, two copy loops and a count assigned at startup. Five of them did,
   identically, and the failure mode was silent: forget the init and the node advertises pairs of
   zeroes, which reads as a node that answers subject 0 action 0 and nothing else.
 *
 * Spliced instead, the array is `static const`, the count is a sizeof, and there is nothing left to
 * forget. The trailing comma is deliberate and is what makes two of these sit next to each other. */
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_ENABLE, IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_CLEAR, _IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS_DUMP

#if IOTDATA_NODE_DIAGNOSTICS_DUMP
#define _IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS_DUMP IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_DUMP,
#else
#define _IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS_DUMP /* absent from the list as well as the handler */
#endif

/* Kept for an app that advertises nothing else and wants the module's list as it stands. */
static const uint8_t iotdata_node_diagnostics_control_actions[] = { IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS };
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT ((uint8_t)(sizeof(iotdata_node_diagnostics_control_actions) / (2 * sizeof(iotdata_node_diagnostics_control_actions[0]))))

/*
 * What to put in a node's config. These are the FUNCTIONS when the recorder is compiled in and
 * NULL when it is not, which is the distinction the node layer reads: a NULL diag hook means this
 * device keeps no diagnostics at all and its report is empty, while a present one that returns
 * nothing means it keeps a blackbox that happens to be empty. Passing the function unconditionally
 * would make every node claim a recorder. Both are usable with no #if at the call site.
 */
#define IOTDATA_NODE_DIAGNOSTICS_PULL                  iotdata_node_diagnostics_pull
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL               iotdata_node_diagnostics_node_control

static inline bool iotdata_node_diagnostics_control(const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen) {
    /* ENABLE ahead of the ready test: a recorder that is off from boot was never started, and enabling
       it is what starts it */
    if (subject == IOTDATA_NODE_TLV_DIAGNOSTICS && action == IOTDATA_NODE_ACTION_DIAG_BBOX_ENABLE) {
        const bool on = (arglen >= 1) ? (args[0] != 0u) : true;
        iotdata_node_diagnostics_enable(on);
        _iotdata_node_diagnostics_say(!on ? "diag: disabled" : _iotdata_node_diagnostics_ready ? "diag: enabled" : "diag: enable failed -- the store would not open");
        return true;
    }
    if (_iotdata_node_diagnostics_ready)
        if (subject == IOTDATA_NODE_TLV_DIAGNOSTICS)
            switch (action) {
            case IOTDATA_NODE_ACTION_DIAG_BBOX_CLEAR:
                iotdata_node_diagnostics_clear();
                _iotdata_node_diagnostics_say("diag: cleared");
                return true;
#if IOTDATA_NODE_DIAGNOSTICS_DUMP
            case IOTDATA_NODE_ACTION_DIAG_BBOX_DUMP:
                iotdata_node_diagnostics_dump_start();
                return true;
#endif
            default:
                break;
            }
    return false;
}

/* THE RECORDER HAS NO REFUSALS, so this adapter only widens a bool. A build with the recorder
   compiled out registers no handler at all and advertises no keys, which makes "unknown" the
   truthful answer there rather than a shrug -- see WHAT HAPPENED TO IT in iotdata_node_control.h. */
static inline iotdata_node_control_result_t iotdata_node_diagnostics_node_control(__attribute__((unused)) const uint16_t station, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen,
                                                                                  __attribute__((unused)) const char **const reason) {
    return iotdata_node_control_result_of(iotdata_node_diagnostics_control(subject, action, args, arglen));
}

#else /* !IOTDATA_NODE_DIAGNOSTICS -- compiled out, but every call site still type-checks */

static const uint8_t *const iotdata_node_diagnostics_control_actions = NULL;
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTIONS_COUNT ((uint8_t)0)
/* Expands to NOTHING, so an app that splices this and the content module's pairs into one array
   still compiles with the recorder off -- and advertises only the other module's keys, which is
   exactly what a node with no recorder should say. */
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS
/* NULL, so a node with no recorder reports no diagnostics rather than an empty blackbox */
#define IOTDATA_NODE_DIAGNOSTICS_PULL    NULL
#define IOTDATA_NODE_DIAGNOSTICS_CONTROL NULL

static inline iotdata_node_diagnostics_emit_fn iotdata_node_diagnostics_emit_set(__attribute__((unused)) const iotdata_node_diagnostics_emit_fn fn) {
    return NULL;
}
static inline bool iotdata_node_diagnostics_ready(void) {
    return false;
}
static inline blackbox_handle_t *iotdata_node_diagnostics_handle(void) {
    return NULL;
}
static inline bool iotdata_node_diagnostics_begin(__attribute__((unused)) const uint8_t reason, __attribute__((unused)) const bool cold) {
    return false;
}
typedef struct {
    bool enabled;
    const char *persist_arg;
    blackbox_flush_t flush;
    uint32_t flush_ms;
    uint32_t max_records, max_bytes;
    uint8_t generations;
} iotdata_node_diagnostics_conf_t;
static inline bool iotdata_node_diagnostics_begin_conf(__attribute__((unused)) const iotdata_node_diagnostics_conf_t *const conf, __attribute__((unused)) const iotdata_node_bb_lc_event_t ev, __attribute__((unused)) const uint8_t reason) {
    return false;
}
static inline const char *iotdata_node_diagnostics_persist_arg(void) {
    return NULL;
}
static inline void iotdata_node_diagnostics_end(__attribute__((unused)) const iotdata_node_bb_lc_event_t ev, __attribute__((unused)) const uint8_t reason) {
}
static inline void iotdata_node_diagnostics_event(__attribute__((unused)) const iotdata_node_bb_lc_event_t ev, __attribute__((unused)) const uint8_t reason) {
}
static inline void iotdata_node_diagnostics_power(__attribute__((unused)) const iotdata_node_bb_power_event_t *const r) {
}
static inline void iotdata_node_diagnostics_power_source(__attribute__((unused)) const iotdata_node_bb_power_source_t *const r) {
}
static inline void iotdata_node_diagnostics_power_detail(__attribute__((unused)) const uint8_t rail, __attribute__((unused)) const char *const key, __attribute__((unused)) const char *const value) {
}
static inline void iotdata_node_diagnostics_flush(void) {
}
static inline void iotdata_node_diagnostics_enable(__attribute__((unused)) const bool on) {
}
static inline void iotdata_node_diagnostics_clear(void) {
}
static inline size_t iotdata_node_diagnostics_pull(__attribute__((unused)) size_t *const cursor, __attribute__((unused)) char *const out, __attribute__((unused)) const size_t outsize) {
    return 0;
}
static inline void _iotdata_node_diagnostics_tick(__attribute__((unused)) const uint32_t now_ms) {
}
static inline void iotdata_node_diagnostics_stat(void) {
}
static inline void iotdata_node_diagnostics_dump_start(void) {
}
static inline bool iotdata_node_diagnostics_pump(void) {
    return false;
}
static inline void iotdata_node_diagnostics_tick(__attribute__((unused)) const uint32_t now_ms) {
}
static inline bool iotdata_node_diagnostics_control(__attribute__((unused)) const uint8_t subject, __attribute__((unused)) const uint8_t action, __attribute__((unused)) const uint8_t *const args,
                                                    __attribute__((unused)) const uint8_t arglen) {
    return false;
}
static inline iotdata_node_control_result_t iotdata_node_diagnostics_node_control(__attribute__((unused)) const uint16_t station, __attribute__((unused)) const uint8_t subject, __attribute__((unused)) const uint8_t action,
                                                                                  __attribute__((unused)) const uint8_t *const args, __attribute__((unused)) const uint8_t arglen, __attribute__((unused)) const char **const reason) {
    return IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
}

#endif /* IOTDATA_NODE_DIAGNOSTICS */

// -----------------------------------------------------------------------------------------------------------------------------------------
// ADAPTER IMPLEMENTATION
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_NODE_BLACKBOX_IMPLEMENTATION

static int iotdata_node_bb__enc_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_lifecycle_t *r = (const iotdata_node_bb_lifecycle_t *)data;
    return snprintf(out, n, "%u,%u", (unsigned)r->event, (unsigned)r->reason);
}
static int iotdata_node_bb__dec_lifecycle(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_lifecycle_t *r = (iotdata_node_bb_lifecycle_t *)data;
    unsigned e = 0, rs = 0;
    if (sscanf(in, "%u,%u", &e, &rs) != 2)
        return -1;
    r->event = (uint8_t)e;
    r->reason = (uint8_t)rs;
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_lifecycle = { "LC", iotdata_node_bb__enc_lifecycle, iotdata_node_bb__dec_lifecycle };

/* Power. Fields are positional and MAY BE EMPTY, which sscanf cannot express, so the decoders walk
   to a field and parse from there -- strtol stops at the comma of its own accord, and an empty
   field yields the caller's "unknown". The value of a PWD is last so it may contain commas. */

static const char *iotdata_node_bb__field(const char *in, const int idx) {
    for (int i = 0; i < idx && in != NULL; i++)
        in = (in = strchr(in, ',')) != NULL ? in + 1 : NULL;
    return in;
}
static bool iotdata_node_bb__field_absent(const char *const f) {
    return f == NULL || *f == '\0' || *f == ',' || *f == '\n';
}
static long iotdata_node_bb__field_num(const char *const in, const int idx, const long absent) {
    const char *const f = iotdata_node_bb__field(in, idx);
    return iotdata_node_bb__field_absent(f) ? absent : strtol(f, NULL, 10);
}
static void iotdata_node_bb__field_str(const char *const in, const int idx, char *const out, const size_t outlen, const bool to_end) {
    const char *const f = iotdata_node_bb__field(in, idx);
    out[0] = '\0';
    if (!iotdata_node_bb__field_absent(f)) {
        const char *end = to_end ? strchr(f, '\n') : strchr(f, ',');
        const size_t len = (end != NULL) ? (size_t)(end - f) : strlen(f);
        (void)snprintf(out, outlen, "%.*s", (int)(len < outlen - 1 ? len : outlen - 1), f);
    }
}

/* An absent numeric field is printed as nothing at all: ",,". */
#define _IOTDATA_NODE_BB_PW_NUM(buf, val, absent) \
    do { \
        (buf)[0] = '\0'; \
        if ((val) != (absent)) \
            (void)snprintf((buf), sizeof(buf), "%ld", (long)(val)); \
    } while (0)

static int iotdata_node_bb__enc_power(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_event_t *r = (const iotdata_node_bb_power_event_t *)data;
    char mv[12], ua[12], mw[12], pct[5], degc[6];
    _IOTDATA_NODE_BB_PW_NUM(mv, r->mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(ua, r->ua, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(mw, r->mw, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(pct, r->pct, IOTDATA_NODE_BB_PW_NO_PCT);
    _IOTDATA_NODE_BB_PW_NUM(degc, r->degc, IOTDATA_NODE_BB_PW_NO_DEGC);
    return snprintf(out, n, "%u,%u,%s,%s,%s,%s,%s,%u", (unsigned)r->event, (unsigned)r->rail, mv, ua, mw, pct, degc, (unsigned)r->flags);
}
static int iotdata_node_bb__dec_power(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_event_t *r = (iotdata_node_bb_power_event_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 1)))
        return -1; /* event and rail are the only two that are never absent */
    r->event = (uint8_t)iotdata_node_bb__field_num(in, 0, 0);
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 1, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->mv = (int32_t)iotdata_node_bb__field_num(in, 2, IOTDATA_NODE_BB_PW_NO_I32);
    r->ua = (int32_t)iotdata_node_bb__field_num(in, 3, IOTDATA_NODE_BB_PW_NO_I32);
    r->mw = (int32_t)iotdata_node_bb__field_num(in, 4, IOTDATA_NODE_BB_PW_NO_I32);
    r->pct = (uint8_t)iotdata_node_bb__field_num(in, 5, IOTDATA_NODE_BB_PW_NO_PCT);
    r->degc = (int8_t)iotdata_node_bb__field_num(in, 6, IOTDATA_NODE_BB_PW_NO_DEGC);
    r->flags = (uint8_t)iotdata_node_bb__field_num(in, 7, 0);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power = { "PWE", iotdata_node_bb__enc_power, iotdata_node_bb__dec_power };

static int iotdata_node_bb__enc_power_source(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_source_t *r = (const iotdata_node_bb_power_source_t *)data;
    char nom[12], lo[12], hi[12], cap[12], lim[12], ratio[8], off[8];
    _IOTDATA_NODE_BB_PW_NUM(nom, r->nominal_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(lo, r->min_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(hi, r->max_mv, IOTDATA_NODE_BB_PW_NO_I32);
    _IOTDATA_NODE_BB_PW_NUM(cap, r->capacity_mah, IOTDATA_NODE_BB_PW_NO_U32);
    _IOTDATA_NODE_BB_PW_NUM(lim, r->limit_mw, IOTDATA_NODE_BB_PW_NO_U32);
    _IOTDATA_NODE_BB_PW_NUM(ratio, r->ratio_x100, 0);
    _IOTDATA_NODE_BB_PW_NUM(off, r->offset_mv, 0);
    return snprintf(out, n, "%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s", (unsigned)r->rail, (unsigned)r->from_rail, (unsigned)r->type, (unsigned)r->form, (unsigned)r->hz, (unsigned)r->chem, (unsigned)r->cells, nom, lo, hi, cap, lim, ratio,
                    off);
}
static int iotdata_node_bb__dec_power_source(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_source_t *r = (iotdata_node_bb_power_source_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 2)))
        return -1;
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 0, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->from_rail = (uint8_t)iotdata_node_bb__field_num(in, 1, IOTDATA_NODE_BB_PW_NO_RAIL);
    r->type = (uint8_t)iotdata_node_bb__field_num(in, 2, IOTDATA_NODE_BB_PW_TYPE_UNKNOWN);
    r->form = (uint8_t)iotdata_node_bb__field_num(in, 3, IOTDATA_NODE_BB_PW_FORM_DC);
    r->hz = (uint8_t)iotdata_node_bb__field_num(in, 4, 0);
    r->chem = (uint8_t)iotdata_node_bb__field_num(in, 5, IOTDATA_NODE_BB_PW_CHEM_NONE);
    r->cells = (uint8_t)iotdata_node_bb__field_num(in, 6, 0);
    r->nominal_mv = (int32_t)iotdata_node_bb__field_num(in, 7, IOTDATA_NODE_BB_PW_NO_I32);
    r->min_mv = (int32_t)iotdata_node_bb__field_num(in, 8, IOTDATA_NODE_BB_PW_NO_I32);
    r->max_mv = (int32_t)iotdata_node_bb__field_num(in, 9, IOTDATA_NODE_BB_PW_NO_I32);
    r->capacity_mah = (uint32_t)iotdata_node_bb__field_num(in, 10, IOTDATA_NODE_BB_PW_NO_U32);
    r->limit_mw = (uint32_t)iotdata_node_bb__field_num(in, 11, IOTDATA_NODE_BB_PW_NO_U32);
    r->ratio_x100 = (uint16_t)iotdata_node_bb__field_num(in, 12, 0);
    r->offset_mv = (int16_t)iotdata_node_bb__field_num(in, 13, 0);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power_source = { "PWS", iotdata_node_bb__enc_power_source, iotdata_node_bb__dec_power_source };

static int iotdata_node_bb__enc_power_detail(__attribute__((unused)) const blackbox_struct_config_t *sc, const void *data, char *out, size_t n) {
    const iotdata_node_bb_power_detail_t *r = (const iotdata_node_bb_power_detail_t *)data;
    return snprintf(out, n, "%u,%s,%s", (unsigned)r->rail, r->key, r->value);
}
static int iotdata_node_bb__dec_power_detail(__attribute__((unused)) const blackbox_struct_config_t *sc, const char *in, __attribute__((unused)) size_t n, void *data) {
    iotdata_node_bb_power_detail_t *r = (iotdata_node_bb_power_detail_t *)data;
    if (iotdata_node_bb__field_absent(iotdata_node_bb__field(in, 1)))
        return -1;
    r->rail = (uint8_t)iotdata_node_bb__field_num(in, 0, IOTDATA_NODE_BB_PW_NO_RAIL);
    iotdata_node_bb__field_str(in, 1, r->key, sizeof(r->key), false);
    iotdata_node_bb__field_str(in, 2, r->value, sizeof(r->value), true);
    return 0;
}
const blackbox_struct_config_t iotdata_node_blackbox_config_power_detail = { "PWD", iotdata_node_bb__enc_power_detail, iotdata_node_bb__dec_power_detail };

/*
 * THE RECORD CLOCK: "seq:up_ms", and ONE clock on every platform.
 *
 * `seq` is a counter in the reset-surviving domain, so records keep their order across a watchdog,
 * a panic and a deep sleep -- which also means it holds GARBAGE on a true cold start. A MAGIC WORD
 * is how that is detected, and NOT a reset-reason test: a USB-powered ESP32-C3 reports ESP_RST_USB
 * on a genuine power-up rather than ESP_RST_POWERON (the ROM logs it as rst:0x15
 * USB_UART_CHIP_RESET), so `if (reason == POWERON) seq = 0;` silently never fires and the garbage
 * survives every later reset. The magic word asks the only question that matters -- is this memory
 * ours -- and asks it the same way everywhere.
 *
 * RTC_NOINIT_ATTR IS THE PLATFORM'S: the real attribute on a device, nothing at all on a host. So
 * on a host these are ordinary statics -- zero at start, seeded by the same magic word, and the
 * counter restarts with the process, which is the truth, because a host restart IS a cold start.
 * A run is a run whether it began with a reset or with execve. `up_ms` is hw_time_ms() on both.
 *
 * A HOST THAT WANTS WALL CLOCK TIME IN ITS RECORDS says so for itself, with
 * BLACKBOX_CLOCK iotdata_node_blackbox_clock_wall below -- the application knows whether its
 * records will be read beside a syslog; this header does not.
 *
 * Both variables are owned HERE rather than by each app, so this cannot be got wrong per project.
 * Call iotdata_node_blackbox_begin() once before blackbox_init().
 */
#define IOTDATA_NODE_BLACKBOX_SEQ_MAGIC 0x1D5EC10Cu /* 'IDSEC-CLOC(k)' */

RTC_NOINIT_ATTR uint32_t iotdata_node_blackbox_seq;
RTC_NOINIT_ATTR static uint32_t iotdata_node_blackbox_seq_magic;

void iotdata_node_blackbox_begin(void) {
    if (iotdata_node_blackbox_seq_magic != IOTDATA_NODE_BLACKBOX_SEQ_MAGIC) { /* garbage: a cold start */
        iotdata_node_blackbox_seq = 0;
        iotdata_node_blackbox_seq_magic = IOTDATA_NODE_BLACKBOX_SEQ_MAGIC;
    }
}

int iotdata_node_blackbox_clock(char *out, size_t n) {
    return snprintf(out, n, "%u:%u", (unsigned)(++iotdata_node_blackbox_seq), (unsigned)hw_time_ms());
}

/* THE OTHER ONE, for a reader who needs to line records up against something outside this node.
   hw_clock() is the platform's wall clock and may be nothing but seconds since boot, so it is
   asked whether it means anything first -- a stamp that lies about a date is worse than a tick. */
int iotdata_node_blackbox_clock_wall(char *out, size_t n) {
    if (!hw_clock_is_set())
        return iotdata_node_blackbox_clock(out, n);
    return snprintf(out, n, "%lld", (long long)hw_clock() * 1000);
}

#endif /* IOTDATA_NODE_BLACKBOX_IMPLEMENTATION */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_DIAGNOSTICS_H */
