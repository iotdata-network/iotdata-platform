
#ifndef IOTDATA_NODE_RUNTIME_H
#define IOTDATA_NODE_RUNTIME_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_runtime.h - what a node IS and DOES, as against what it says. The library's
// iotdata_node.h defines the system TLVs -- the wire form every node speaks; this holds the running
// state behind them and the operations over it: identity and sequence, the receive window, building
// and sending the reports, and dispatching an arriving frame.
//
// EVERY ROLE USES THIS. A sensor, a simulator's virtual sensors, a relay and a gateway all keep an
// iotdata_node_t and drive it from here. relay_node.h and iotdata_gateway_node.h are built ON this,
// not instead of it -- they add what their role needs and leave the identity, the sequence rule and
// the window to the code below. (This header spent a while describing itself as the end-device
// counterpart to those two. It was not, and the name said platform, which it never was either.)
//
// WHAT THE WINDOW IS FOR is the end-device case, and it shapes the design even though the other
// roles do not sleep. A sensor is asleep almost always, so it cannot be commanded whenever a manager
// feels like it, because there is nothing listening. Instead it advertises: every
// IOTDATA_NODE_RECEIVE_EVERY_S it puts a RECEIVE TLV in its next outbound frame and then holds its
// receiver on for IOTDATA_NODE_RECEIVE_WINDOW_S. Whoever is holding a downstream frame for it --
// gateway or relay, see iotdata_node_down.h -- sends it in that window. An always-on role simply
// never closes its window, and the same code serves both.
//
// THE DEVICE DECIDES. Only it knows its power budget. A node that never advertises is never sent
// to, and that is the default, costing nothing. Everything above is the node choosing to be
// reachable, not something done to it.
//
// REQUIRES A DECODER on any node that accepts commands; one that only ever transmits could build
// with IOTDATA_NO_DECODE.
//
// SPLIT IN TWO, because a simulator is a fleet in one box: iotdata_node_params_t is per application (what
// the firmware is, how to transmit), iotdata_node_t is per station (identity, sequence, where its
// window has got to). A sensor has one node; a simulator has one per virtual sensor, each with its
// own window, so the simulated fleet behaves like a real one rather than all waking together.
//
// NO PERSISTENCE YET. The periods below are compile-time, and the window state lives whereever the
// caller puts iotdata_node_t -- RTC memory on a deep-sleeping sensor, plain RAM on a simulator. When
// config persistence arrives these become settable through CONFIG and this comment goes away.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_RECEIVE_EVERY_S
#define IOTDATA_NODE_RECEIVE_EVERY_S (6u * 60u * 60u) /* 6 hours between windows */
#endif

/* The SAME number the protocol assumes of a RECEIVE that states no duration, by derivation through
   iotdata_node.h rather than by two copies agreeing: a node whose own window disagreed with what
   everyone else assumes of a silent one would be the odd case rather than the normal one. A manager
   can change it over the air -- it is a SETTINGS value -- so this is a default and not a limit. */
#ifndef IOTDATA_NODE_RECEIVE_WINDOW_S
#define IOTDATA_NODE_RECEIVE_WINDOW_S IOTDATA_NODE_RECEIVE_WINDOW_S_DEFAULT
#endif

/* The cadence cannot be derived the same way -- it is this header's and the settings default is
   declared before this file is read -- so it is asserted instead. A node WITH a settings block and
   one without must wake on the same schedule; they disagreed once, and the configured value turned
   out not to be the one anybody had read. Override one of these and the build says so. */
_Static_assert((unsigned)IOTDATA_NODE_SETTINGS_DEFAULT_INTERVAL_M * 60u == (unsigned)IOTDATA_NODE_RECEIVE_EVERY_S, "IOTDATA_NODE_SETTINGS_DEFAULT_INTERVAL_M and IOTDATA_NODE_RECEIVE_EVERY_S must describe the same cadence");

#ifndef IOTDATA_NODE_DIAG_RECORD_MAX
#define IOTDATA_NODE_DIAG_RECORD_MAX 128 /* one diagnostic record: only allocated when a device HAS a recorder */
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* The app adds its own STATUS keys -- battery, temperature, whatever it has. Called with the
   builder mid-flight, so it just appends. */
/* Fill in what is true of this device; iotdata_node_status_pack does the encoding. The app used to
   write keys into the payload itself, which is how three nodes came to emit three subsets of the
   same list in three orders. */
typedef void (*iotdata_node_status_fn)(uint16_t station, iotdata_node_status_t *out);
typedef bool (*iotdata_node_tx_fn)(const uint8_t *packet, size_t len);

/* A CONTROL key this layer does not know, offered to the application: already split into its
   subject and action, so the app never sees the CONTROL key or re-parses the prefix.
   Answers with one of the three in iotdata_node_control.h -- UNKNOWN (not mine, counted unknown
   and skipped rather than failing the frame), DONE, or REFUSED with an optional `*reason` for the
   console and the log. See WHAT HAPPENED TO IT there for why a bool was not enough. `reason` is
   never NULL; leaving it alone is fine. */
typedef iotdata_node_control_result_t (*iotdata_node_control_fn)(uint16_t station, uint8_t subject, uint8_t action, const uint8_t *args, uint8_t arglen, const char **reason);

/* One diagnostic record per call, from `cursor` (start it at 0), returning its length and 0 when
   there are no more -- the same shape the relay's node layer uses, so a device that gains a
   recorder plugs it in the same way wherever it runs. */
typedef size_t (*iotdata_node_diag_fn)(size_t *cursor, char *out, size_t outsize);

/* The device's own CONFIG. Reached through callbacks rather than by naming iotdata_node_config_table,
   because the table is compiled into the APPLICATION and this header is included before the X-macro
   that expands it -- the same reason the variant and control seams are function pointers.
   Build packs the table (resuming from `partial`); apply takes a record stream that arrived over the
   air and returns whether it committed, setting `reboot_out` when a written row demands one. */
typedef int (*iotdata_node_config_build_fn)(uint8_t *buf, size_t size, iotdata_node_partial_t *partial);
typedef bool (*iotdata_node_config_apply_fn)(const uint8_t *buf, size_t len, bool *reboot_out);

typedef struct {
    /* What this instance HAS -- the rest of VERSION (chip, IDF, stamp, eFuse serial) is detected
       by iotdata_node_version.h and needs nothing from the app. May be NULL. */
    const iotdata_node_version_caps_t *caps;
    /* Fills iotdata_node_status_t. A plain end device leaves .mesh.present false -- it is in
       nobody's mesh and must OMIT the group rather than report a zeroed one. A node that both
       senses and relays, which is coming, fills the group in and answers the mesh scope as a
       relay does, without needing a second callback to do it. */
    iotdata_node_status_fn status;
    iotdata_node_tx_fn tx;
    /* Optional: the same app-control seam the relay and the gateway have. `control_actions` is what
       it implements, so the CONTROL report can advertise it -- this layer cannot know, and a
       hardcoded list here would go stale the first time the app changed. */
    iotdata_node_control_fn control;
    /* The protocol's own settings, and where they persist. Both or neither: a settings block that
       cannot be flushed would answer a write with a promise rather than a fact. */
    iotdata_node_settings_t *settings;
    iotdata_node_state_t *state;
    /* The device's own configuration. BOTH OR NEITHER, for the settings pair's reason exactly: a
       table that can be read but not written would answer a write by ignoring it, and a table that
       can be written but not read could not answer with what is now true. A node with no config
       leaves both NULL and answers a CONFIG request with an empty report, which is an answer. */
    iotdata_node_config_build_fn config_build;
    iotdata_node_config_apply_fn config_apply;
    /* Optional: just the rows the last write named (iotdata_node_config_pack_written), which is what
       a write is answered with. Absent, the answer is the whole table, as it always was. */
    int (*config_build_written)(uint8_t *buf, size_t size);

    const uint8_t *control_actions;
    uint8_t control_actions_count;
    /* Optional: a recorder. Most end devices have none, and then a diagnostics request is still
       ANSWERED -- see iotdata_node_build_diagnostics. */
    iotdata_node_diag_fn diag;
    /*
     * The receiver is NEVER off: a simulator on the bench, or any mains-powered end device.
     *
     * Such a node does not advertise at all, and the other two fields are ignored. A DOWN frame is
     * always TRANSMITTED first and only then held (see iotdata_node_down.h), so a node that is always
     * listening hears the immediate transmission -- the advertisement exists purely to tell a
     * holder when a SLEEPING node is briefly awake, and there is nothing to tell about a node that
     * never sleeps. The held copy simply goes unused and expires.
     *
     * The one thing given up is the retry path: if that immediate transmission is lost on the air,
     * nothing prompts the holder to try again, so the command has to be reissued. For a bench
     * instrument that is the right trade; a field node that wants the retry should advertise
     * occasionally instead, which is what receive_every_s is for.
     */
    bool receive_always;
    uint32_t receive_every_s;  /* 0 = never open a window: this node cannot be reached */
    uint16_t receive_window_s; /* advertised, so a sender can skip one too short to use */
    uint16_t packet_max;
} iotdata_node_params_t;

/* Per station. On a deep-sleeping sensor this must live in RTC memory to survive the sleep. */
typedef struct {
    uint16_t station;
    uint16_t sequence; /* our own, for frames we originate */
    /* Where `sequence` persists. NULL is fine and simply means it does not. The station id
       persists too, in the SETTINGS block -- see iotdata_node_settings_station(). */
    iotdata_node_state_t *state;
    uint32_t elapsed_ms;    /* since the last window closed */
    uint32_t window_end_ms; /* meaningful only while window_open */
    bool window_open;
    bool window_pending; /* due, and to be advertised in the next frame we send */
    uint32_t stat_rx, stat_requests, stat_reports, stat_commands, stat_unknown, stat_windows;
    /* Commands this node owns and declined on a nameable condition -- apart from both of the
       above, for the reason iotdata_node_control.h gives. `refusal` is the last one's words, for
       a console standing right here; it points at a literal and is never freed. */
    uint32_t stat_refused;
    const char *refusal;
    /* The config this node runs against, so that every iotdata_node_* call takes the NODE and nothing else.
       NOT persistent state, even though it lives in a struct that is: it points at function
       pointers and at the running state, so iotdata_node_attach() re-sets it on every boot and it is
       never trusted across one. */
    const iotdata_node_params_t *cfg;
    iotdata_encoder_t _encoder;
    iotdata_decoder_t _decoder;
    uint8_t _packet[IOTDATA_MAX_PACKET_SIZE];
    uint8_t _kvbuf[IOTDATA_TLV_VALUE_MAX];
} iotdata_node_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

/* REQUIRED, ON EVERY BOOT, before anything else touches the node. Separate from bind() because the
   two are not equally optional -- `state` may be NULL and an unpersisted node still works, whereas
   a node with no config cannot answer a request, build a report or decide a window. Separate from
   init() because init runs only on a COLD boot: the struct lives in RTC memory and survives deep
   sleep, so a sleeping node would otherwise keep the pointer the PREVIOUS boot left, which reads
   as correct right up until the binary underneath it changes. */
static inline void iotdata_node_attach(iotdata_node_t *const n, const iotdata_node_params_t *const cfg) {
    n->cfg = cfg;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const iotdata_node_params_t *iotdata_node_params(const iotdata_node_t *const n) {
    return n->cfg;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint16_t iotdata_node_window_s(const iotdata_node_t *const n) {
    const iotdata_node_params_t *const cfg = n->cfg;
    return cfg->settings ? iotdata_node_settings_window_s(cfg->settings) : cfg->receive_window_s;
}

static inline uint32_t iotdata_node_window_every_s(const iotdata_node_t *const n) {
    const iotdata_node_params_t *const cfg = n->cfg;
    return cfg->settings ? iotdata_node_settings_interval_s(cfg->settings) : cfg->receive_every_s;
}

static inline uint32_t iotdata_node_window_ms(const iotdata_node_t *const n) {
    return (uint32_t)iotdata_node_window_s(n) * 1000u; /* u16 seconds is 18.2h at the top, nowhere near a wrap */
}

/* Saturating, where the window above cannot overflow at all: the settings interval is capped at a
   week, but cfg->receive_every_s is whatever the application put there and a uint32 of milliseconds
   runs out at 49.7 days. A cadence that long and one of 0xFFFFFFFF ms are the same answer in the
   field; what matters is that an absurd one cannot wrap round into a frantic one. 0 is still 0,
   and still means never. */
static inline uint32_t iotdata_node_window_every_ms(const iotdata_node_t *const n) {
    const uint32_t every_s = iotdata_node_window_every_s(n);
    return (every_s > 0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : every_s * 1000u;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline size_t iotdata_node_packet_budget(const iotdata_node_t *const n) {
    const iotdata_node_params_t *const cfg = n->cfg;
    const size_t want = (cfg->packet_max != 0u) ? (size_t)cfg->packet_max : (size_t)IOTDATA_MAX_PACKET_SIZE;
    return want < (size_t)IOTDATA_MAX_PACKET_SIZE ? want : (size_t)IOTDATA_MAX_PACKET_SIZE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * ONE STATION, ONE SEQUENCE, and this is where it lives.
 *
 * (station, sequence) is the protocol's whole identity for a packet, so a station that both relays
 * and reports still has a SINGLE stream: two counters under one id give a receiver two interleaved
 * sequences, and then dedup can drop a good frame because the other stream already used that
 * number, while counting gaps to measure loss becomes meaningless. Every module that emits a frame
 * as us takes a pointer to this and none keeps a counter of its own.
 *
 * `state` may be NULL -- an unpersisted node still counts, it just starts from 0 after a reboot.
 */
/* Registration, split out from init because the two happen at DIFFERENT times on a sleeping node.
   The struct lives in RTC memory and must survive a deep sleep, so init -- which memsets -- runs
   only on a cold boot; but the store has to be told about the block on EVERY boot or it has
   nothing to flush or load. An always-on node calls init alone and never notices the difference. */
static inline bool iotdata_node_bind(iotdata_node_t *const n, iotdata_node_state_t *const state, const uint32_t tag) {
    n->state = state;
    return iotdata_node_state_insert(state, tag, 1u, &n->sequence, sizeof(n->sequence), NULL);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_init(iotdata_node_t *const n, const uint16_t station, iotdata_node_state_t *const state, const uint32_t tag) {
    *n = (iotdata_node_t){ .station = station };
    (void)iotdata_node_bind(n, state, tag);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint16_t iotdata_node_station(const iotdata_node_t *const n) {
    return n->station;
}

static inline bool iotdata_node_station_restore(iotdata_node_t *const n, const iotdata_node_settings_t *const s) {
    if (!n || !s)
        return false;
    const uint16_t station = iotdata_node_settings_station(s);
    if (station == n->station || station == 0u || station >= IOTDATA_STATION_MAX)
        return false;
    n->station = station;
    return true;
}

static inline bool iotdata_node_station_adopt(iotdata_node_t *const n, const uint16_t station) {
    if (!n || station == n->station || station == 0u || station >= IOTDATA_STATION_MAX)
        return false;
    n->station = station;
    n->sequence = 0;
    if (n->state)
        (void)iotdata_node_state_flush(n->state);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint16_t iotdata_node_sequence(const iotdata_node_t *const n) {
    return n->sequence;
}

static inline void iotdata_node_sequence_used(iotdata_node_t *const n) {
    n->sequence = iotdata_sequence_next(n->sequence);
    iotdata_node_state_touch(n->state);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Account for time passing. Returns true when a window has become due, at which point the next
   frame this node sends should carry a RECEIVE TLV. */
static inline bool iotdata_node_window_advance(iotdata_node_t *const n, const uint32_t delta_ms) {
    const iotdata_node_params_t *const cfg = n->cfg;
    if (cfg->receive_always)
        return false;
    const uint32_t every_ms = iotdata_node_window_every_ms(n);
    if (every_ms == 0)
        return false;
    if (n->window_pending)
        return true; /* already due and not yet advertised */
    n->elapsed_ms += delta_ms;
    if (n->elapsed_ms < every_ms)
        return false;
    n->window_pending = true;
    return true;
}

static inline bool iotdata_node_window_pending(const iotdata_node_t *const n) {
    return n->window_pending;
}

static inline void iotdata_node_window_begin(iotdata_node_t *const n, const uint32_t now_ms) {
    n->window_pending = false;
    n->window_open = true;
    n->window_end_ms = now_ms + iotdata_node_window_ms(n);
    n->elapsed_ms = 0;
    n->stat_windows++;
}

static inline bool iotdata_node_window_active(const iotdata_node_t *const n, const uint32_t now_ms) {
    const iotdata_node_params_t *const cfg = n->cfg;
    return cfg->receive_always || (n->window_open && (int32_t)(n->window_end_ms - now_ms) > 0);
}

static inline void iotdata_node_window_end(iotdata_node_t *const n) {
    n->window_open = false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// Builders. Same contract as the relay and gateway: the length, or -1 if it could not be built.
// Length 0 is a valid answer, so callers test for -1 and never for 0.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_add_str(iotdata_kvr_t *const kv, const uint8_t key, const char *const s) {
    if (s && *s != '\0')
        iotdata_kvr_add_str(kv, key, s);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_receive_append(const iotdata_node_t *const n, iotdata_encoder_t *const enc) {
    const iotdata_node_params_t *const cfg = n->cfg;
    uint8_t kv[16]; /* duration 4 + types 6 + delay 6: all three fit, so the advert cannot truncate */
    /* 0 = unstated = "listening, for anything", which is the whole truth for an always-on node.
     *
     * NO DELAY EITHER, and that is a decision rather than an omission: this node advertises when its
     * window is open or about to be, so the countdown is zero. A node that wanted to state a future
     * window -- transmit at noon, listen in the evening -- would pass it here, but it must not until
     * the holder side acts on it (iotdata_node_down.h holds such a window rather than serving it). */
    const size_t kvlen = iotdata_node_receive_build(kv, sizeof(kv), cfg->receive_always ? 0u : iotdata_node_window_s(n), 0, 0);
    return iotdata_encode_tlv(enc, IOTDATA_NODE_TLV_RECEIVE, kv, (uint8_t)kvlen) == IOTDATA_OK;
}

static inline bool iotdata_node_receive_announce(iotdata_node_t *const n) {
    const iotdata_node_params_t *const cfg = n->cfg;
    if (!cfg->tx)
        return false;
    size_t len = 0;
    if (iotdata_encode_begin(&n->_encoder, n->_packet, iotdata_node_packet_budget(n), 0, n->station, n->sequence) != IOTDATA_OK)
        return false;
    if (!iotdata_node_receive_append(n, &n->_encoder))
        return false;
    if (iotdata_encode_end(&n->_encoder, &len) != IOTDATA_OK)
        return false;
    iotdata_node_sequence_used(n);
    return cfg->tx(n->_packet, len);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_version(const iotdata_node_t *const n, uint8_t *const buf, const size_t size) {
    const iotdata_node_params_t *const cfg = n->cfg;
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    return iotdata_node_version_pack(&kv, &(const iotdata_node_version_t){ .caps = cfg->caps });
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_variant(uint8_t *const buf, const size_t size, const uint8_t want, iotdata_node_partial_t *const p) {
    return iotdata_node_variant_pack(buf, size, want, p);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_status(const iotdata_node_t *const n, uint8_t *const buf, const size_t size, const uint8_t scope) {
    const iotdata_node_params_t *const cfg = n->cfg;
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    iotdata_node_status_t s = { 0 };
    if (cfg->status)
        cfg->status(n->station, &s); /* the app knows its own uptime, battery, heap */
    s.has_image = iotdata_node_firmware_image_status(&s.image);
    return iotdata_node_status_pack(&kv, &s, scope);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* CONFIG's scope, as STATUS has one: 0 is the whole table -- what a request is answered with -- and
   WRITTEN is the rows a write named, which is what the write is answered with. */
#define IOTDATA_NODE_CONFIG_SCOPE_WRITTEN 1u

static inline int iotdata_node_build_config(const iotdata_node_t *const n, uint8_t *const buf, const size_t size, const uint8_t scope, iotdata_node_partial_t *const partial) {
    const iotdata_node_params_t *const cfg = n->cfg;
    if (scope == IOTDATA_NODE_CONFIG_SCOPE_WRITTEN && cfg->config_build_written)
        return cfg->config_build_written(buf, size);
    if (cfg->config_build)
        return cfg->config_build(buf, size, partial);
    return 0;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_control(const iotdata_node_t *const n, uint8_t *const buf, const size_t size) {
    const iotdata_node_params_t *const cfg = n->cfg;
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    /* an end device keeps no mesh tables */
    return iotdata_node_control_pack(&kv, &(const iotdata_node_control_report_t){ .actions = cfg->control_actions, .actions_count = cfg->control_actions_count });
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_diagnostics(const iotdata_node_t *const n, uint8_t *const buf, const size_t size, iotdata_node_partial_t *const p) {
    const iotdata_node_params_t *const cfg = n->cfg;
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    uint8_t packed = 0;
    bool more = false;
    if (cfg->diag) {
        static char rec[IOTDATA_NODE_DIAG_RECORD_MAX]; // XXX
        size_t cursor = 0, reclen;
        iotdata_kvr_add_u8(&kv, IOTDATA_NODE_DIAGNOSTICS_TYPE, IOTDATA_NODE_DIAG_BLACKBOX);
        while ((reclen = cfg->diag(&cursor, rec, sizeof(rec))) > 0) {
            if (reclen > 255u || kv.len + 2u + reclen > size) {
                more = true; /* what does not fit waits for the next request */
                break;
            }
            iotdata_kvr_add(&kv, IOTDATA_NODE_DIAGNOSTICS_DATA, rec, (uint8_t)reclen);
            packed++;
        }
    }
    if (p) {
        p->chunk = packed;
        p->more = more;
        p->total = (uint8_t)(p->index + packed + (more ? 1u : 0u));
    }
    return kv.overflow ? -1 : (int)kv.len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build_settings(const iotdata_node_t *const n, uint8_t *const buf, const size_t size, const uint8_t *const sel, const uint8_t sellen) {
    const iotdata_node_params_t *const cfg = n->cfg;
    iotdata_kvr_t kv;
    iotdata_kvr_init(&kv, buf, size);
    if (cfg->settings)
        return iotdata_node_settings_pack(&kv, cfg->settings, sel, sellen);
    return kv.overflow ? -1 : (int)kv.len;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_build(const iotdata_node_t *const n, const uint8_t type, uint8_t *const buf, const size_t size, const uint8_t scope, iotdata_node_partial_t *const partial) {
    switch (type) {
    /*IOTDATA_NODE_TLV_RECEIVE*/
    case IOTDATA_NODE_TLV_VERSION:
        return iotdata_node_build_version(n, buf, size);
    case IOTDATA_NODE_TLV_VARIANT:
        return iotdata_node_build_variant(buf, size, scope, partial);
    case IOTDATA_NODE_TLV_STATUS:
        return iotdata_node_build_status(n, buf, size, scope);
    case IOTDATA_NODE_TLV_CONFIG:
        return iotdata_node_build_config(n, buf, size, scope, partial);
    case IOTDATA_NODE_TLV_CONTROL:
        return iotdata_node_build_control(n, buf, size);
    case IOTDATA_NODE_TLV_DIAGNOSTICS:
        return iotdata_node_build_diagnostics(n, buf, size, partial);
    /*IOTDATA_NODE_TLV_DISCRIMINATOR*/
    /*IOTDATA_NODE_TLV_CONTENT*/
    case IOTDATA_NODE_TLV_SETTINGS:
        return iotdata_node_build_settings(n, buf, size, scope ? &scope : NULL, scope ? 1u : 0u);
    default:
        return -1;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Reply with one system TLV, as an ordinary frame from us. A report says who SENT it, so there is
   no addressing to do -- whoever is listening republishes it. */
/* `scope` only means anything to STATUS; 0 is "every group", which is what every caller that is
   not answering an explicit request wants. */
/* One chunk of a report. `partial` NULL is the old behaviour exactly; non-NULL carries the walk
   across calls and comes back with `more` set while records remain.
 *
 * The CALLER decides when to send the next one. Looping in here would put a whole report on the
 * air back-to-back, and at 2.4kbps against a 180ms inter-frame floor that is a node colliding with
 * itself -- so a paged report goes out on the application's own transmit cadence, one chunk a
 * cycle. */
static inline bool iotdata_node_report_paged(iotdata_node_t *const n, const uint8_t type, const uint8_t scope, iotdata_node_partial_t *const partial) {
    const iotdata_node_params_t *const cfg = n->cfg;
    if (!cfg->tx)
        return false;
    iotdata_node_partial_begin(partial, n->sequence);
    const size_t budget = iotdata_node_packet_budget(n);
    const int kvlen = iotdata_node_build(n, type, n->_kvbuf, budget < sizeof(n->_kvbuf) ? budget : sizeof(n->_kvbuf), scope, partial);
    if (kvlen < 0)
        return false;
    size_t len = 0;
    if (iotdata_encode_begin(&n->_encoder, n->_packet, budget, 0, n->station, n->sequence) != IOTDATA_OK)
        return false;
    if (iotdata_node_partial_needed(partial) && !iotdata_node_partial_emit(&n->_encoder, partial))
        return false;
    if (iotdata_encode_tlv(&n->_encoder, type, n->_kvbuf, (uint8_t)kvlen) != IOTDATA_OK)
        return false;
    if (iotdata_encode_end(&n->_encoder, &len) != IOTDATA_OK)
        return false;
    n->stat_reports++;
    if (cfg->tx(n->_packet, len)) {
        iotdata_node_sequence_used(n);      /* only now: the rule is that a refused frame consumes nothing */
        iotdata_node_partial_sent(partial); /* only now: a refused frame must not skip its records */
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_report_scoped(iotdata_node_t *const n, const uint8_t type, const uint8_t scope) {
    return iotdata_node_report_paged(n, type, scope, &(iotdata_node_partial_t){ 0 }); /* one chunk; the rest needs a caller that loops */
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool iotdata_node_report(iotdata_node_t *const n, const uint8_t type) {
    return iotdata_node_report_scoped(n, type, 0); /* 0 = every group */
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_process_config(iotdata_node_t *const n, const uint8_t *const kv, const size_t kvlen, bool *const reboot_out) {
    const iotdata_node_params_t *const cfg = n->cfg;
    n->stat_commands++;
    if (cfg->config_apply)
        (void)cfg->config_apply(kv, kvlen, reboot_out);
    (void)iotdata_node_report_scoped(n, IOTDATA_NODE_TLV_CONFIG, IOTDATA_NODE_CONFIG_SCOPE_WRITTEN); /* what it named, not the table */
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_process_control(iotdata_node_t *const n, const uint8_t *const kv, const size_t kvlen, bool *const reboot_out) {
    const iotdata_node_params_t *const cfg = n->cfg;
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(kv, kvlen, &cur, &key, &val, &vlen)) {
        /* REQUEST: [subject][selection...]. A subject that is a TLV type is answered by a report of
           that type; STATUS reads the selection as a scope, and every other type answers in full. */
        if (key == IOTDATA_NODE_CONTROL_REQUEST && vlen >= 1) {
            const uint8_t subject = val[0];
            /* counted a request only once it is one we can answer: NODE and MESH have nothing to
               report, and a subject we do not implement is unknown rather than serviced */
            if (iotdata_node_tlv_is_reportable(subject)) {
                n->stat_requests++;
                (void)iotdata_node_report_scoped(n, subject, (subject == IOTDATA_NODE_TLV_STATUS && vlen >= 2) ? val[1] : 0u);
            } else
                n->stat_unknown++;
        } else if (key == IOTDATA_NODE_CONTROL_CONTROL && vlen >= 2) { /* CONTROL: [subject][action][args...] */
            const uint8_t subject = val[0], action = val[1];
            const uint8_t *const args = (vlen > 2) ? val + 2 : NULL, arglen = (uint8_t)(vlen - 2);
            if (subject == IOTDATA_NODE_SUBJECT_NODE && action == IOTDATA_NODE_ACTION_NODE_REBOOT) {
                n->stat_commands++;
                if (reboot_out)
                    *reboot_out = true;
            } else if (subject == IOTDATA_NODE_SUBJECT_NODE && action == IOTDATA_NODE_ACTION_NODE_ROLLBACK) {
                if (iotdata_node_firmware_rollback_possible())
                    (void)iotdata_node_firmware_rollback(); /* does not return when it works */
                n->stat_refused++;
                n->refusal = "no fallback image to roll back to";
            } else {
                /* not ours: offer it to the app before giving up. A mesh command reaching a node with
                no mesh lands here and is correctly counted unknown. */
                const char *reason = NULL;
                switch (cfg->control ? cfg->control(n->station, subject, action, args, arglen, &reason) : IOTDATA_NODE_CONTROL_RESULT_UNKNOWN) {
                case IOTDATA_NODE_CONTROL_RESULT_DONE:
                    n->stat_commands++;
                    break;
                case IOTDATA_NODE_CONTROL_RESULT_REFUSED:
                    n->stat_refused++;
                    n->refusal = reason ? reason : "refused, without saying why";
                    break;
                default:
                    n->stat_unknown++; /* skipped, not fatal: an older node meeting a newer manager */
                    break;
                }
            }
        } else
            n->stat_unknown++; /* a proprietary key, or one from a newer manager: skipped, not fatal */
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void iotdata_node_process_settings(iotdata_node_t *const n, const uint8_t *const kv, const size_t kvlen, __attribute__((unused)) bool *const reboot_out) {
    const iotdata_node_params_t *const cfg = n->cfg;
    n->stat_commands++;
    if (cfg->settings && iotdata_node_settings_apply(cfg->settings, kv, kvlen)) {
        (void)iotdata_node_settings_commit(cfg->settings, cfg->state);
        (void)iotdata_node_station_adopt(n, iotdata_node_settings_station(cfg->settings));
    }
    (void)iotdata_node_report_scoped(n, IOTDATA_NODE_TLV_SETTINGS, 0);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* A frame arrived while our window was open. Returns true if it was a downstream CONTROL for us and we acted on it. */
static inline bool iotdata_node_on_frame(iotdata_node_t *const n, const uint8_t *const buf, const size_t len, bool *const reboot_out) {
    if (len < IOTDATA_MIN_PACKET_SIZE)
        return false;

    const uint8_t variant = (uint8_t)((buf[0] >> 4) & 0x0F);
    const uint16_t station = (uint16_t)(((uint16_t)(buf[0] & 0x0F) << 8) | buf[1]);
    const uint16_t sequence = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);

    if (variant == IOTDATA_VARIANT_MESH || !iotdata_node_is_down(sequence))
        return false;
    if (!iotdata_node_addressed_to(station, n->station))
        return false;
    if (iotdata_decode(buf, len, &n->_decoder) != IOTDATA_OK || n->_decoder.tlv_count == 0)
        return false;
    n->stat_rx++;

    bool acted = false;
    for (uint8_t i = 0; i < n->_decoder.tlv_count; i++) {
        const iotdata_decoder_tlv_t *const t = &n->_decoder.tlv[i];
        if (t->format == IOTDATA_TLV_FMT_RAW && t->type == IOTDATA_NODE_TLV_CONFIG) {
            iotdata_node_process_config(n, t->raw, t->length, reboot_out);
            acted = true;
        } else if (t->format == IOTDATA_TLV_FMT_RAW && t->type == IOTDATA_NODE_TLV_CONTROL) {
            iotdata_node_process_control(n, t->raw, t->length, reboot_out);
            acted = true;
        } else if (t->format == IOTDATA_TLV_FMT_RAW && t->type == IOTDATA_NODE_TLV_SETTINGS) {
            iotdata_node_process_settings(n, t->raw, t->length, reboot_out);
            acted = true;
        } else
            n->stat_unknown++;
    }
    return acted;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_RUNTIME_H */
