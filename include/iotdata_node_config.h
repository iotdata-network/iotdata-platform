#ifndef IOTDATA_NODE_CONFIG_TYPES_H
#define IOTDATA_NODE_CONFIG_TYPES_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// iotdata_node_config.h - what the DEVICE is set to: the application's own settable parameters.
//
// NOT SETTINGS. SETTINGS is what the PROTOCOL is set to -- station id, reporting schedule, receive
// schedule -- it is the same on every node, and the library owns it. This is the device's own, it
// differs on every device, and the library knows nothing about what any entry MEANS.
//
// THE TABLE IS COMPILED IN, and the header is included TWICE:
//
//     #include "iotdata_node_config.h"     // the types, so a handler can be written
//
//     static bool on_channel(const iotdata_node_config_row_t *r, const iotdata_node_config_value_t *was,
//                            const iotdata_node_config_info_t *i) { ... }
//
//     #define IOTDATA_NODE_CONFIG_ENTRIES(X)
//         X(SENSOR_TX_PERIOD_S, 0x001, U16, 10, 3600, 60, 0,                         NULL, NULL, "how often a reading goes out")
//         X(LORA_CHANNEL,       0x002, U8,  0,  83,   23, IOTDATA_NODE_CONFIG_FLAG_REBOOT, NULL, on_channel, "the radio channel")
//     #include "iotdata_node_config.h"     // now the table, the ids and the accessors
//
// (each X line ends with a backslash in real code; they are omitted here so this stays a comment)
//
// ONE TRANSLATION UNIT. The expansion defines a STATIC table and a STATIC value array, so a second
// .c file including it gets a second, independent copy -- two halves of one device disagreeing
// about its own settings, silently. These apps are single-TU by construction (app.c #includes what
// it needs), which is what makes the compiled-in table affordable; an app that is not must put the
// table in one file and reach it through accessors from the others.
//
// THE GENERATED NAMES ARE IOTDATA_NODE_CFG_*, not IOTDATA_NODE_CONFIG_*. The latter is already the
// compile-time knob namespace (iotdata_config.h), and an entry named after one of those knobs --
// LORA_CHANNEL, say -- would otherwise expand an existing #define into the middle of an enum.
//
// Compiled in rather than registered, because that is what lets an accessor be TYPE-CHECKED: the id
// carries its type as a sibling macro, so config_get_u16(LORA_CHANNEL) fails to BUILD rather than
// returning a wrong answer at three in the morning. For the same reason the accessors assert rather
// than returning bool+out-param -- with a compiled-in table a missing id is a build error, never a
// runtime condition, and every call site would otherwise carry a branch that can never be taken.
//
// TWO WAYS TO CONSUME A VALUE, and a device wants both:
//
//   READ IT WHEN YOU NEED IT   config_get_u16(SENSOR_TX_PERIOD_S) in the loop that uses it. No
//                              cached global, so a change is picked up by construction and there is
//                              no third place for the value to go stale.
//
//   BE TOLD IT CHANGED         a notify handler, for anything that cannot simply be re-read: a
//                              radio that must be reconfigured, a buffer that must be re-sized.
//                              Reading it every time would not help -- the ACT is what matters.
// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------
// RECORD -- id(12) | type(4), one 16-bit word, and it is the SAME shape in the datastore and on the wire.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONFIG_TYPE_BOOL      0x0
#define IOTDATA_NODE_CONFIG_TYPE_U8        0x1
#define IOTDATA_NODE_CONFIG_TYPE_I8        0x2
#define IOTDATA_NODE_CONFIG_TYPE_U16       0x3
#define IOTDATA_NODE_CONFIG_TYPE_I16       0x4
#define IOTDATA_NODE_CONFIG_TYPE_U32       0x5
#define IOTDATA_NODE_CONFIG_TYPE_I32       0x6
#define IOTDATA_NODE_CONFIG_TYPE_U64       0x7
#define IOTDATA_NODE_CONFIG_TYPE_I64       0x8
#define IOTDATA_NODE_CONFIG_TYPE_FLOAT     0x9
#define IOTDATA_NODE_CONFIG_TYPE_STRING    0xA
#define IOTDATA_NODE_CONFIG_TYPE_BLOB      0xB
/* 0xC..0xF spare */

#define IOTDATA_NODE_CONFIG_ID_MAX         0x0FFFu
#define IOTDATA_NODE_CONFIG_ID_PROPRIETARY 0x0800u /* the upper half of the id space is the vendor's */

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint16_t iotdata_node_config_rec(const uint16_t id, const uint8_t type) {
    return (uint16_t)(((id & IOTDATA_NODE_CONFIG_ID_MAX) << 4) | (type & 0x0Fu));
}
static inline uint16_t iotdata_node_config_rec_id(const uint16_t rec) {
    return (uint16_t)(rec >> 4);
}
static inline uint8_t iotdata_node_config_rec_type(const uint16_t rec) {
    return (uint8_t)(rec & 0x0Fu);
}

static inline uint8_t iotdata_node_config_type_size(const uint8_t type) {
    switch (type) {
    case IOTDATA_NODE_CONFIG_TYPE_BOOL:
    case IOTDATA_NODE_CONFIG_TYPE_U8:
    case IOTDATA_NODE_CONFIG_TYPE_I8:
        return 1;
    case IOTDATA_NODE_CONFIG_TYPE_U16:
    case IOTDATA_NODE_CONFIG_TYPE_I16:
        return 2;
    case IOTDATA_NODE_CONFIG_TYPE_U32:
    case IOTDATA_NODE_CONFIG_TYPE_I32:
    case IOTDATA_NODE_CONFIG_TYPE_FLOAT:
        return 4;
    case IOTDATA_NODE_CONFIG_TYPE_U64:
    case IOTDATA_NODE_CONFIG_TYPE_I64:
        return 8;
    default:
        return 0; /* variable: string and blob */
    }
}

static inline bool iotdata_node_config_type_is_signed(const uint8_t type) {
    return type == IOTDATA_NODE_CONFIG_TYPE_I8 || type == IOTDATA_NODE_CONFIG_TYPE_I16 || type == IOTDATA_NODE_CONFIG_TYPE_I32 || type == IOTDATA_NODE_CONFIG_TYPE_I64;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// TABLE
// -----------------------------------------------------------------------------------------------------------------------------------------

#define IOTDATA_NODE_CONFIG_FLAG_NONE     0x00u
#define IOTDATA_NODE_CONFIG_FLAG_REBOOT   0x01u /* takes effect only after a restart                       */
#define IOTDATA_NODE_CONFIG_FLAG_READONLY 0x02u /* reportable, not writable: a fact rather than a setting  */
#define IOTDATA_NODE_CONFIG_FLAG_LOCAL    0x04u /* settable, but only from a local console/command line */
#define IOTDATA_NODE_CONFIG_FLAG_STARTUP  0x08u /* read before the configuration, e.g. config file itself */

typedef union {
    bool b;
    int64_t i;
    uint64_t u;
    float f;
    struct {
        const char *p;
        uint16_t len;
    } s;
} iotdata_node_config_value_t;

struct iotdata_node_config_row;
struct iotdata_node_config_update;

typedef struct {
    bool validation_only; /* a dry run: check, do not act                                   */
    bool version_changed; /* the persisted image was written by a different build           */
} iotdata_node_config_info_t;

/* Check one proposed value. NULL means "the standard check", which is the bounds. Runs on BOTH
   paths -- a value arriving typed over the air never parses, and must still be validated.
   It is handed the whole UPDATE, not just its own row, and that is what staging buys: "the minimum
   must not exceed the maximum" can only be checked once both proposed values are in hand. Use
   iotdata_node_config_update_peek() to read another entry as it WILL BE, which is its staged value if
   the same update touched it and its current one otherwise. `u` is NULL for a standalone check. */
typedef bool (*iotdata_node_config_validate_fn)(const struct iotdata_node_config_row *row, const iotdata_node_config_value_t *proposed, const struct iotdata_node_config_update *u);

/* Told AFTER the whole set is committed and saved. Cannot veto -- that is what validate is for --
   and returns whether a reboot is needed to make it real. This is where a radio gets reconfigured.
   Separate from validate because they run on different paths and at different times: one may reject
   and must not act, the other may act and cannot reject. */
typedef bool (*iotdata_node_config_notify_fn)(const struct iotdata_node_config_row *row, const iotdata_node_config_value_t *old, const iotdata_node_config_info_t *info);

typedef struct iotdata_node_config_row {
    uint16_t id;
    uint8_t type;
    uint8_t flags;
    const char *name;
    iotdata_node_config_value_t min, max, dflt;
    iotdata_node_config_validate_fn validate;
    iotdata_node_config_notify_fn notify;
    const char *help;
    uint16_t sidx; /* slot for STRING row */
} iotdata_node_config_row_t;

#endif /* IOTDATA_NODE_CONFIG_TYPES_H */

// -----------------------------------------------------------------------------------------------------------------------------------------
// EXPANSION
//
// One X-macro list becomes four things: the ids, a TYPE sibling per id, an index per id, and the
// table itself. The TYPE sibling is the whole point -- it is what a typed accessor can assert on.
//
// A SCALAR LIVES IN THE UNION; A STRING GETS A SLOT SIZED BY ITS OWN max.
//
// The union is a value, not a buffer, so a mutable string needs somewhere to live. The expansion
// declares one struct member per STRING row, `char NAME[max + 1]` -- so a row that says it holds a
// 31-character client id costs 32 bytes and a row that says it holds a 255-character URL costs 256.
// All members are char arrays, so the struct has alignment 1 and its sizeof is the exact sum.
//
// THE SAME NUMBER BOUNDS IT AND SIZES IT. `max` is the length a write is validated against and the
// storage that write lands in -- one declaration, so they cannot disagree. A flat per-row maximum
// would be the obvious alternative and is worse both ways: too small and it truncates a URL or a
// path, which is a config that looks like it worked; too large and every short row pays for it.
//
// A BUMP POOL IS THE OTHER SHAPE, and is what the gateway's own file loader does. That one parses
// once at startup and never writes again, so orphaning the old string on an overwrite is bounded.
// This table is written over the air for as long as the node runs, so a pool that never reclaims
// is a slow leak with a reboot at the end of it, and one that does reclaim has to repack and loses
// the per-row bound. Fixed slots cannot fragment, cannot run out, and cost a compile-time number.
//
// BLOB is still refused: the same storage question with no caller to answer it.
// -----------------------------------------------------------------------------------------------------------------------------------------

#if defined(IOTDATA_NODE_CONFIG_ENTRIES) && !defined(IOTDATA_NODE_CONFIG_EXPANDED)
#define IOTDATA_NODE_CONFIG_EXPANDED

/* Bounds are always numeric -- a string's are its LENGTH -- so they need only the three families. */
#define _IOTDATA_NODE_CFG_B_BOOL(x)                                  { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_U8(x)                                    { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_U16(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_U32(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_U64(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_I8(x)                                    { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_B_I16(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_B_I32(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_B_I64(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_B_FLOAT(x)                                 { .f = (float)(x) }
#define _IOTDATA_NODE_CFG_B_STRING(x)                                { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B_BLOB(x)                                  { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_B(t, x)                                    _IOTDATA_NODE_CFG_B_##t(x)

#define _IOTDATA_NODE_CFG_D_BOOL(x)                                  { .b = (bool)(x) }
#define _IOTDATA_NODE_CFG_D_U8(x)                                    { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_D_U16(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_D_U32(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_D_U64(x)                                   { .u = (uint64_t)(x) }
#define _IOTDATA_NODE_CFG_D_I8(x)                                    { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_D_I16(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_D_I32(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_D_I64(x)                                   { .i = (int64_t)(x) }
#define _IOTDATA_NODE_CFG_D_FLOAT(x)                                 { .f = (float)(x) }
#define _IOTDATA_NODE_CFG_D_STRING(x)                                { .s = { (x), 0 } }
#define _IOTDATA_NODE_CFG_D_BLOB(x)                                  { .s = { (x), 0 } }
#define _IOTDATA_NODE_CFG_D(t, x)                                    _IOTDATA_NODE_CFG_D_##t(x)

#define _IOTDATA_NODE_CFG_ID(n, id, t, mn, mx, df, fl, va, no, hp)   IOTDATA_NODE_CFG_##n = (id),
#define _IOTDATA_NODE_CFG_TYPE(n, id, t, mn, mx, df, fl, va, no, hp) IOTDATA_NODE_CFG_##n##__TYPE = IOTDATA_NODE_CONFIG_TYPE_##t,
#define _IOTDATA_NODE_CFG_S_BOOL(a, b)
#define _IOTDATA_NODE_CFG_S_U8(a, b)
#define _IOTDATA_NODE_CFG_S_U16(a, b)
#define _IOTDATA_NODE_CFG_S_U32(a, b)
#define _IOTDATA_NODE_CFG_S_U64(a, b)
#define _IOTDATA_NODE_CFG_S_I8(a, b)
#define _IOTDATA_NODE_CFG_S_I16(a, b)
#define _IOTDATA_NODE_CFG_S_I32(a, b)
#define _IOTDATA_NODE_CFG_S_I64(a, b)
#define _IOTDATA_NODE_CFG_S_FLOAT(a, b)
#define _IOTDATA_NODE_CFG_S_BLOB(a, b)

#define _IOTDATA_NODE_CFG_SIX_STRING(n, mx)                         IOTDATA_NODE_CFG_SIX_##n,
#define _IOTDATA_NODE_CFG_SIX_BOOL                                  _IOTDATA_NODE_CFG_S_BOOL
#define _IOTDATA_NODE_CFG_SIX_U8                                    _IOTDATA_NODE_CFG_S_U8
#define _IOTDATA_NODE_CFG_SIX_U16                                   _IOTDATA_NODE_CFG_S_U16
#define _IOTDATA_NODE_CFG_SIX_U32                                   _IOTDATA_NODE_CFG_S_U32
#define _IOTDATA_NODE_CFG_SIX_U64                                   _IOTDATA_NODE_CFG_S_U64
#define _IOTDATA_NODE_CFG_SIX_I8                                    _IOTDATA_NODE_CFG_S_I8
#define _IOTDATA_NODE_CFG_SIX_I16                                   _IOTDATA_NODE_CFG_S_I16
#define _IOTDATA_NODE_CFG_SIX_I32                                   _IOTDATA_NODE_CFG_S_I32
#define _IOTDATA_NODE_CFG_SIX_I64                                   _IOTDATA_NODE_CFG_S_I64
#define _IOTDATA_NODE_CFG_SIX_FLOAT                                 _IOTDATA_NODE_CFG_S_FLOAT
#define _IOTDATA_NODE_CFG_SIX_BLOB                                  _IOTDATA_NODE_CFG_S_BLOB
#define _IOTDATA_NODE_CFG_SIX(n, id, t, mn, mx, df, fl, va, no, hp) _IOTDATA_NODE_CFG_SIX_##t(n, mx)

#define _IOTDATA_NODE_CFG_SST_STRING(n, mx)                         char n[(mx) + 1];
#define _IOTDATA_NODE_CFG_SST_BOOL                                  _IOTDATA_NODE_CFG_S_BOOL
#define _IOTDATA_NODE_CFG_SST_U8                                    _IOTDATA_NODE_CFG_S_U8
#define _IOTDATA_NODE_CFG_SST_U16                                   _IOTDATA_NODE_CFG_S_U16
#define _IOTDATA_NODE_CFG_SST_U32                                   _IOTDATA_NODE_CFG_S_U32
#define _IOTDATA_NODE_CFG_SST_U64                                   _IOTDATA_NODE_CFG_S_U64
#define _IOTDATA_NODE_CFG_SST_I8                                    _IOTDATA_NODE_CFG_S_I8
#define _IOTDATA_NODE_CFG_SST_I16                                   _IOTDATA_NODE_CFG_S_I16
#define _IOTDATA_NODE_CFG_SST_I32                                   _IOTDATA_NODE_CFG_S_I32
#define _IOTDATA_NODE_CFG_SST_I64                                   _IOTDATA_NODE_CFG_S_I64
#define _IOTDATA_NODE_CFG_SST_FLOAT                                 _IOTDATA_NODE_CFG_S_FLOAT
#define _IOTDATA_NODE_CFG_SST_BLOB                                  _IOTDATA_NODE_CFG_S_BLOB
#define _IOTDATA_NODE_CFG_SST(n, id, t, mn, mx, df, fl, va, no, hp) _IOTDATA_NODE_CFG_SST_##t(n, mx)

#define _IOTDATA_NODE_CFG_SPT_STRING(n, mx)                         _iotdata_node_config_strings.n,
#define _IOTDATA_NODE_CFG_SPT_BOOL                                  _IOTDATA_NODE_CFG_S_BOOL
#define _IOTDATA_NODE_CFG_SPT_U8                                    _IOTDATA_NODE_CFG_S_U8
#define _IOTDATA_NODE_CFG_SPT_U16                                   _IOTDATA_NODE_CFG_S_U16
#define _IOTDATA_NODE_CFG_SPT_U32                                   _IOTDATA_NODE_CFG_S_U32
#define _IOTDATA_NODE_CFG_SPT_U64                                   _IOTDATA_NODE_CFG_S_U64
#define _IOTDATA_NODE_CFG_SPT_I8                                    _IOTDATA_NODE_CFG_S_I8
#define _IOTDATA_NODE_CFG_SPT_I16                                   _IOTDATA_NODE_CFG_S_I16
#define _IOTDATA_NODE_CFG_SPT_I32                                   _IOTDATA_NODE_CFG_S_I32
#define _IOTDATA_NODE_CFG_SPT_I64                                   _IOTDATA_NODE_CFG_S_I64
#define _IOTDATA_NODE_CFG_SPT_FLOAT                                 _IOTDATA_NODE_CFG_S_FLOAT
#define _IOTDATA_NODE_CFG_SPT_BLOB                                  _IOTDATA_NODE_CFG_S_BLOB
#define _IOTDATA_NODE_CFG_SPT(n, id, t, mn, mx, df, fl, va, no, hp) _IOTDATA_NODE_CFG_SPT_##t(n, mx)

#define _IOTDATA_NODE_CFG_SI_STRING(n)                              IOTDATA_NODE_CFG_SIX_##n
#define _IOTDATA_NODE_CFG_SI_BOOL(n)                                0
#define _IOTDATA_NODE_CFG_SI_U8(n)                                  0
#define _IOTDATA_NODE_CFG_SI_U16(n)                                 0
#define _IOTDATA_NODE_CFG_SI_U32(n)                                 0
#define _IOTDATA_NODE_CFG_SI_U64(n)                                 0
#define _IOTDATA_NODE_CFG_SI_I8(n)                                  0
#define _IOTDATA_NODE_CFG_SI_I16(n)                                 0
#define _IOTDATA_NODE_CFG_SI_I32(n)                                 0
#define _IOTDATA_NODE_CFG_SI_I64(n)                                 0
#define _IOTDATA_NODE_CFG_SI_FLOAT(n)                               0
#define _IOTDATA_NODE_CFG_SI_BLOB(n)                                0
#define _IOTDATA_NODE_CFG_SI(n, t)                                  _IOTDATA_NODE_CFG_SI_##t(n)

#define _IOTDATA_NODE_CFG_IX(n, id, t, mn, mx, df, fl, va, no, hp)  IOTDATA_NODE_CFG_IX_##n,
#define _IOTDATA_NODE_CFG_ROW(n, id, t, mn, mx, df, fl, va, no, hp) \
    { (id), IOTDATA_NODE_CONFIG_TYPE_##t, (fl), #n, _IOTDATA_NODE_CFG_B(t, mn), _IOTDATA_NODE_CFG_B(t, mx), _IOTDATA_NODE_CFG_D(t, df), va, no, (hp), _IOTDATA_NODE_CFG_SI(n, t) },

#define _IOTDATA_NODE_CFG_CASE(n, id, t, mn, mx, df, fl, va, no, hp) case (id):
__attribute__((unused)) static void _iotdata_node_config_ids_are_unique(const int x) {
    switch (x) {
        IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_CASE)
    default:
        break;
    }
}

enum {
    IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_ID) _IOTDATA_NODE_CFG_ID_END
};
enum {
    IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_TYPE) _IOTDATA_NODE_CFG_TYPE_END
};
enum {
    IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_IX) IOTDATA_NODE_CFG_COUNT
};
enum {
    IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_SIX) IOTDATA_NODE_CFG_STRING_COUNT
};

/* One member per string row, each the length that row declared. All char arrays, so alignment is 1
   and sizeof is the exact sum -- this struct IS the aggregate cost, and it is readable in a map. */
__attribute__((unused)) static struct {
    IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_SST)
    char _end[1]; /* a struct with no members is not C, and a table with no strings has none */
} _iotdata_node_config_strings;

/* One past the end, holding NULL. Sized that way so the initialiser is never empty either -- a
   table with no strings expands the entries to nothing and would otherwise leave `= { }`. */
static char *const _iotdata_node_config_string_slot[IOTDATA_NODE_CFG_STRING_COUNT + 1] = { IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_SPT) NULL };

static const iotdata_node_config_row_t iotdata_node_config_table[IOTDATA_NODE_CFG_COUNT] = { IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_ROW) };

/* A string row's storage, and how much of it there is. The capacity is the row's own `max`, which
   is also what a write is validated against -- read from the table rather than kept twice. */
static inline char *_iotdata_node_config_slot(const iotdata_node_config_row_t *const row) {
    return (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING) ? _iotdata_node_config_string_slot[row->sidx] : NULL;
}
static inline uint16_t _iotdata_node_config_slot_cap(const iotdata_node_config_row_t *const row) {
    return (uint16_t)row->max.u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// TEXT
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *_iotdata_node_config_type_name(const uint8_t t) {
    switch (t) {
    case IOTDATA_NODE_CONFIG_TYPE_BOOL:
        return "bool";
    case IOTDATA_NODE_CONFIG_TYPE_U8:
        return "u8";
    case IOTDATA_NODE_CONFIG_TYPE_I8:
        return "i8";
    case IOTDATA_NODE_CONFIG_TYPE_U16:
        return "u16";
    case IOTDATA_NODE_CONFIG_TYPE_I16:
        return "i16";
    case IOTDATA_NODE_CONFIG_TYPE_U32:
        return "u32";
    case IOTDATA_NODE_CONFIG_TYPE_I32:
        return "i32";
    case IOTDATA_NODE_CONFIG_TYPE_U64:
        return "u64";
    case IOTDATA_NODE_CONFIG_TYPE_I64:
        return "i64";
    case IOTDATA_NODE_CONFIG_TYPE_FLOAT:
        return "float";
    case IOTDATA_NODE_CONFIG_TYPE_STRING:
        return "string";
    case IOTDATA_NODE_CONFIG_TYPE_BLOB:
        return "blob";
    default:
        return "?";
    }
}

static inline const char *_iotdata_node_config_fmt(const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, char *const out, const size_t size) {
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING) /* quoted, so an empty one and a missing one differ on sight */
        return snprintf_inline(out, size, "\"%s\"", (v->s.p != NULL) ? v->s.p : "");
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_BOOL)
        return snprintf_inline(out, size, "%s", v->b ? "true" : "false");
    else if (row->type == IOTDATA_NODE_CONFIG_TYPE_FLOAT)
        return snprintf_inline(out, size, "%.3f", (double)v->f);
    else if (iotdata_node_config_type_is_signed(row->type))
        return snprintf_inline(out, size, "%lld", (long long)v->i);
    else
        return snprintf_inline(out, size, "%llu", (unsigned long long)v->u);
}

static inline bool _iotdata_node_config_parse(const iotdata_node_config_row_t *const row, const char *const text, iotdata_node_config_value_t *const out) {
    char *end = NULL;
    *out = (iotdata_node_config_value_t){ 0 };
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING) {
        /* BORROWED from the caller's argv, which is why the commit copies. The LENGTH is not
           checked here -- that is a bound, and staging is where a value meets its bounds. */
        out->s.p = text;
        out->s.len = (uint16_t)strlen(text);
        return true;
    }
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_BOOL) {
        if (strcmp(text, "true") == 0 || strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "1") == 0)
            out->b = true;
        else if (strcmp(text, "false") == 0 || strcmp(text, "off") == 0 || strcmp(text, "no") == 0 || strcmp(text, "0") == 0)
            out->b = false;
        else
            return false;
        return true;
    }
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_FLOAT)
        out->f = strtof(text, &end);
    else if (iotdata_node_config_type_is_signed(row->type))
        out->i = (int64_t)strtoll(text, &end, 0);
    else if (text[0] == '-')
        return false; /* strtoull would wrap it into something enormous and comfortably in range */
    else
        out->u = (uint64_t)strtoull(text, &end, 0);
    return end != NULL && *end == '\0' && end != text;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// STORE
//
// Fixed-size in RAM so a read is an array index, self-describing on disk so adding or removing an
// entry does not wipe the rest: the image is a sequence of the same [id|type][value] records the
// wire carries, and a load matches each one BY ID. A record whose type or width disagrees with the
// table is skipped and that entry defaults -- which is what makes a firmware change survivable.
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONFIG_STORE_KEY
#define IOTDATA_NODE_CONFIG_STORE_KEY "config"
#endif
/* Ten bytes covers the widest fixed record ([id|type] + u64); a string adds its own declared max
   plus the length byte, and sizeof(_iotdata_node_config_strings) is exactly the sum of those maxima. */
#define IOTDATA_NODE_CONFIG_IMAGE_MAX                                (IOTDATA_NODE_CFG_COUNT * 10u + sizeof(_iotdata_node_config_strings) + 4u)

/* INITIALISED TO THE DEFAULTS AT COMPILE TIME, not left in .bss for iotdata_node_config_defaults() to
   fill. The two say the same thing, and the duplication is the point: a read that happens before
   anything has run -- from an init path ordered earlier than the load, say -- then returns the row's
   DEFAULT rather than zero. Zero is rarely a legal value for anything here and never says so; it
   reads as a plausible number and is acted on. The cost is these bytes moving from .bss to .data. */
#define _IOTDATA_NODE_CFG_DFLT(n, id, t, mn, mx, df, fl, va, no, hp) _IOTDATA_NODE_CFG_D(t, df),
static iotdata_node_config_value_t _iotdata_node_config_value[IOTDATA_NODE_CFG_COUNT] = { IOTDATA_NODE_CONFIG_ENTRIES(_IOTDATA_NODE_CFG_DFLT) };

/* THE X-MACRO HELPERS END HERE. They are expansion machinery, not interface: every one of them has
   done its work by this line, and a header that leaves them defined pushes a hundred names into
   every translation unit that includes it -- names short enough to collide with somebody else's.
   _IOTDATA_NODE_CONFIG_IS is deliberately NOT undefined: the typed accessors expand it at the call
   site, so it has to outlive this file. */
#undef _IOTDATA_NODE_CFG_B
#undef _IOTDATA_NODE_CFG_B_BLOB
#undef _IOTDATA_NODE_CFG_B_BOOL
#undef _IOTDATA_NODE_CFG_B_FLOAT
#undef _IOTDATA_NODE_CFG_B_I16
#undef _IOTDATA_NODE_CFG_B_I32
#undef _IOTDATA_NODE_CFG_B_I64
#undef _IOTDATA_NODE_CFG_B_I8
#undef _IOTDATA_NODE_CFG_B_STRING
#undef _IOTDATA_NODE_CFG_B_U16
#undef _IOTDATA_NODE_CFG_B_U32
#undef _IOTDATA_NODE_CFG_B_U64
#undef _IOTDATA_NODE_CFG_B_U8
#undef _IOTDATA_NODE_CFG_CASE
#undef _IOTDATA_NODE_CFG_D
#undef _IOTDATA_NODE_CFG_DFLT
#undef _IOTDATA_NODE_CFG_D_BLOB
#undef _IOTDATA_NODE_CFG_D_BOOL
#undef _IOTDATA_NODE_CFG_D_FLOAT
#undef _IOTDATA_NODE_CFG_D_I16
#undef _IOTDATA_NODE_CFG_D_I32
#undef _IOTDATA_NODE_CFG_D_I64
#undef _IOTDATA_NODE_CFG_D_I8
#undef _IOTDATA_NODE_CFG_D_STRING
#undef _IOTDATA_NODE_CFG_D_U16
#undef _IOTDATA_NODE_CFG_D_U32
#undef _IOTDATA_NODE_CFG_D_U64
#undef _IOTDATA_NODE_CFG_D_U8
#undef _IOTDATA_NODE_CFG_ID
#undef _IOTDATA_NODE_CFG_IX
#undef _IOTDATA_NODE_CFG_ROW
#undef _IOTDATA_NODE_CFG_SI
#undef _IOTDATA_NODE_CFG_SIX
#undef _IOTDATA_NODE_CFG_SIX_BLOB
#undef _IOTDATA_NODE_CFG_SIX_BOOL
#undef _IOTDATA_NODE_CFG_SIX_FLOAT
#undef _IOTDATA_NODE_CFG_SIX_I16
#undef _IOTDATA_NODE_CFG_SIX_I32
#undef _IOTDATA_NODE_CFG_SIX_I64
#undef _IOTDATA_NODE_CFG_SIX_I8
#undef _IOTDATA_NODE_CFG_SIX_STRING
#undef _IOTDATA_NODE_CFG_SIX_U16
#undef _IOTDATA_NODE_CFG_SIX_U32
#undef _IOTDATA_NODE_CFG_SIX_U64
#undef _IOTDATA_NODE_CFG_SIX_U8
#undef _IOTDATA_NODE_CFG_SI_BLOB
#undef _IOTDATA_NODE_CFG_SI_BOOL
#undef _IOTDATA_NODE_CFG_SI_FLOAT
#undef _IOTDATA_NODE_CFG_SI_I16
#undef _IOTDATA_NODE_CFG_SI_I32
#undef _IOTDATA_NODE_CFG_SI_I64
#undef _IOTDATA_NODE_CFG_SI_I8
#undef _IOTDATA_NODE_CFG_SI_STRING
#undef _IOTDATA_NODE_CFG_SI_U16
#undef _IOTDATA_NODE_CFG_SI_U32
#undef _IOTDATA_NODE_CFG_SI_U64
#undef _IOTDATA_NODE_CFG_SI_U8
#undef _IOTDATA_NODE_CFG_SPT
#undef _IOTDATA_NODE_CFG_SPT_BLOB
#undef _IOTDATA_NODE_CFG_SPT_BOOL
#undef _IOTDATA_NODE_CFG_SPT_FLOAT
#undef _IOTDATA_NODE_CFG_SPT_I16
#undef _IOTDATA_NODE_CFG_SPT_I32
#undef _IOTDATA_NODE_CFG_SPT_I64
#undef _IOTDATA_NODE_CFG_SPT_I8
#undef _IOTDATA_NODE_CFG_SPT_STRING
#undef _IOTDATA_NODE_CFG_SPT_U16
#undef _IOTDATA_NODE_CFG_SPT_U32
#undef _IOTDATA_NODE_CFG_SPT_U64
#undef _IOTDATA_NODE_CFG_SPT_U8
#undef _IOTDATA_NODE_CFG_SST
#undef _IOTDATA_NODE_CFG_SST_BLOB
#undef _IOTDATA_NODE_CFG_SST_BOOL
#undef _IOTDATA_NODE_CFG_SST_FLOAT
#undef _IOTDATA_NODE_CFG_SST_I16
#undef _IOTDATA_NODE_CFG_SST_I32
#undef _IOTDATA_NODE_CFG_SST_I64
#undef _IOTDATA_NODE_CFG_SST_I8
#undef _IOTDATA_NODE_CFG_SST_STRING
#undef _IOTDATA_NODE_CFG_SST_U16
#undef _IOTDATA_NODE_CFG_SST_U32
#undef _IOTDATA_NODE_CFG_SST_U64
#undef _IOTDATA_NODE_CFG_SST_U8
#undef _IOTDATA_NODE_CFG_S_BLOB
#undef _IOTDATA_NODE_CFG_S_BOOL
#undef _IOTDATA_NODE_CFG_S_FLOAT
#undef _IOTDATA_NODE_CFG_S_I16
#undef _IOTDATA_NODE_CFG_S_I32
#undef _IOTDATA_NODE_CFG_S_I64
#undef _IOTDATA_NODE_CFG_S_I8
#undef _IOTDATA_NODE_CFG_S_U16
#undef _IOTDATA_NODE_CFG_S_U32
#undef _IOTDATA_NODE_CFG_S_U64
#undef _IOTDATA_NODE_CFG_S_U8
#undef _IOTDATA_NODE_CFG_TYPE

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * PINNED: stated on the command line, and so immutable for as long as this run lasts.
 *
 * THE ARGUMENT IS A FORCING FUNCTION. Someone standing at the machine said what this value is, and
 * a console write or a manager on the radio changing it afterwards would leave the running node
 * disagreeing with the command that started it -- and the next restart would silently undo their
 * change anyway, because the argument is still there. Refusing is the honest answer; the escape is
 * to restart without the argument.
 *
 * REFUSED FROM EVERYWHERE, local included. Matt's call, and the right one: the reason to pin is
 * usually that something about WHERE this is running makes the value not negotiable -- a port that
 * is this box's port, a station id an installer assigned -- and "not negotiable unless you are
 * sitting at the console" is a different, weaker claim than the one the operator made.
 *
 * NOT A FLAG ON THE ROW. A row's flags are what the BUILD says and are the same on every node; a
 * pin is what this INVOCATION says and differs between two boxes running the same binary. Keeping
 * them apart is why `conf` can show "readonly" and "pinned" as the different facts they are.
 */
static bool _iotdata_node_config_pinned[IOTDATA_NODE_CFG_COUNT];

static inline int iotdata_node_config_index(const uint16_t id) {
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
        if (iotdata_node_config_table[i].id == id)
            return i;
    return -1;
}

static inline int iotdata_node_config_index_by_name(const char *const what) {
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const char *a = iotdata_node_config_table[i].name, *b = what;
        while (*a != '\0' && *b != '\0' && (((*a | 0x20) == (*b | 0x20)) || (*a == '_' && *b == '-')))
            a++, b++;
        if (*a == '\0' && *b == '\0')
            return i;
    }
    /* Then as an id: 0x-prefixed hex, or decimal. */
    const char *d = what;
    unsigned long id = 0;
    unsigned digits = 0;
    const bool hex = (d[0] == '0' && (d[1] == 'x' || d[1] == 'X'));
    if (hex)
        d += 2;
    for (; *d != '\0'; d++, digits++) {
        unsigned v;
        if (*d >= '0' && *d <= '9')
            v = (unsigned)(*d - '0');
        else if (hex && (*d | 0x20) >= 'a' && (*d | 0x20) <= 'f')
            v = (unsigned)((*d | 0x20) - 'a') + 10u;
        else
            return -1; /* not a number either */
        id = id * (hex ? 16u : 10u) + v;
        if (id > IOTDATA_NODE_CONFIG_ID_MAX)
            return -1;
    }
    return (digits > 0) ? iotdata_node_config_index((uint16_t)id) : -1;
}

static inline bool iotdata_node_config_pin(const uint16_t id) {
    const int i = iotdata_node_config_index(id);
    if (i < 0)
        return false;
    _iotdata_node_config_pinned[i] = true;
    return true;
}

static inline bool iotdata_node_config_is_pinned(const uint16_t id) {
    const int i = iotdata_node_config_index(id);
    return (i >= 0) && _iotdata_node_config_pinned[i];
}

static inline const char *iotdata_node_config_refusal(const uint16_t id, const bool remote) {
    const int i = iotdata_node_config_index(id);
    if (i < 0)
        return "not a setting this build has";
    if (_iotdata_node_config_pinned[i])
        return "pinned on the command line -- restart without that argument to change it";
    if ((iotdata_node_config_table[i].flags & IOTDATA_NODE_CONFIG_FLAG_READONLY) != 0u)
        return "read-only: a fact about this node, not a setting";
    if (remote && (iotdata_node_config_table[i].flags & IOTDATA_NODE_CONFIG_FLAG_LOCAL) != 0u)
        return "local-only: it cannot be written over the air";
    return NULL;
}

static inline const iotdata_node_config_row_t *iotdata_node_config_row(const uint16_t id) {
    const int i = iotdata_node_config_index(id);
    return (i < 0) ? NULL : &iotdata_node_config_table[i];
}

static inline void _iotdata_node_config_string_set(const int i, const char *const src, const size_t len) {
    const iotdata_node_config_row_t *const row = &iotdata_node_config_table[i];
    char *const slot = _iotdata_node_config_slot(row);
    if (slot != NULL) {
        const size_t cap = _iotdata_node_config_slot_cap(row);
        const size_t n = (len < cap) ? len : cap;
        if (src != NULL && n > 0)
            memcpy(slot, src, n);
        slot[n] = '\0';
        _iotdata_node_config_value[i].s.p = slot;
        _iotdata_node_config_value[i].s.len = (uint16_t)n;
    }
}

static inline void iotdata_node_config_defaults(void) {
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        _iotdata_node_config_value[i] = iotdata_node_config_table[i].dflt;
        if (iotdata_node_config_table[i].type == IOTDATA_NODE_CONFIG_TYPE_STRING) {
            const char *const d = iotdata_node_config_table[i].dflt.s.p;
            _iotdata_node_config_string_set(i, d, (d != NULL) ? strlen(d) : 0u);
        }
    }
}

static inline bool iotdata_node_config_validate(const iotdata_node_config_row_t *const row, const iotdata_node_config_value_t *const v, const struct iotdata_node_config_update *const u) {
    if (row == NULL || v == NULL)
        return false;
    if (row->validate != NULL)
        return row->validate(row, v, u);
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING) /* the bounds of a string are its LENGTH */
        return v->s.len >= row->min.u && v->s.len <= row->max.u;
    if (iotdata_node_config_type_is_signed(row->type))
        return v->i >= row->min.i && v->i <= row->max.i;
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_FLOAT)
        return v->f >= row->min.f && v->f <= row->max.f;
    return v->u >= row->min.u && v->u <= row->max.u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// READING
//
// Assert-style, and type-checked at COMPILE time: the id carries its type as a sibling enum, so
// iotdata_node_config_u16(LORA_CHANNEL) on a u8 row is a build error rather than a silent widening.
// With a compiled-in table a missing id cannot happen, so there is no bool+out-param and no branch
// at every call site that can never be taken.
// -----------------------------------------------------------------------------------------------------------------------------------------

#define _IOTDATA_NODE_CONFIG_IS(n, want) ((void)sizeof(char[1 - 2 * !((int)(IOTDATA_NODE_CFG_##n##__TYPE) == (int)(want))]))

#define iotdata_node_config_bool(n)      (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_BOOL), _iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].b)
#define iotdata_node_config_u8(n)        (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_U8), (uint8_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].u)
#define iotdata_node_config_u16(n)       (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_U16), (uint16_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].u)
#define iotdata_node_config_u32(n)       (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_U32), (uint32_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].u)
#define iotdata_node_config_i8(n)        (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_I8), (int8_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].i)
#define iotdata_node_config_i16(n)       (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_I16), (int16_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].i)
#define iotdata_node_config_i32(n)       (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_I32), (int32_t)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].i)
#define iotdata_node_config_float(n)     (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_FLOAT), _iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].f)
/* Always NUL-terminated and never NULL: a row that has never been written points at its own
   default, and one that has points at its slot. */
#define iotdata_node_config_string(n)    (_IOTDATA_NODE_CONFIG_IS(n, IOTDATA_NODE_CONFIG_TYPE_STRING), (const char *)_iotdata_node_config_value[IOTDATA_NODE_CFG_IX_##n].s.p)

static inline bool iotdata_node_config_get(const uint16_t id, iotdata_node_config_value_t *const out) {
    const int i = iotdata_node_config_index(id);
    if (i < 0 || out == NULL)
        return false;
    *out = _iotdata_node_config_value[i];
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// WRITING: BEGIN -> STAGE -> COMMIT -> notify
//
// THE WHOLE SET IS VALIDATED BEFORE ANY OF IT IS APPLIED. A rejection rejects EVERYTHING, because a
// half-applied set is worse than none: change a remote node's radio channel and its transmit power
// together, apply one, reject the other, and the node is unreachable and cannot be told so. There
// is deliberately no reject-this-one.
//
// Staging is also what makes CROSS-ENTRY validation possible at all -- "min must be below max" can
// only be checked once both proposed values are in hand, which is why validate sees the update and
// not just its own row.
//
// Notify runs AFTER the commit and the save, and cannot veto: by then it is true. It returns
// whether a reboot is needed, which is OR'd with the row's own flag -- the flag is necessary rather
// than advisory, because a row that validates entirely from its bounds has no handler to ask.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* The image encoder, defined below: the commit saves before it announces, so that a handler which
   acts on a change can never be told about one that failed to persist. */
static inline bool iotdata_node_config_save(datastore_t *ds);

/* WHERE A COMMIT PERSISTS TO, when the datastore blob is not the answer.
 *
 * The datastore is right for a node: an opaque image in NVS or a directory, written whole. It is
 * wrong for a host whose configuration is a file a person edits, where a write has to come back as
 * readable text beside the file it overrides rather than as a blob nothing can inspect. So the sink
 * is swappable, and the commit calls whichever is attached -- still BEFORE it announces, so the
 * rule that a handler is never told about a change that failed to persist holds either way. */
/* Handed the update that caused it, because a sink that writes a DELTA has to know which rows moved
   and cannot work it out afterwards: "differs from the default" is not the same question, and
   answering it instead sweeps up everything an operator put in their own file. By this point the
   update's `touched` has been narrowed to the rows that actually changed. */
typedef bool (*iotdata_node_config_saver_fn)(const struct iotdata_node_config_update *u);
static iotdata_node_config_saver_fn _iotdata_node_config_saver = NULL;
static inline void iotdata_node_config_saver_attach(const iotdata_node_config_saver_fn fn) {
    _iotdata_node_config_saver = fn;
}

/* Does a commit have anywhere to put it? EITHER sink counts, which is the point of there being two:
   a node persists through its datastore and a host through a file of its own, and something that
   only asked about the datastore would tell a host its writes were being thrown away. */
static inline bool iotdata_node_config_persists(const datastore_t *const ds) {
    return (_iotdata_node_config_saver != NULL) || (ds != NULL);
}

typedef struct iotdata_node_config_update {
    iotdata_node_config_value_t staged[IOTDATA_NODE_CFG_COUNT];
    bool touched[IOTDATA_NODE_CFG_COUNT];
    bool rejected;
    bool remote;
    uint8_t count;
} iotdata_node_config_update_t;

static inline bool iotdata_node_config_update_peek(const struct iotdata_node_config_update *const u, const uint16_t id, iotdata_node_config_value_t *const out) {
    const int i = iotdata_node_config_index(id);
    if (i < 0 || out == NULL)
        return false;
    *out = (u != NULL && u->touched[i]) ? u->staged[i] : _iotdata_node_config_value[i];
    return true;
}

static inline void iotdata_node_config_update_begin(iotdata_node_config_update_t *const u, bool remote) {
    if (u != NULL)
        *u = (iotdata_node_config_update_t){ 0 };
    if (remote && u != NULL)
        u->remote = true;
}

static inline bool iotdata_node_config_update_stage(iotdata_node_config_update_t *const u, const uint16_t id, const iotdata_node_config_value_t *const v) {
    if (u == NULL || v == NULL)
        return false;
    const int i = iotdata_node_config_index(id);
    /* staged FIRST, then validated, so a cross-entry check sees this value too */
    const bool was_touched = (i >= 0) && u->touched[i];
    iotdata_node_config_value_t was = { 0 };
    if (i >= 0) {
        was = u->staged[i];
        u->touched[i] = true;
        u->staged[i] = *v;
    }
    const uint8_t refuse = (uint8_t)(IOTDATA_NODE_CONFIG_FLAG_READONLY | (u->remote ? IOTDATA_NODE_CONFIG_FLAG_LOCAL : 0u));
    /* pinned is checked beside the flags and not among them: same refusal, different authority */
    if (i < 0 || _iotdata_node_config_pinned[i] || (iotdata_node_config_table[i].flags & refuse) != 0u || !iotdata_node_config_validate(&iotdata_node_config_table[i], v, u)) {
        if (i >= 0) {
            u->touched[i] = was_touched;
            u->staged[i] = was;
        }
        u->rejected = true;
        return false;
    }
    if (!was_touched)
        u->count++;
    return true;
}

/* Everything staged, in one step: commit to the cache, write the image, then tell the handlers.
   Returns false if anything was rejected, in which case NOTHING was applied. */
static inline bool iotdata_node_config_update_commit(iotdata_node_config_update_t *const u, datastore_t *const ds, bool *const reboot_out) {
    if (u == NULL || u->rejected)
        return false;
    static iotdata_node_config_value_t was[IOTDATA_NODE_CFG_COUNT]; // XXX
    bool changed = false;
    /* `touched` is narrowed here from "the update named it" to "and it actually moved", which is
       what the announce loop below then reads. A string needs this because its slot is its home:
       the pointer never moves, so `was` cannot be compared against the new value afterwards -- the
       old bytes are gone the moment the slot is overwritten. Deciding it here, while both are still
       in hand, is the only place the comparison is honest. */
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        was[i] = _iotdata_node_config_value[i];
        if (u->touched[i]) {
            if (iotdata_node_config_table[i].type == IOTDATA_NODE_CONFIG_TYPE_STRING) {
                /* A STAGED STRING IS BORROWED: it points at the caller's buffer -- the console's argv,
                a decoded TLV, a line off a config file. Staging one and committing it later would be
                a use-after-free; every path commits inside the same call, and this copy is what ends
                the borrow. */
                u->touched[i] = !((_iotdata_node_config_value[i].s.len == u->staged[i].s.len) && _iotdata_node_config_value[i].s.p != NULL && u->staged[i].s.p != NULL &&
                                  memcmp(_iotdata_node_config_value[i].s.p, u->staged[i].s.p, _iotdata_node_config_value[i].s.len) == 0);
                if (u->touched[i]) {
                    _iotdata_node_config_string_set(i, u->staged[i].s.p, u->staged[i].s.len);
                    changed = true;
                }
            } else {
                u->touched[i] = (memcmp(&was[i], &u->staged[i], sizeof(was[i])) != 0);
                if (u->touched[i]) {
                    _iotdata_node_config_value[i] = u->staged[i];
                    changed = true;
                }
            }
        }
    }
    if (!changed)
        return true; /* accepted, and nothing to save or announce */
    if (_iotdata_node_config_saver != NULL)
        (void)_iotdata_node_config_saver(u);
    else
        (void)iotdata_node_config_save(ds);
    const iotdata_node_config_info_t info = { .validation_only = false, .version_changed = false };
    bool reboot = false;
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        if (u->touched[i]) { /* narrowed above to "named AND moved" */
            if ((iotdata_node_config_table[i].flags & IOTDATA_NODE_CONFIG_FLAG_REBOOT) != 0u)
                reboot = true;
            if (iotdata_node_config_table[i].notify != NULL && iotdata_node_config_table[i].notify(&iotdata_node_config_table[i], &was[i], &info))
                reboot = true;
        }
    }
    if (reboot_out != NULL)
        *reboot_out = reboot;
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE IMAGE: one encoder for the datastore and the wire
// -----------------------------------------------------------------------------------------------------------------------------------------

/* One record, read WITHOUT the table: its id, its type, and its value as the type says to read it --
   which is all a gateway has for another node's rows, never compiled into it. A STRING is BORROWED
   from buf. Returns the record's length, or 0 when it cannot be read (truncated, or a type with
   neither a width nor a length), after which no later record can be found either. */
static inline size_t iotdata_node_config_record_peek(const uint8_t *const buf, const size_t len, uint16_t *const id_out, uint8_t *const type_out, iotdata_node_config_value_t *const val_out) {
    if (buf == NULL || len < 3u)
        return 0;
    const uint16_t rec = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    const uint8_t type = iotdata_node_config_rec_type(rec), w = iotdata_node_config_type_size(type);
    iotdata_node_config_value_t v = { 0 };
    size_t n;
    /* A string is skippable by a build that has never heard of this id, which is the whole reason
       the length is on the wire and not only in the table. */
    if (type == IOTDATA_NODE_CONFIG_TYPE_STRING) {
        n = 3u + (size_t)buf[2];
        if (len < n)
            return 0; /* truncated: stop rather than guess where the next record begins */
        v.s.p = (const char *)&buf[3];
        v.s.len = buf[2];
    } else {
        n = (size_t)(2u + w);
        if (w == 0 || len < n)
            return 0; /* unreadable: stop rather than guess where the next record begins */
        uint64_t raw = 0;
        for (uint8_t b = 0; b < w; b++)
            raw = (raw << 8) | buf[2u + b];
        if (type == IOTDATA_NODE_CONFIG_TYPE_FLOAT) {
            const uint32_t bits = (uint32_t)raw;
            memcpy(&v.f, &bits, sizeof(float));
        } else if (iotdata_node_config_type_is_signed(type)) {
            const uint8_t shift = (uint8_t)(64u - 8u * w);
            v.i = (int64_t)(raw << shift) >> shift; /* sign-extend */
        } else if (type == IOTDATA_NODE_CONFIG_TYPE_BOOL)
            v.b = (raw != 0u);
        else
            v.u = raw;
    }
    if (id_out != NULL)
        *id_out = iotdata_node_config_rec_id(rec);
    if (type_out != NULL)
        *type_out = type;
    if (val_out != NULL)
        *val_out = v;
    return n;
}

/* A record for THIS build: the value only for a row it has, at the type it has -- anything else is
   skipped by its length, the ignore-unknown rule. */
static inline size_t iotdata_node_config_record_read(const uint8_t *const buf, const size_t len, uint16_t *const id_out, int *const index_out, iotdata_node_config_value_t *const val_out) {
    if (index_out != NULL)
        *index_out = -1;
    uint16_t id = 0;
    uint8_t type = 0;
    iotdata_node_config_value_t v = { 0 };
    const size_t n = iotdata_node_config_record_peek(buf, len, &id, &type, &v);
    if (n == 0)
        return 0;
    if (id_out != NULL)
        *id_out = id;
    const int i = iotdata_node_config_index(id);
    if (i >= 0 && iotdata_node_config_table[i].type == type && val_out != NULL) {
        *val_out = v;
        if (index_out != NULL)
            *index_out = i;
    }
    return n;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline size_t iotdata_node_config_decode(const uint8_t *const buf, const size_t len) {
    uint16_t id = 0;
    int i = -1;
    iotdata_node_config_value_t v;
    const size_t n = iotdata_node_config_record_read(buf, len, &id, &i, &v);
    if (n > 0 && i >= 0)
        _iotdata_node_config_value[i] = v;
    return n;
}

/* Append one entry as [id|type][value], big-endian, the same shape the TLV carries. Returns the
   bytes written, or 0 if it does not fit. */
static inline size_t iotdata_node_config_encode(const int i, uint8_t *const buf, const size_t size) {
    if (i < 0 || i >= (int)IOTDATA_NODE_CFG_COUNT || buf == NULL)
        return 0;
    const iotdata_node_config_row_t *const row = &iotdata_node_config_table[i];
    const uint16_t rec = iotdata_node_config_rec(row->id, row->type);
    /* A STRING carries its own length, because it has to: [id|type][len][bytes]. A fixed-width type
       does not, because the type already said. The 255 ceiling is the length byte's, which is also
       why a row may not declare a max above it. */
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING) {
        const uint16_t n = _iotdata_node_config_value[i].s.len;
        if (n > 255u || (size_t)(3u + n) > size)
            return 0;
        buf[0] = (uint8_t)(rec >> 8);
        buf[1] = (uint8_t)rec;
        buf[2] = (uint8_t)n;
        if (n > 0 && _iotdata_node_config_value[i].s.p != NULL)
            memcpy(&buf[3], _iotdata_node_config_value[i].s.p, n);
        return (size_t)(3u + n);
    }
    const uint8_t w = iotdata_node_config_type_size(row->type);
    if (w == 0 || (size_t)(2u + w) > size)
        return 0; /* a type with neither a width nor a length: nothing sensible to write */
    buf[0] = (uint8_t)(rec >> 8);
    buf[1] = (uint8_t)rec;
    uint64_t raw = iotdata_node_config_type_is_signed(row->type) ? (uint64_t)_iotdata_node_config_value[i].i : _iotdata_node_config_value[i].u;
    if (row->type == IOTDATA_NODE_CONFIG_TYPE_FLOAT) {
        uint32_t bits;
        memcpy(&bits, &_iotdata_node_config_value[i].f, sizeof(bits));
        raw = bits;
    }
    for (uint8_t b = 0; b < w; b++)
        buf[2u + b] = (uint8_t)(raw >> (8u * (w - 1u - b)));
    return (size_t)(2u + w);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_config_pack(uint8_t *const buf, const size_t size, iotdata_node_partial_t *const p) {
    size_t at = 0;
    uint8_t packed = 0;
    bool more = false;
    uint32_t cur = (p != NULL) ? p->cursor : 0u;
    for (; cur < (uint32_t)IOTDATA_NODE_CFG_COUNT; cur++) {
        const size_t n = iotdata_node_config_encode((int)cur, buf + at, size - at);
        if (n > 0) {
            at += n;
            packed++;
        } else {
            if (at > 0) {
                more = true; /* no room left in THIS chunk: the next frame resumes here */
                break;
            }
        }
    }
    if (p != NULL) {
        p->total = (uint8_t)IOTDATA_NODE_CFG_COUNT;
        p->chunk = packed;
        p->more = more;
        p->cursor = more ? cur : 0u; /* 0: the table is done, a later request restarts it */
    }
    return (int)at;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static bool _iotdata_node_config_written[IOTDATA_NODE_CFG_COUNT];

static inline bool iotdata_node_config_apply(const uint8_t *const buf, const size_t len, datastore_t *const ds, bool *const reboot_out) {
    iotdata_node_config_update_t u;
    iotdata_node_config_update_begin(&u, true);
    memset(_iotdata_node_config_written, 0, sizeof(_iotdata_node_config_written));
    for (size_t at = 0; at < len;) {
        uint16_t id = 0;
        int i = -1;
        iotdata_node_config_value_t v;
        const size_t n = iotdata_node_config_record_read(buf + at, len - at, &id, &i, &v);
        if (n == 0)
            break;
        if (i >= 0) {
            _iotdata_node_config_written[i] = true;
            (void)iotdata_node_config_update_stage(&u, id, &v); /* a rejection poisons the whole update */
        }
        at += n;
    }
    return iotdata_node_config_update_commit(&u, ds, reboot_out);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_config_pack_written(uint8_t *const buf, const size_t size) {
    size_t at = 0;
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
        if (_iotdata_node_config_written[i]) {
            const size_t n = iotdata_node_config_encode(i, buf + at, size - at);
            if (n == 0)
                break; /* cannot happen for a write that itself fitted one frame, but never overrun */
            at += n;
        }
    return (int)at;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static uint8_t _iotdata_node_config_buffer[IOTDATA_NODE_CONFIG_IMAGE_MAX]; // XXX

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_config_save(datastore_t *const ds) {
    if (ds == NULL)
        return false;
    size_t at = 0;
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
        at += iotdata_node_config_encode(i, _iotdata_node_config_buffer + at, sizeof(_iotdata_node_config_buffer) - at);
    return datastore_write(ds, IOTDATA_NODE_CONFIG_STORE_KEY, _iotdata_node_config_buffer, at);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_config_load(datastore_t *const ds) {
    iotdata_node_config_defaults(); // defaults first
    if (ds == NULL)
        return false;
    size_t len = 0;
    if (datastore_read(ds, IOTDATA_NODE_CONFIG_STORE_KEY, _iotdata_node_config_buffer, sizeof(_iotdata_node_config_buffer), &len)) {
        for (size_t at = 0; at < len;) {
            const size_t n = iotdata_node_config_decode(_iotdata_node_config_buffer + at, len - at);
            if (n == 0)
                break;
            at += n;
        }
        return true;
    }
    return false;
}
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_NODE_CONSOLE_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE CONSOLE
//
// Appears by itself when iotdata_node_console.h was included before this expansion. A node with no
// console compiles none of it.
//
// THE CONSOLE RESOLVES NAMES, and it is the only part of the system that should. It has the table
// compiled in, it is sitting in front of a person, and it cannot go stale relative to the firmware
// because it IS the firmware. The gateway deliberately resolves nothing and relays ids, which is
// what keeps it free of a name table it would have to hold in step with every build in the fleet.
// The two are the same rule, not two: resolution goes as close to the human as it will go.
//
// A console write is a LOCAL one, so FLAG_LOCAL rows are settable here and only here.
//
//     conf                    every row, with its value
//     conf <name|id>          one row, with its bounds and flags
//     conf <name|id> <value>  set it, then print what is now true
// -----------------------------------------------------------------------------------------------------------------------------------------

static datastore_t *_iotdata_node_config_console_ds = NULL;

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_config_console_attach(datastore_t *const ds) {
    _iotdata_node_config_console_ds = ds;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_config_show(const iotdata_node_console_emit_fn emit, const int i, const bool detail) {
    const iotdata_node_config_row_t *const row = &iotdata_node_config_table[i];
    char val[32];
    _iotdata_node_config_fmt(row, &_iotdata_node_config_value[i], val, sizeof(val));
    if (detail) {
        char lo[32], hi[32];
        emit("%s = %s\n", row->name, val);
        if (row->help != NULL)
            emit("  %s\n", row->help);
        if (row->type == IOTDATA_NODE_CONFIG_TYPE_STRING)
            emit("  id=0x%03X type=%s length=%u..%u (%u byte slot)\n", (unsigned)row->id, _iotdata_node_config_type_name(row->type), (unsigned)row->min.u, (unsigned)row->max.u, (unsigned)row->max.u + 1u);
        else
            emit("  id=0x%03X type=%s range=%s..%s\n", (unsigned)row->id, _iotdata_node_config_type_name(row->type), _iotdata_node_config_fmt(row, &row->min, lo, sizeof(lo)), _iotdata_node_config_fmt(row, &row->max, hi, sizeof(hi)));
        emit("  %s%s%s%s%s\n", (row->flags & IOTDATA_NODE_CONFIG_FLAG_READONLY) ? "read-only " : "", (row->flags & IOTDATA_NODE_CONFIG_FLAG_LOCAL) ? "local-only " : "",
             (row->flags & IOTDATA_NODE_CONFIG_FLAG_STARTUP) ? "command-line-only " : "", (row->flags & IOTDATA_NODE_CONFIG_FLAG_REBOOT) ? "reboot-required " : "", row->flags == 0 ? "effective-immediately" : "");
        /* on its own line, and in words: a pin is about THIS INVOCATION, not about the row, and the
           way out of it is an instruction rather than a property */
        if (_iotdata_node_config_pinned[i])
            emit("  PINNED on the command line -- restart without that argument to change it\n");
    } else
        emit("  %-32s = %-12s [0x%03X %s%s%s%s%s]\n", row->name, val, (unsigned)row->id, _iotdata_node_config_type_name(row->type), (row->flags & IOTDATA_NODE_CONFIG_FLAG_READONLY) ? " ro" : "",
             (row->flags & IOTDATA_NODE_CONFIG_FLAG_LOCAL) ? " local" : "", (row->flags & IOTDATA_NODE_CONFIG_FLAG_REBOOT) ? " reboot" : "", _iotdata_node_config_pinned[i] ? " pinned" : "");
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* UNUSED IS NOT AN ERROR HERE: including the adapter and registering the command are separate decisions. */
__attribute__((unused)) static void iotdata_node_config_console(const iotdata_node_console_emit_fn emit, __attribute__((unused)) const uint32_t now_ms, const int argc, char **const argv) {
    if (argc < 2) {
        emit("config: %u entries%s\n", (unsigned)IOTDATA_NODE_CFG_COUNT, iotdata_node_config_persists(_iotdata_node_config_console_ds) ? "" : " (NOT PERSISTED: nowhere to write them)");
        for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++)
            _iotdata_node_config_show(emit, i, false);
        return;
    }
    const int i = iotdata_node_config_index_by_name(argv[1]);
    if (i < 0) {
        emit("%s: no such entry (try `%s` with no argument)\n", argv[1], argv[0]);
        return;
    }
    if (argc < 3) {
        _iotdata_node_config_show(emit, i, true);
        return;
    }
    iotdata_node_config_value_t v;
    if (!_iotdata_node_config_parse(&iotdata_node_config_table[i], argv[2], &v)) {
        emit("%s: '%s' is not a %s\n", iotdata_node_config_table[i].name, argv[2], _iotdata_node_config_type_name(iotdata_node_config_table[i].type));
        return;
    }
    iotdata_node_config_update_t u;
    iotdata_node_config_update_begin(&u, false);
    bool reboot = false;
    if (!iotdata_node_config_update_stage(&u, iotdata_node_config_table[i].id, &v) || !iotdata_node_config_update_commit(&u, _iotdata_node_config_console_ds, &reboot)) {
        const char *const why = iotdata_node_config_refusal(iotdata_node_config_table[i].id, false);
        emit("%s: refused -- %s\n", iotdata_node_config_table[i].name, (why != NULL) ? why : "out of range, or a cross-entry rule");
        _iotdata_node_config_show(emit, i, true); /* the response is the truth: say what it still is, and why */
        return;
    }
    _iotdata_node_config_show(emit, i, false);
    if (reboot)
        emit("  (requires restart)\n");
    if (!iotdata_node_config_persists(_iotdata_node_config_console_ds))
        emit("  (not persisted: this node has nowhere to write it)\n");
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_config_help(const iotdata_node_console_emit_fn emit, const char *const prog) {
    char dflt[64];
    emit("usage: %s [--<setting> <value>] ...\n\n", (prog != NULL) ? prog : "program");
    for (int i = 0; i < (int)IOTDATA_NODE_CFG_COUNT; i++) {
        const iotdata_node_config_row_t *const row = &iotdata_node_config_table[i];
        char opt[80];
        size_t at = 0;
        opt[at++] = '-';
        opt[at++] = '-';
        for (const char *p = row->name; *p != '\0' && at + 2u < sizeof(opt); p++)
            opt[at++] = (*p == '_') ? '-' : (char)(*p | 0x20);
        opt[at] = '\0';
        emit("  %-34s %s\n", opt, (row->help != NULL) ? row->help : "");
        emit("  %-34s   %s, default %s%s%s%s\n", "", _iotdata_node_config_type_name(row->type), _iotdata_node_config_fmt(row, &row->dflt, dflt, sizeof(dflt)), (row->flags & IOTDATA_NODE_CONFIG_FLAG_READONLY) ? ", read-only" : "",
             (row->flags & IOTDATA_NODE_CONFIG_FLAG_STARTUP) ? ", command line only" : "", (row->flags & IOTDATA_NODE_CONFIG_FLAG_REBOOT) ? ", needs a restart" : "");
    }
}

/* Drop this into the application's iotdata_node_console_t array. */
#define IOTDATA_NODE_CONFIG_CONSOLE_COMMAND { "conf", iotdata_node_config_console, "show or set configuration: conf [<name|id> [<value>]]" }

// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONSOLE_H */

#endif /* IOTDATA_NODE_CONFIG_ENTRIES && !IOTDATA_NODE_CONFIG_EXPANDED */
