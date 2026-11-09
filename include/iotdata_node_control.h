#ifndef IOTDATA_NODE_CONTROL_H
#define IOTDATA_NODE_CONTROL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_control.h - driving a node: the command vocabulary, its packaging, and the
// translation to and from whatever medium the operator is using.
//
// A TABLE, so a medium is a thin adapter. Each medium fills an iotdata_node_control_args_t from
// whatever it has -- JSON members here, argv on a console later -- and the packaging is shared.
// Adding a medium should not touch the vocabulary, and adding a command should not touch a medium.
//
// A NODE PLUGS IN ITS OWN. iotdata_node_control_init() registers commands specific to one device (a
// TSA's "calibrate", say). The built-ins are searched FIRST, so an application cannot quietly
// shadow `boot` or `stat` and leave an operator holding a word that means something else here.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* How many stations one request may name. Bounded because each becomes its own DOWN frame in the
   gateway's staging queue: a request that would outrun that queue is refused whole, rather than
   half-delivered with no way to say which half. */
#ifndef IOTDATA_NODE_CONTROL_TARGETS_MAX
#define IOTDATA_NODE_CONTROL_TARGETS_MAX 8
#endif

/*
 * THE MQTT MANAGEMENT CHANNEL. Requests arrive on one flat topic; everything a station has to say
 * goes out under a subtree, at <prefix>/manage/from/<station>, keyed the same way the telemetry
 * topics are. So `manage/from/#` is the whole fleet and `manage/from/F1B` is one node, without a
 * client having to filter either.
 *
 * REQUESTS STAY FLAT. The gateway subscribes to them and wants all of them, the target is already
 * in the payload, and putting it in the topic as well would be a second source of truth for the
 * same fact. The two directions stay in disjoint subtrees, so neither wildcard catches the other.
 */
#define IOTDATA_NODE_MQTT_MANAGE_TOPIC_REQ  "/manage/req"
#define IOTDATA_NODE_MQTT_MANAGE_TOPIC_FROM "/manage/from"

/*
 * TELEMETRY, under its own category for the same reason: <prefix>/variant/<name>/<station>.
 *
 * THE FIRST SEGMENT AFTER THE PREFIX IS ALWAYS A CATEGORY -- `manage`, `stats`, `variant` -- and
 * never a payload kind. It used to be <prefix>/<variant name>/<station>, which put `weather_station`
 * at the same level as `manage`: a reader could not tell structurally whether a segment named a
 * kind of thing or one particular variant, "all telemetry" meant "everything except the two
 * categories I happen to know about", and a variant named like a category would have collided.
 * One level down, `variant/#` is every reading and `variant/snow_depth/#` is one kind of them.
 */
#define IOTDATA_NODE_MQTT_VARIANT_TOPIC     "/variant"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* What a command does with the request's arguments. The kind is a property of the COMMAND, not of
   the medium, which is what lets one table serve all of them. */
typedef enum {
    IOTDATA_NODE_CONTROL_ARG_NONE,             /* a flag: the key present, no value */
    IOTDATA_NODE_CONTROL_ARG_FIXED,            /* a u8 the command itself fixes (enable = 1, disable = 0) */
    IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS,     /* a u8 from "scope", read as STATUS_SCOPE_* (which groups) */
    IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER,     /* a u8 from "scope", read as FILTER_SCOPE_* (which entries) */
    IOTDATA_NODE_CONTROL_ARG_STATION,          /* { u16 station, u8 action } -- the action fixed by the command */
    IOTDATA_NODE_CONTROL_ARG_STATION_ACTION,   /* { u16 station, u8 action } -- the action named in the request */
    IOTDATA_NODE_CONTROL_ARG_REPORTS,          /* not one key: every report that can be asked for */
    IOTDATA_NODE_CONTROL_ARG_CONTENT,          /* u24 content id -- §4's width, which is not a station's */
    IOTDATA_NODE_CONTROL_ARG_CONTENT_AUDIENCE, /* { u24 content id, u16 station } -- who an offer is for */
    IOTDATA_NODE_CONTROL_ARG_MANIFEST,         /* a whole offer: see the note by ARG_MANIFEST below */
} iotdata_node_control_arg_t;

/* How long a name a MANIFEST command may carry. It is NOT the row's bound: a node truncates to
   whatever its own IOTDATA_NODE_CONTENT_NAME_MAX is, which this header cannot see because it is
   read before the content module. Wider than the 32 bytes §3 recommends, so that typing a longer
   one is the node's decision to shorten rather than this layer's to refuse. */
#ifndef IOTDATA_NODE_CONTROL_CONTENT_NAME_MAX
#define IOTDATA_NODE_CONTROL_CONTENT_NAME_MAX 48
#endif

/*
 * THE WIDEST kvr iotdata_node_control_build() CAN PRODUCE, so a medium sizes its buffer from the
 * vocabulary instead of counting the current widest command by hand.
 *
 * HAND-COUNTED BOUNDS HERE HAVE NOW GONE STALE TWICE. At UPDATE_ENTRY_SIZE+2 every
 * `* update <station> <action>` typed at a device console overflowed by two and answered "cannot
 * build"; the fix was +4 and a comment explaining the arithmetic. Then MANIFEST arrived -- nine
 * fixed bytes and a name -- and overflowed the same buffer by twenty, so `content manifest` worked
 * over MQTT and said "cannot build" at a console. The gateway's own buffer was a flat 32 and was
 * one character of name away from the same failure. Every time, the number lived in an application
 * and a change to THIS table quietly outgrew it.
 *
 * Two shapes compete for the maximum: ONE PAIR carrying the widest argument, which is a manifest;
 * and REPORTS, which is not one pair but one per reportable type.
 */
#define _IOTDATA_NODE_CONTROL_PAIR_MAX          (2u + 2u + 9u + IOTDATA_NODE_CONTROL_CONTENT_NAME_MAX + 1u) /* key, vlen, subject, action, the fixed fields, the name, its NUL */
#define _IOTDATA_NODE_CONTROL_REPORTS_MAX       (3u * IOTDATA_NODE_TLV_SYSTEM_COUNT)                        /* key, vlen, subject -- once per reportable type */
#define IOTDATA_NODE_CONTROL_BUILD_MAX          ((_IOTDATA_NODE_CONTROL_PAIR_MAX > _IOTDATA_NODE_CONTROL_REPORTS_MAX) ? _IOTDATA_NODE_CONTROL_PAIR_MAX : _IOTDATA_NODE_CONTROL_REPORTS_MAX)

#define IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST 0xFF

typedef struct {
    const char *name;
    uint8_t key;     /* IOTDATA_NODE_CONTROL_REQUEST or _CONTROL */
    uint8_t subject; /* a TLV type, or NODE / MESH */
    uint8_t action;  /* only meaningful when key == _CONTROL */
    iotdata_node_control_arg_t arg;
    uint8_t fixed; /* FIXED: the value. STATION: the action. SCOPE: an override, or FROM_REQUEST. */
} iotdata_node_control_command_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Every CONTROL value begins the same way: the subject, and for a command its action. Whatever the
   argument kind adds, it adds after this. */
static inline uint8_t _iotdata_node_control_prefix(const iotdata_node_control_command_t *const c, uint8_t *const out) {
    out[0] = c->subject;
    if (c->key == IOTDATA_NODE_CONTROL_REQUEST)
        return 1;
    out[1] = c->action;
    return 2;
}

/* Everything a command might need, however the medium expressed it. One struct rather than a
   parameter list, because the next medium fills the same fields from argv. */
typedef struct {
    /* Who it is for. `target` is simply the first of `targets`, so a request naming one station --
       which is most of them -- reads exactly as it always did. A LIST is the general case: the
       wire carries ONE 12-bit station per DOWN frame, so addressing several means the gateway
       emitting several frames, and that fan-out is the gateway's business rather than the
       requester's. Broadcast absorbs the rest: asking for "all, plus 0537" is asking for all. */
    uint16_t target; /* the node to drive; BROADCAST when unsaid */
    uint16_t targets[IOTDATA_NODE_CONTROL_TARGETS_MAX];
    uint8_t targets_count;
    bool targets_overflow; /* the request named more than fit: it must be refused, not truncated */
    uint16_t station;      /* the station a mesh command acts ON, which is not the target */
    uint8_t scope_status;  /* STATUS_SCOPE_* bits; 0 = every group */
    uint8_t scope_filter;  /* FILTER_SCOPE_*; ALL when unsaid */
    uint8_t action;        /* FILTERS_BLOCK / _ALLOW / _NONE */
    uint32_t content;      /* a content id: 24 bits, 0 when unsaid (and 0x000xxx is unaddressable) */
    /* A MANIFEST's fields, and they are exactly the ones the bytes cannot say: a node authoring an
       entry reads the size and the gross hash out of its own store, so there is nothing here that
       could be mistyped into a wrong offer. INDEFINITE is the default expiry rather than zero
       because zero is §3's revocation sentinel -- an omitted expiry must not withdraw the thing it
       was meant to offer -- and because an offer standing until it is replaced is the common case. */
    uint8_t content_encoding;  /* content-type:4 | compression:4 */
    uint16_t content_expiry_m; /* minutes from receipt; INDEFINITE when unsaid */
    /* u24, the block-hash stream. NO DEFAULT WORTH THE NAME: every layer below takes any value,
       and the two places a person states an offer -- this header's argv parser and the control
       tool -- refuse to build the command without one. See ARG_MANIFEST's parser. */
    uint32_t content_sig;
    char content_name[IOTDATA_NODE_CONTROL_CONTENT_NAME_MAX + 1];
} iotdata_node_control_args_t;

// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT HAPPENED TO IT
//
// THREE ANSWERS, NOT TWO. An app's control handler used to return a bool, and a bool cannot tell
// "I have never heard of this" apart from "I know exactly what you mean and I am not doing it".
// Both came back false, were counted unknown, and the console said "not implemented here" -- so a
// `node rollback` on a node with no fallback image read as though the firmware had been built
// without the feature, which is the one reading that sends an operator off to check build flags.
//
// The other direction was worse, because it was silent. A handler that wanted the refusal to count
// as SERVICED returned true instead -- the relay's cart command does, with CART_ENABLE off -- and
// then the console said "ok" for a command that did nothing at all. Two outcomes, three meanings,
// and every site picking which lie it preferred.
//
// REFUSED IS A SUCCESSFUL DELIVERY. It says the node has the verb, understood the request, and
// declined on a condition it can name: there is no fallback slot, the feature is configured off,
// the store is not mounted. That is an answer to the operator and not a fault, so it is counted
// apart from both -- a run of refusals is a node that needs its configuration looked at, a run of
// unknowns is a manager talking to the wrong kind of node, and the two want different fixing.
//
// THE REASON IS LOCAL, AND SO, FOR NOW, IS THE WHOLE ANSWER. A handler may set `*reason` to a
// short literal; it reaches the console in front of the node and the log line, and nothing else.
// A command arriving over the air is NOT acknowledged at all -- there is no reply TLV for one, and
// these counters are read by the console and by nothing that leaves the node. So a manager that
// pushes a command to a remote node still cannot tell serviced from refused from unknown, and this
// change does not pretend to fix that: it makes the three distinguishable AT the node, and leaves
// the counters true for whoever surfaces them. See the control-acknowledgement item in
// iotdata-specs/NOTES_ISSUES.md, which is a protocol decision and not a patch.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    /* 0, so a zeroed result and an older handler's `false` both still mean unknown */
    IOTDATA_NODE_CONTROL_RESULT_UNKNOWN = 0,
    IOTDATA_NODE_CONTROL_RESULT_DONE = 1,    /* 1, so an older handler's `true` still means done */
    IOTDATA_NODE_CONTROL_RESULT_REFUSED = 2, /* mine, understood, and deliberately not done */
} iotdata_node_control_result_t;

/* For a module whose own API still answers the narrower question "was it mine?" with a bool -- the
   recorder's does, and has no refusal to express, because a build with no recorder registers no
   handler at all and is honestly unknown. Spelled out rather than left to an implicit conversion,
   which -Wconversion is right to be suspicious of. */
static inline iotdata_node_control_result_t iotdata_node_control_result_of(const bool claimed) {
    return claimed ? IOTDATA_NODE_CONTROL_RESULT_DONE : IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
}

/* For a console, a log line, or an MQTT answer: the outcome in the words an operator wants, which
   is why "refused" is not spelled "failed". Nothing failed. */
static inline const char *iotdata_node_control_result_name(const iotdata_node_control_result_t r) {
    switch (r) {
    case IOTDATA_NODE_CONTROL_RESULT_DONE:
        return "ok";
    case IOTDATA_NODE_CONTROL_RESULT_REFUSED:
        return "refused";
    default:
        return "not implemented here";
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE VOCABULARY
// -----------------------------------------------------------------------------------------------------------------------------------------

static const iotdata_node_control_command_t iotdata_node_control_commands[] = {

/* Shorthands, so the table reads as a table rather than as a wall of prefixes. */
#define _REQ(subject)         IOTDATA_NODE_CONTROL_REQUEST, (subject), 0
#define _CMD(subject, action) IOTDATA_NODE_CONTROL_CONTROL, (subject), (action)

    /* --- the system TLVs, one request each --------------------------------------------------
       RECEIVE is here now. Under the old scheme its derived key was 0x00, which REBOOT already
       had, so "when are you next reachable?" was a question the protocol could not ask. */
    { "recv", _REQ(IOTDATA_NODE_TLV_RECEIVE), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "vers", _REQ(IOTDATA_NODE_TLV_VERSION), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "vari", _REQ(IOTDATA_NODE_TLV_VARIANT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "ctrl", _REQ(IOTDATA_NODE_TLV_CONTROL), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "stat", _REQ(IOTDATA_NODE_TLV_STATUS), IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS, IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST },
    { "conf", _REQ(IOTDATA_NODE_TLV_CONFIG), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "diag", _REQ(IOTDATA_NODE_TLV_DIAGNOSTICS), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "cont", _REQ(IOTDATA_NODE_TLV_CONTENT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "reports", 0, 0, 0, IOTDATA_NODE_CONTROL_ARG_REPORTS, 0 }, /* every report, in type order */

    /* --- node control ------------------------------------------------------------------------ */
    { "boot", _CMD(IOTDATA_NODE_SUBJECT_NODE, IOTDATA_NODE_ACTION_NODE_REBOOT), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

    /* --- diag -------------------------------------------------------------------------------- */
    { "diag-bbox-enable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 1 },
    { "diag-bbox-disable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 0 },
    { "diag-bbox-clear", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_CLEAR), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "diag-bbox-dump", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_BBOX_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "diag-cart-enable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_CART_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 1 },
    { "diag-cart-disable", _CMD(IOTDATA_NODE_TLV_DIAGNOSTICS, IOTDATA_NODE_ACTION_DIAG_CART_ENABLE), IOTDATA_NODE_CONTROL_ARG_FIXED, 0 },
    /* --- node management --------------------------------------------------------------------- */
    { "node-stations-dump", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_NODE_STATIONS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "node-filters-update", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_NODE_FILTERS_UPDATE), IOTDATA_NODE_CONTROL_ARG_STATION_ACTION, 0 },
    { "node-filters-clear", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_NODE_FILTERS_CLEAR), IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER, IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST },
    { "node-filters-dump", _CMD(IOTDATA_NODE_TLV_STATUS, IOTDATA_NODE_ACTION_NODE_FILTERS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    /* node-ROLLBACK and not a bare `rollback`, for the reason the console table states: one verb per
       prefix, and a bare name needs a console verb of its own to be typeable at all. `boot` gets
       away with it only because a console BUILTIN shadows it. This way `node rollback` works, the
       `node` usage lists it, and the wire name matches its subject. */
    { "node-rollback", _CMD(IOTDATA_NODE_SUBJECT_NODE, IOTDATA_NODE_ACTION_NODE_ROLLBACK), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    /* --- content -----------------------------------------------------------------------------
       The store's words that have a wire equivalent. `insert` and `download` are NOT here and
       never will be in this shape: both are a byte stream, and the wire's answer to that is the
       CONTENT TLVs rather than a command with a very large argument. */
    { "content-list", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "content-delete", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE), IOTDATA_NODE_CONTROL_ARG_CONTENT, 0 },
    { "content-inspect", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT), IOTDATA_NODE_CONTROL_ARG_CONTENT, 0 },
    { "content-validate", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_VALIDATE), IOTDATA_NODE_CONTROL_ARG_CONTENT, 0 },
    { "content-ota", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_STATUS), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "content-ota-apply", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_APPLY), IOTDATA_NODE_CONTROL_ARG_CONTENT, 0 },
    { "content-manifest", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET), IOTDATA_NODE_CONTROL_ARG_MANIFEST, 0 },
    { "content-manifests", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "content-advertise", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE), IOTDATA_NODE_CONTROL_ARG_CONTENT_AUDIENCE, 0 },
    { "content-revoke", _CMD(IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_REVOKE), IOTDATA_NODE_CONTROL_ARG_CONTENT, 0 },
    /* --- mesh management --------------------------------------------------------------------- */
    { "mesh-peers-update", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_UPDATE), IOTDATA_NODE_CONTROL_ARG_STATION_ACTION, 0 },
    { "mesh-peers-clear", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
    { "mesh-peers-dump", _CMD(IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_DUMP), IOTDATA_NODE_CONTROL_ARG_NONE, 0 },

#undef _REQ
#undef _CMD
};

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Commands one device adds to the common set -- see iotdata_node_control_init. */
static const iotdata_node_control_command_t *_iotdata_node_control_app = NULL;
static uint8_t _iotdata_node_control_app_count = 0;

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_control_init(const iotdata_node_control_command_t *const app, const uint8_t count) {
    _iotdata_node_control_app = app;
    _iotdata_node_control_app_count = (app != NULL) ? count : 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_node_control_command_t *iotdata_node_control_find(const char *const name) {
    if (name == NULL)
        return NULL;
    for (uint8_t i = 0; i < ((uint8_t)(sizeof(iotdata_node_control_commands) / sizeof(iotdata_node_control_commands[0]))); i++) // builtins first
        if (strcmp(iotdata_node_control_commands[i].name, name) == 0)
            return &iotdata_node_control_commands[i];
    for (uint8_t i = 0; i < _iotdata_node_control_app_count; i++)
        if (strcmp(_iotdata_node_control_app[i].name, name) == 0)
            return &_iotdata_node_control_app[i];
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_node_control_command_t *iotdata_node_control_at(const uint8_t index) {
    if (index < ((uint8_t)(sizeof(iotdata_node_control_commands) / sizeof(iotdata_node_control_commands[0]))))
        return &iotdata_node_control_commands[index];
    const uint8_t i = (uint8_t)(index - ((uint8_t)(sizeof(iotdata_node_control_commands) / sizeof(iotdata_node_control_commands[0]))));
    return (i < _iotdata_node_control_app_count) ? &_iotdata_node_control_app[i] : NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// PACKAGING
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Build the CONTROL payload for one command. Returns false when nothing was added. */
static inline bool iotdata_node_control_build(iotdata_kvr_t *const kv, const iotdata_node_control_command_t *const c, const iotdata_node_control_args_t *const a) {
    if (kv == NULL || c == NULL || a == NULL)
        return false;
    static uint8_t v[IOTDATA_TLV_VALUE_MAX]; // XXX
    uint8_t n = _iotdata_node_control_prefix(c, v);

    switch (c->arg) {
    case IOTDATA_NODE_CONTROL_ARG_NONE:
        break;
    case IOTDATA_NODE_CONTROL_ARG_FIXED:
        v[n++] = c->fixed;
        break;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER: {
        const bool status = (c->arg == IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS);
        const uint8_t from_request = status ? a->scope_status : a->scope_filter;
        const uint8_t want = (c->fixed == IOTDATA_NODE_CONTROL_SCOPE_FROM_REQUEST) ? from_request : c->fixed;
        /*
         * A zero scope is omittable for STATUS and not for FILTERS, and the difference is in what
         * zero MEANS rather than in any declared width.
         *
         * An absent STATUS scope already means "the default groups", so sending an explicit zero
         * would say the same thing a second time and cost a byte. But FILTERS_SCOPE_ALL is 0x00
         * and is a real instruction -- "clear everything" -- so dropping it would turn a request
         * into a silence, and a reader would have to guess which was meant.
         */
        if (want != 0 || !status)
            v[n++] = want;
        break;
    }
    case IOTDATA_NODE_CONTROL_ARG_STATION:
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION:
        v[n++] = (uint8_t)(a->station >> 8);
        v[n++] = (uint8_t)a->station;
        v[n++] = (c->arg == IOTDATA_NODE_CONTROL_ARG_STATION) ? c->fixed : a->action;
        break;
    case IOTDATA_NODE_CONTROL_ARG_CONTENT:
        /* Big-endian u24, like every other multi-byte field here. Sent even when zero: a content
           command with no id is not a command with a default, it is one that will be refused, and
           refusing it at the node keeps that decision in one place. */
        v[n++] = (uint8_t)(a->content >> 16);
        v[n++] = (uint8_t)(a->content >> 8);
        v[n++] = (uint8_t)a->content;
        break;
    case IOTDATA_NODE_CONTROL_ARG_CONTENT_AUDIENCE:
        v[n++] = (uint8_t)(a->content >> 16);
        v[n++] = (uint8_t)(a->content >> 8);
        v[n++] = (uint8_t)a->content;
        /* WHO THE OFFER IS FOR, which is not who the COMMAND is for: `target` drives the node that
           will advertise, and this is the audience it will advertise TO. Station 0 is not a
           station, so an unsaid audience means everybody -- and the substitution is made here
           rather than in each medium, so a console and a JSON request cannot disagree about what
           saying nothing means. */
        {
            const uint16_t audience = (a->station == 0u) ? (uint16_t)IOTDATA_STATION_BROADCAST : a->station;
            v[n++] = (uint8_t)(audience >> 8);
            v[n++] = (uint8_t)audience;
        }
        break;
    case IOTDATA_NODE_CONTROL_ARG_MANIFEST: {
        v[n++] = (uint8_t)(a->content >> 16);
        v[n++] = (uint8_t)(a->content >> 8);
        v[n++] = (uint8_t)a->content;
        v[n++] = a->content_encoding;
        v[n++] = (uint8_t)(a->content_expiry_m >> 8);
        v[n++] = (uint8_t)a->content_expiry_m;
        v[n++] = (uint8_t)(a->content_sig >> 16);
        v[n++] = (uint8_t)(a->content_sig >> 8);
        v[n++] = (uint8_t)a->content_sig;
        /* NUL-TERMINATED, as it is in the MANIFEST TLV itself (§3) and for the same reason: there
           is then no length field to disagree with the value's own, and a lone NUL is no name. */
        for (const char *p3 = a->content_name; *p3 != '\0' && (size_t)n + 1u < sizeof(v); p3++)
            v[n++] = (uint8_t)*p3;
        v[n++] = 0u;
        break;
    }
    case IOTDATA_NODE_CONTROL_ARG_REPORTS:
        /* every report that can be asked for, which is now every system type: a node with nothing
           to say for one answers it empty, and empty is an answer */
        for (uint8_t type = 0; type < IOTDATA_NODE_TLV_SYSTEM_COUNT; type++)
            if (iotdata_node_tlv_is_reportable(type))
                iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_REQUEST, &type, 1);
        return !kv->overflow && kv->len > 0;
    default:
        return false;
    }
    return iotdata_kvr_add(kv, c->key, v, n);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE REPORT
//
// The other direction: not a command being sent to a node, but a node saying which commands it
// ACCEPTS. That answer is how a manager discovers a device instead of being configured with a list
// of what it ought to support, so it has to be true rather than aspirational -- a node that
// advertises a key it does not service sends whoever asked on a wild goose chase.
//
// EVERY node answers the same seven, so they are unconditional: the five reports the node layer
// builds -- version, variant, control, status, config -- plus DIAGNOSTICS and REBOOT.
//
// Those last two are universal by DEFINITION rather than by coincidence. A reboot means restarting
// whatever this node is: an MCU restarts itself, an application restarts its own process, and
// neither reboots the machine it happens to be running on. And a diagnostics request is always
// answerable, because "no records" is an answer -- the same rule VARIANT already follows, where a
// node with no telemetry sends the TLV empty rather than staying silent.
//
// What is left over is genuinely per-device, and it is little: whether this node is part of a mesh
// and therefore keeps the tables, and whatever commands the device adds of its own.
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const uint8_t *actions; /* (subject, action) PAIRS: whatever else this device implements */
    uint8_t actions_count;  /* the number of PAIRS, not of bytes */
} iotdata_node_control_report_t;

static inline int iotdata_node_control_pack(iotdata_kvr_t *const kv, const iotdata_node_control_report_t *const r) {
    if (kv == NULL || r == NULL)
        return -1;
    /* The inventory is literally a list of the messages you could send: each entry is the VALUE of
       a REQUEST or a CONTROL, in the encoding it would take on the wire. That is more informative
       than the old list of key flags, which could say a node took diagnostics commands but never
       which -- and it needs no second format to describe the first. */
    for (uint8_t type = 0; type < IOTDATA_NODE_TLV_SYSTEM_COUNT; type++)
        if (iotdata_node_tlv_is_reportable(type))
            iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_REQUEST, &type, 1);
    static const uint8_t node_actions[] = { IOTDATA_NODE_ACTION_NODE_REBOOT, IOTDATA_NODE_ACTION_NODE_RESET };
    for (size_t i = 0; i < sizeof(node_actions) / sizeof(node_actions[0]); i++)
        iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_CONTROL, (uint8_t[2]){ IOTDATA_NODE_SUBJECT_NODE, node_actions[i] }, (uint8_t)2);
    /* whatever the device adds of its own, in the same (subject, action) shape */
    for (uint8_t i = 0; i < r->actions_count; i++)
        iotdata_kvr_add(kv, IOTDATA_NODE_CONTROL_CONTROL, r->actions + (size_t)i * 2u, 2);
    return kv->overflow ? -1 : (int)kv->len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE WORDS AN ARGUMENT CAN BE
//
// Shared by every medium and by none of them in particular: a scope is spelled "mesh" whether it
// arrived as a JSON member or as a console token. They sit above the adapters, and outside the
// JSON guard, because a node built without JSON still has a console.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* A STATUS scope: "node", "mesh", "node,mesh". The words belong to the TLV they scope, so they are
   defined once in iotdata_node_status.h -- which this header therefore expects to have been
   included first -- and a request typed at a console spells them exactly as the JSON does. */
#define iotdata_node_control_scope_status(s) iotdata_node_status_scope_from_name(s)

static inline uint8_t iotdata_node_control_scope_filter(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "manual") == 0)
            return IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_MANUAL;
        if (strcmp(s, "auto") == 0)
            return IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_AUTO;
    }
    return IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_ALL;
}

static inline uint8_t iotdata_node_control_action(const char *const s) {
    if (s != NULL) {
        if (strcmp(s, "block") == 0)
            return IOTDATA_NODE_CONTROL_NODE_FILTERS_BLOCK;
        if (strcmp(s, "allow") == 0)
            return IOTDATA_NODE_CONTROL_NODE_FILTERS_ALLOW;
    }
    return IOTDATA_NODE_CONTROL_NODE_FILTERS_NONE; /* "none", "remove", absent: no entry */
}

/* strtoul: every medium here parses a station id or a content id, JSON or not. It sits above the
   first of them rather than beside one, because both halves need it now. */
#include <stdlib.h>

/*
 * A STATION HAS TO BE THE WHOLE WORD, which this used to not check: it asked whether the first
 * character was a digit, and strtol stops at the first thing it does not like. So `57d` -- which is
 * exactly how every log line (`%03PRIX16`) and every MQTT topic in this system spells station
 * 0x57D -- parsed as decimal 57, and the command went to a different node without a word of
 * complaint. Typing back what you just read is the natural thing to do, and it silently addressed
 * somebody else.
 *
 * DECIMAL STAYS THE DEFAULT, because changing that would silently redirect every existing script;
 * what is refused now is a word that is neither wholly decimal nor 0x-prefixed hex. The mistake
 * names itself instead of being delivered. A content id is the other way round -- hex by default,
 * see _iotdata_node_control_is_content -- because ids are only ever legible that way.
 */
static inline bool _iotdata_node_control_is_station(const char *const s) {
    if (s == NULL || s[0] == '\0')
        return false;
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    return end != NULL && *end == '\0' && v <= (unsigned long)IOTDATA_STATION_MAX;
}

/* ONE PARSE FOR THE PREDICATE AND THE VALUE, as a content id already has. The call sites used
   strtol with base 0, which also reads a leading zero as OCTAL -- `057` was station 47. */
static inline uint16_t iotdata_node_control_station(const char *const s) {
    if (s == NULL)
        return 0u;
    return (uint16_t)(strtoul(s, NULL, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10) & 0x0FFFu);
}
static inline bool _iotdata_node_control_is_action(const char *const s) {
    return s != NULL && (strcmp(s, "none") == 0 || strcmp(s, "remove") == 0 || strcmp(s, "block") == 0 || strcmp(s, "allow") == 0);
}
static inline bool _iotdata_node_control_is_scope_filter(const char *const s) {
    return s != NULL && (strcmp(s, "all") == 0 || strcmp(s, "manual") == 0 || strcmp(s, "auto") == 0);
}
static inline bool _iotdata_node_control_is_scope_status(const char *const s) {
    return iotdata_node_status_scope_is_name(s);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: A CONSOLE
//
// argv joined with '-', longest match first: `mesh peers dump` and `mesh-peers-dump` are the same
// command, so an operator can type it either way and neither spelling is a second vocabulary. The
// longest match matters because the trailing words may be ARGUMENTS -- `mesh peers update 0x537
// remove` has to resolve to mesh-peers-update with two arguments, not fail looking for a command
// called mesh-peers-update-0x537-remove.
//
// Whatever is left after the name is read positionally: a station, then an action or scope. That
// is looser than JSON's named members, which is the nature of a console; what matters is that both
// end at the same iotdata_node_control_args_t and the same iotdata_node_control_build.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* A content id is typed in hex far more often than in decimal -- §4's allocation is
   (station << 12) | counter, which is only legible as hex -- so a bare `1a2b3c` is read as hex and
   an explicit `0x`/`0` prefix still means what C says it means. */
static inline bool _iotdata_node_control_is_content(const char *const s) {
    if (s == NULL || s[0] == '\0')
        return false;
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 0 : 16);
    return end != NULL && *end == '\0' && v <= 0xFFFFFFuL;
}
static inline uint32_t iotdata_node_control_content(const char *const s) {
    if (s == NULL)
        return 0u;
    return (uint32_t)(strtoul(s, NULL, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 0 : 16) & 0xFFFFFFuL);
}

#define IOTDATA_NODE_CONTROL_ARGV_NAME_MAX 48

/* Read the words after the name, positionally, as the arguments THIS command takes. Returns false
   if they cannot all be consumed -- too many, too few, or a word that is not one of ours -- which
   is what makes the longest-match loop safe: a name that matches but cannot own its remainder is
   not the command that was typed. */
static inline bool _iotdata_node_control_argv_args(const iotdata_node_control_command_t *const c, char **const argv, const int first, const int argc, iotdata_node_control_args_t *const args) {
    const int n = argc - first;
    switch (c->arg) {
    case IOTDATA_NODE_CONTROL_ARG_NONE:
    case IOTDATA_NODE_CONTROL_ARG_FIXED:
    case IOTDATA_NODE_CONTROL_ARG_REPORTS:
        return n == 0;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_node_control_is_scope_status(argv[first]))
            return false;
        args->scope_status = iotdata_node_control_scope_status(argv[first]);
        return true;
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER:
        if (n == 0)
            return true;
        if (n != 1 || !_iotdata_node_control_is_scope_filter(argv[first]))
            return false;
        args->scope_filter = iotdata_node_control_scope_filter(argv[first]);
        return true;
    case IOTDATA_NODE_CONTROL_ARG_CONTENT:
        if (n != 1 || !_iotdata_node_control_is_content(argv[first]))
            return false;
        args->content = iotdata_node_control_content(argv[first]);
        return true;
    case IOTDATA_NODE_CONTROL_ARG_CONTENT_AUDIENCE:
        if (n < 1 || n > 2 || !_iotdata_node_control_is_content(argv[first]))
            return false;
        args->content = iotdata_node_control_content(argv[first]);
        if (n == 2) {
            if (!_iotdata_node_control_is_station(argv[first + 1]))
                return false;
            args->station = iotdata_node_control_station(argv[first + 1]);
        }
        return true; /* no audience typed leaves station 0, which the pack reads as everybody */
    case IOTDATA_NODE_CONTROL_ARG_MANIFEST:
        /* POSITIONAL: <id> <sig-id|self> [name] [expiry] [encoding].
         *
         * THE SIGNATURE IS REQUIRED HERE AND NOWHERE BELOW, which is the whole shape of §3's
         * "mandatory to provide, optional to use". The codec takes any value and the offers table
         * keeps any value -- forgiving, because a manifest arriving off the air may legitimately
         * carry 0 (it IS a signature stream), and refusing one would make this implementation the
         * arbiter of somebody else's. What is NOT forgiving is the place a PERSON states an offer:
         * there, forgetting the stream has to be impossible, so the field has no default and the
         * command does not parse without it.
         *
         * `self` IS HOW YOU SAY ZERO ON PURPOSE -- a signature stream's own manifest names no
         * stream, since hashing the hashes would recurse without end. So the field cannot be
         * omitted, and one of the things it may say is "none, because I am one": a deliberate
         * statement rather than an omission, which is exactly the distinction the requirement
         * exists to force. */
        if (n < 2 || n > 5 || !_iotdata_node_control_is_content(argv[first]))
            return false;
        args->content = iotdata_node_control_content(argv[first]);
        if (strcmp(argv[first + 1], "self") == 0 || strcmp(argv[first + 1], "none") == 0)
            args->content_sig = (uint32_t)IOTDATA_CONTENT_SIG_NONE;
        else if (!_iotdata_node_control_is_content(argv[first + 1]))
            return false;
        else
            args->content_sig = iotdata_node_control_content(argv[first + 1]);
        if (n >= 3)
            (void)snprintf(args->content_name, sizeof(args->content_name), "%s", argv[first + 2]);
        if (n >= 4) {
            if (strcmp(argv[first + 3], "indefinite") == 0)
                args->content_expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE;
            else {
                char *end = NULL;
                const unsigned long m = strtoul(argv[first + 3], &end, 10);
                if (end == argv[first + 3] || end == NULL || *end != '\0' || m > 0xFFFFuL)
                    return false;
                args->content_expiry_m = (uint16_t)m;
            }
        }
        if (n >= 5) {
            char *end = NULL;
            const unsigned long e = strtoul(argv[first + 4], &end, 16); /* two nibbles, so hex */
            if (end == argv[first + 4] || end == NULL || *end != '\0' || e > 0xFFuL)
                return false;
            args->content_encoding = (uint8_t)e;
        }
        return true;
    case IOTDATA_NODE_CONTROL_ARG_STATION:
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION: {
        /* the action is optional and defaults to none, as it does in JSON, so that a station typed
           on its own means the same thing through either medium */
        const int most = (c->arg == IOTDATA_NODE_CONTROL_ARG_STATION_ACTION) ? 2 : 1;
        if (n < 1 || n > most || !_iotdata_node_control_is_station(argv[first]))
            return false;
        args->station = iotdata_node_control_station(argv[first]);
        if (n == 2) {
            if (!_iotdata_node_control_is_action(argv[first + 1]))
                return false;
            args->action = iotdata_node_control_action(argv[first + 1]);
        }
        return true;
    }
    default:
        return false;
    }
}

/* How a console SHOWS a command: the name with spaces for hyphens, and what may follow it. A
   console that writes its own version of this prints one that is subtly wrong the first time a row
   above changes, so both come from the table the parser reads. */
static inline const char *iotdata_node_control_argv_name(const iotdata_node_control_command_t *const c, char *const out, const size_t size) {
    size_t n = 0;
    if (out == NULL || size == 0)
        return "";
    if (c != NULL)
        for (const char *p = c->name; *p != '\0' && n + 1u < size; p++)
            out[n++] = (*p == '-') ? ' ' : *p;
    out[n] = '\0';
    return out;
}

static inline const char *iotdata_node_control_argv_help(const iotdata_node_control_command_t *const c) {
    switch ((c != NULL) ? c->arg : IOTDATA_NODE_CONTROL_ARG_NONE) {
    case IOTDATA_NODE_CONTROL_ARG_STATION:
        return " <station>";
    case IOTDATA_NODE_CONTROL_ARG_STATION_ACTION:
        return " <station> [none|remove|block|allow]";
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_FILTER:
        return " [all|manual|auto]";
    case IOTDATA_NODE_CONTROL_ARG_SCOPE_STATUS:
        return " [node|mesh]";
    case IOTDATA_NODE_CONTROL_ARG_CONTENT:
        return " <id>";
    case IOTDATA_NODE_CONTROL_ARG_CONTENT_AUDIENCE:
        return " <id> [station]";
    case IOTDATA_NODE_CONTROL_ARG_MANIFEST:
        return " <id> <sig-id|self> [name] [minutes|indefinite] [encoding-hex]";
    default:
        return "";
    }
}

static inline const iotdata_node_control_command_t *iotdata_node_control_from_argv(const int argc, char **const argv, iotdata_node_control_args_t *const args, int *const consumed) {
    if (argv == NULL || args == NULL || argc <= 0)
        return NULL;
    *args = (iotdata_node_control_args_t){ 0 };
    args->target = args->targets[0] = IOTDATA_STATION_BROADCAST;
    args->targets_count = 1;
    args->scope_filter = IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_ALL;
    /* An unsaid expiry OFFERS indefinitely rather than revoking, which zero would do. */
    args->content_expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE;

    for (int words = argc; words >= 1; words--) {
        char name[IOTDATA_NODE_CONTROL_ARGV_NAME_MAX];
        size_t n = 0;
        bool fits = true;
        for (int i = 0; i < words && fits; i++) {
            const char *p2 = argv[i];
            if (i > 0 && n + 1u < sizeof(name))
                name[n++] = '-';
            for (; *p2 != '\0'; p2++) {
                if (n + 1u >= sizeof(name)) {
                    fits = false;
                    break;
                }
                name[n++] = *p2;
            }
        }
        if (fits) {
            name[n] = '\0';
            const iotdata_node_control_command_t *const c = iotdata_node_control_find(name);
            if (c != NULL)
                if (_iotdata_node_control_argv_args(c, argv, words, argc, args)) { /* the name matched but the rest cannot belong to it: keep shortening */
                    if (consumed != NULL)
                        *consumed = words;
                    return c;
                }
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// MEDIA: JSON
// -----------------------------------------------------------------------------------------------------------------------------------------

#if !defined(IOTDATA_NO_JSON)

#include <cjson/cJSON.h>

/* A station id, as a decimal or 0x string or a number, masked to the 12 bits the header carries. */
static inline uint16_t _iotdata_node_control_json_station(const cJSON *const j, const uint16_t absent) {
    if (j != NULL) {
        if (cJSON_IsString(j) && j->valuestring != NULL) {
            if (strcmp(j->valuestring, "all") == 0 || strcmp(j->valuestring, "broadcast") == 0)
                return IOTDATA_STATION_BROADCAST;
            return (uint16_t)(strtol(j->valuestring, NULL, 0) & 0x0FFF);
        }
        if (cJSON_IsNumber(j))
            return (uint16_t)(j->valueint & 0x0FFF);
    }
    return absent;
}

static inline const char *_iotdata_node_control_json_str(const cJSON *const root, const char *const name) {
    const cJSON *const j = cJSON_GetObjectItem(root, name);
    return (cJSON_IsString(j) && j->valuestring != NULL) ? j->valuestring : NULL;
}

/* A request object -> the command and its arguments. Returns NULL when the object names no command
   this node knows, which the caller reports rather than guessing at. */
/* One station, or a list of them. Duplicates are dropped and a broadcast anywhere in the list
   collapses it to a single broadcast -- both because sending the same node the same command twice
   is airtime for nothing, and because the alternative is a caller having to reason about whether
   its list overlaps. Returns how many are in `out`, always at least one. */
static inline uint8_t _iotdata_node_control_json_targets(const cJSON *const j, uint16_t *const out, const uint8_t max, bool *const overflow) {
    uint8_t n = 0;
    *overflow = false;
    if (cJSON_IsArray(j)) {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, j) {
            const uint16_t st = _iotdata_node_control_json_station(e, IOTDATA_STATION_BROADCAST);
            if (st == IOTDATA_STATION_BROADCAST) {
                out[0] = IOTDATA_STATION_BROADCAST;
                return 1;
            }
            bool seen = false;
            for (uint8_t i = 0; i < n; i++)
                seen = seen || (out[i] == st);
            if (!seen) {
                if (n < max)
                    out[n++] = st;
                else
                    *overflow = true; /* silently dropping a station the caller named is not an option */
            }
        }
    }
    if (n == 0) { /* not a list, or an empty one: the single-target reading, broadcast when absent */
        out[0] = _iotdata_node_control_json_station(j, IOTDATA_STATION_BROADCAST);
        n = 1;
    }
    return n;
}

static inline const iotdata_node_control_command_t *iotdata_node_control_from_json(const cJSON *const root, iotdata_node_control_args_t *const args) {
    if (root == NULL || args == NULL)
        return NULL;
    /* ZEROED AND DEFAULTED FIRST, exactly as iotdata_node_control_from_argv does it. This used to
       be total by ENUMERATION -- every field was assigned unconditionally, so the caller's struct
       never showed through -- and that held right up until a field arrived that is only set when
       the request mentions it. Then an omitted `name` left whatever the caller's stack held, and
       the two media built different bytes from the same command. Zeroing makes the property
       structural instead of a thing to remember with every field added. */
    *args = (iotdata_node_control_args_t){ 0 };
    args->scope_filter = IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_ALL;
    args->content_expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE;
    /* absent target means BROADCAST -- "this command, to whoever hears it" -- while an absent
       station means none, because a mesh command with no subject has nothing to act on */
    args->targets_count = _iotdata_node_control_json_targets(cJSON_GetObjectItem(root, "target"), args->targets, IOTDATA_NODE_CONTROL_TARGETS_MAX, &args->targets_overflow);
    args->target = args->targets[0];
    args->station = _iotdata_node_control_json_station(cJSON_GetObjectItem(root, "station"), 0);
    const char *const scope = _iotdata_node_control_json_str(root, "scope");
    args->scope_status = iotdata_node_control_scope_status(scope);
    args->scope_filter = iotdata_node_control_scope_filter(scope);
    args->action = iotdata_node_control_action(_iotdata_node_control_json_str(root, "action"));
    /* A content id as a JSON STRING, read the same way the console reads it -- bare hex -- so that
       `{"cmd":"content-delete","content":"1a2b3c"}` and `content delete 1a2b3c` name one file. A
       number is accepted too and is then plainly decimal, which is what JSON means by a number. */
    const cJSON *const jcontent = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsString(jcontent) && jcontent->valuestring != NULL)
        args->content = iotdata_node_control_content(jcontent->valuestring);
    else if (cJSON_IsNumber(jcontent) && jcontent->valuedouble >= 0.0 && jcontent->valuedouble <= 16777215.0)
        args->content = (uint32_t)jcontent->valuedouble;
    /* A MANIFEST's fields BY NAME, which is the medium that wants them that way -- a console types
       them positionally because four optional words in a row is still readable, and a request
       written by a program should not have to count commas. Each keeps the console's default when
       absent, so the two media describe the same offer. */
    const char *const jname = _iotdata_node_control_json_str(root, "name");
    if (jname != NULL)
        (void)snprintf(args->content_name, sizeof(args->content_name), "%s", jname);
    const cJSON *const jexp = cJSON_GetObjectItem(root, "expiry");
    if (cJSON_IsString(jexp) && jexp->valuestring != NULL && strcmp(jexp->valuestring, "indefinite") != 0)
        args->content_expiry_m = (uint16_t)strtoul(jexp->valuestring, NULL, 10);
    else if (cJSON_IsNumber(jexp) && jexp->valuedouble >= 0.0 && jexp->valuedouble <= 65535.0)
        args->content_expiry_m = (uint16_t)jexp->valuedouble;
    const cJSON *const jenc = cJSON_GetObjectItem(root, "encoding");
    if (cJSON_IsString(jenc) && jenc->valuestring != NULL)
        args->content_encoding = (uint8_t)strtoul(jenc->valuestring, NULL, 16);
    else if (cJSON_IsNumber(jenc) && jenc->valuedouble >= 0.0 && jenc->valuedouble <= 255.0)
        args->content_encoding = (uint8_t)jenc->valuedouble;
    const cJSON *const jsig = cJSON_GetObjectItem(root, "sig");
    if (cJSON_IsString(jsig) && jsig->valuestring != NULL)
        args->content_sig = iotdata_node_control_content(jsig->valuestring);
    else if (cJSON_IsNumber(jsig) && jsig->valuedouble >= 0.0 && jsig->valuedouble <= 16777215.0)
        args->content_sig = (uint32_t)jsig->valuedouble;
    return iotdata_node_control_find(_iotdata_node_control_json_str(root, "cmd"));
}

#endif /* !IOTDATA_NO_JSON */

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTROL_H */
