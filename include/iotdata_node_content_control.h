#ifndef IOTDATA_NODE_CONTENT_CONTROL_H
#define IOTDATA_NODE_CONTENT_CONTROL_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_content_control.h - the store's CONTROL words: list, delete, inspect, validate.
//
// WHY THIS IS NOT IN iotdata_node_content.h, which is where iotdata_node_diagnostics.h keeps its
// equivalent. The store module is deliberately free of the protocol: it builds and is tested on a
// host against a RAM backend with nothing but a checksum underneath it, and dragging the action ids
// in would make the one piece of this that can be exercised without a device depend on the library.
// So the handler sits one layer out, where it can see both, and the module stays testable.
//
// REQUIRES iotdata_node_content_manifest.h as well as the store, because half the vocabulary here
// is now the OFFERS half -- authoring one, listing them, putting one on the air, withdrawing one.
// The includer names both, in that order, as it does for every other pair in this layer.
//
// WHAT IS HERE AND WHAT IS IN iotdata_node_console_content.h is the split the recorder already
// makes: these four have a WIRE equivalent and therefore cannot be written twice, so they live in
// iotdata_node_control.h's table and are reached by a console and by MQTT through the same handler.
// `insert` and `download` have no wire equivalent in this shape -- both are a byte stream, and the
// wire's answer to a byte stream is the CONTENT TLVs rather than a command with a vast argument --
// so they are console-only and live over there.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// INSPECT KNOWS ABOUT OTA, ON PURPOSE, AND IS NOT THE OTA CONSUMER
//
// `content inspect` parses an OTA header if the file starts with one and reports what the checklist
// in iotdata_node_content_ota.h makes of it. That is a READ: it claims nothing, owns nothing,
// writes nothing and reboots nothing. It exists so that a bundle can be put on a device and
// questioned before anything is built that would act on it -- which is exactly how the faux-header
// bring-up is done -- and it stays useful afterwards as the way to ask a device what it thinks of
// a blob without asking it to take it.
//
// The OTA CONSUMER is a different thing entirely: it claims the file, decides when to apply it,
// writes a slot and reboots. When it arrives it will describe() its own files, and this reports
// that description as well -- so the two answers sit side by side rather than one replacing the
// other.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// VALIDATE COMPARES AGAINST NOTHING, AND SAYS SO
//
// It reads the file back out of the store and reports §7's CRC-64 over what it found. It does not
// say "good" or "bad", because there is nothing yet to be good or bad against: the gross hash lives
// in §3's MANIFEST, and there is no manifest until the wire path brings one. Until then the number
// is for the operator to compare with what they sent, which they have and the node does not.
//
// That is worth having on its own. The insert's checksum proves the TRANSFER; this proves the
// STORE, some time later, which is a different claim and the one that catches a decayed page or a
// factory-flashed file that never had a transfer behind it at all.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <inttypes.h>

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX
#define IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX 160
#endif

/* How much of a file is read to look for a header in it. The header is bounded by the kvr keys the
   bundler writes -- app, semver, stamp, three hashes, the sizes -- and 256 covers every one of
   them with room over; a parse that wants more than this is looking at something that is not one. */
#ifndef IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES
#define IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES 256u
#endif

// -----------------------------------------------------------------------------------------------------------------------------------------

/* (subject, action) PAIRS, the shape the CONTROL inventory emits and the shape a command takes on
   the wire, so advertising one and sending one cannot drift apart. */
/* As a macro so it can be SPLICED beside another module's -- see the long note by
   IOTDATA_NODE_DIAGNOSTICS_CONTROL_ACTION_PAIRS, which is where the argument for this lives. The
   trailing comma is what lets two of these sit next to each other in one initialiser. */
#define IOTDATA_NODE_CONTENT_CONTROL_ACTION_PAIRS \
    IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_LIST, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_DELETE, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_INSPECT, IOTDATA_NODE_TLV_CONTENT, \
        IOTDATA_NODE_ACTION_CONTENT_VALIDATE, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_STATUS, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_OTA_APPLY, IOTDATA_NODE_TLV_CONTENT, \
        IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST, IOTDATA_NODE_TLV_CONTENT, IOTDATA_NODE_ACTION_CONTENT_ADVERTISE, IOTDATA_NODE_TLV_CONTENT, \
        IOTDATA_NODE_ACTION_CONTENT_REVOKE,

static const uint8_t iotdata_node_content_control_actions[] = { IOTDATA_NODE_CONTENT_CONTROL_ACTION_PAIRS };
#define IOTDATA_NODE_CONTENT_CONTROL_ACTIONS_COUNT ((uint8_t)(sizeof(iotdata_node_content_control_actions) / (2 * sizeof(iotdata_node_content_control_actions[0]))))

// -----------------------------------------------------------------------------------------------------------------------------------------

static iotdata_node_content_t *_iotdata_node_content_control_store = NULL;
static const iotdata_node_ota_device_t *_iotdata_node_content_control_device = NULL;
static char _iotdata_node_content_control_line[IOTDATA_NODE_CONTENT_CONTROL_LINE_MAX];

/* `device` is what this build knows about itself, and NULL is a legitimate answer: inspect then
   parses the header and declines to judge it, rather than judging it against zeroes and calling a
   perfectly good bundle unacceptable. */
static inline void iotdata_node_content_control_init(iotdata_node_content_t *const c, const iotdata_node_ota_device_t *const device) {
    _iotdata_node_content_control_store = c;
    _iotdata_node_content_control_device = device;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE OFFERS, AND HOW A FRAME GETS OUT
//
// A SECOND init RATHER THAN A WIDER FIRST ONE, because the two name different things and a build
// may legitimately have one without the other: every node with a store answers `content list`,
// while only a node that can ORIGINATE a down frame can advertise. A relay today cannot -- it
// originates upstream and mesh frames and nothing else -- so it passes NULL and `content
// advertise` answers honestly instead of being a verb that exists on some nodes and is "unknown
// command" on others. That is the same honesty the firmware consumer's absence gets.
//
// THE CALLBACK IS THE APPLICATION'S DOWN PATH, which this layer must not reimplement: a frame has
// to be hashed into the node's own hold table and flooded, and only the application has the pool,
// the radio and the table. The gateway's node_send_down() is this signature already.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* Put one TLV on the air as a DOWN frame addressed to `audience`. true if it went. */
typedef bool (*iotdata_node_content_originate_fn)(void *ctx, uint16_t audience, const uint8_t *tlv, size_t len);

static iotdata_node_content_manifests_t *_iotdata_node_content_control_offers = NULL;
static iotdata_node_content_originate_fn _iotdata_node_content_control_originate = NULL;
static void *_iotdata_node_content_control_originate_ctx = NULL;

static inline void iotdata_node_content_control_offers_init(iotdata_node_content_manifests_t *const t, const iotdata_node_content_originate_fn originate, void *const ctx) {
    _iotdata_node_content_control_offers = t;
    _iotdata_node_content_control_originate = originate;
    _iotdata_node_content_control_originate_ctx = ctx;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

#define _IOTDATA_NODE_CONTENT_CONTROL_SAY(...) \
    do { \
        _iotdata_node_content_say(snprintf_inline(_iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), __VA_ARGS__)); \
    } while (0)

/* A u24 id, big-endian, exactly as iotdata_node_control_build writes it. Three bytes and not four
   because §4's id is 24 bits: a four-byte field would have a quarter of itself permanently zero
   and would invite someone to put something in it. */
static inline bool _iotdata_node_content_control_id(const uint8_t *const args, const uint8_t arglen, uint32_t *const id) {
    if (args == NULL || arglen < 3u)
        return false;
    *id = ((uint32_t)args[0] << 16) | ((uint32_t)args[1] << 8) | (uint32_t)args[2];
    return true;
}

/* { u24 id, u16 audience }. The audience is who the OFFER is for and not who the command was for;
   a command that names none is read as broadcast by whoever built it, so by the time it is here the
   field always says something. */
static inline bool _iotdata_node_content_control_audience(const uint8_t *const args, const uint8_t arglen, uint32_t *const id, uint16_t *const audience) {
    if (!_iotdata_node_content_control_id(args, arglen, id) || arglen < 5u)
        return false;
    *audience = (uint16_t)(((uint16_t)args[3] << 8) | args[4]);
    return true;
}

/* { u24 id, u8 encoding, u16 expiry-m, u24 sig-id, name NUL }. NOTHING HERE IS THE SIZE OR THE
   HASH: a node reads those off its own store when it authors the entry, which is what makes this
   command impossible to get wrong in the way that matters. */
static inline bool _iotdata_node_content_control_manifest(const uint8_t *const args, const uint8_t arglen, uint32_t *const id, uint8_t *const encoding, uint16_t *const expiry_m, uint32_t *const sig, const char **const name) {
    if (!_iotdata_node_content_control_id(args, arglen, id) || arglen < 10u)
        return false;
    *encoding = args[3];
    *expiry_m = (uint16_t)(((uint16_t)args[4] << 8) | args[5]);
    *sig = ((uint32_t)args[6] << 16) | ((uint32_t)args[7] << 8) | (uint32_t)args[8];
    /* The terminator has to fall inside the argument, for the reason §3 gives about the name on the
       wire: there is no length to consult for a second opinion. */
    uint8_t n = 9u;
    while (n < arglen && args[n] != '\0')
        n++;
    if (n >= arglen)
        return false;
    *name = (const char *)&args[9];
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_list(iotdata_node_content_t *const c) {
    uint32_t cursor = 0u, id = 0u, total = 0u, held = 0u, files = 0u;
    const char *owner = NULL;
    while (iotdata_node_content_iterate(c, &cursor, &id, &total, &held, &owner)) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %8" PRIu32 " of %8" PRIu32 " %-10s%s", id, held, total, (owner != NULL) ? owner : "-", iotdata_node_content_id_is_scratch(id) ? " scratch" : "");
        files++;
    }
    /* The totals LAST, so a long listing ends with the summary rather than opening with it and
       scrolling it away -- and so the line that says how many there were is the line that proves
       the walk finished. */
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %" PRIu32 " file%s, %" PRIu32 " bytes free", files, (files == 1u) ? "" : "s", iotdata_node_content_space(c));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_delete(iotdata_node_content_t *const c, const uint32_t id) {
    const iotdata_node_content_status_t st = iotdata_node_content_delete(c, id);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, (st == IOTDATA_NODE_CONTENT_OK) ? "deleted" : iotdata_node_content_status_name(st));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_validate(iotdata_node_content_t *const c, const uint32_t id) {
    uint64_t crc = 0u;
    uint32_t len = 0u;
    const iotdata_node_content_status_t st = iotdata_node_content_checksum(c, id, &crc, &len);
    if (st == IOTDATA_NODE_CONTENT_OK)
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " crc64=0x%016" PRIX64 " over %" PRIu32 " bytes", id, crc, len);
    else if (st == IOTDATA_NODE_CONTENT_ERR_SIZE)
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " crc64=0x%016" PRIX64 " over %" PRIu32 " bytes -- INCOMPLETE, so the sum is of a fragment", id, crc, len);
    else
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, iotdata_node_content_status_name(st));
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_inspect_ota(const uint32_t id, const uint8_t *const peek, const size_t peek_len, const uint32_t file_len) {
    iotdata_node_ota_header_t h;
    const iotdata_node_ota_status_t pst = iotdata_node_ota_parse(peek, peek_len, &h);
    if (pst == IOTDATA_NODE_OTA_ERR_MAGIC)
        return; /* not one, and silence is the right answer: most content is not firmware */
    if (pst != IOTDATA_NODE_OTA_OK) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota header: %s", id, iotdata_node_ota_status_name(pst));
        return;
    }
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota app=%s semver=%s stamp=%s", id, h.app, h.semver, h.stamp);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota form=%s codec=%s flags=0x%02x bootloader>=%" PRIu32, id, iotdata_node_ota_form_name(h.form), iotdata_node_ota_codec_name(h.codec), (unsigned)h.flags, h.bootloader_min);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota output=%" PRIu32 " payload=%" PRIu32 " at %u hashes=%s%s%s", id, h.output_size, h.payload_size, (unsigned)h.payload_offset, h.has_target_hash ? "target " : "",
                                      h.has_base_hash ? "base " : "", h.has_payload_hash ? "payload" : "");
    /* The length check is the one that needs the WHOLE file, and the whole file is what the store
       says it holds -- so a bundle that arrived short is caught here and not at the far end of a
       decode, which is the entire argument for PAYLOAD_SIZE existing. */
    if (_iotdata_node_content_control_device == NULL) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota check: not judged (this build declares nothing about itself)", id);
        return;
    }
    const iotdata_node_ota_status_t cst = iotdata_node_ota_check(&h, _iotdata_node_content_control_device, (size_t)file_len);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " ota check: %s", id, iotdata_node_ota_status_name(cst));
}

static inline void _iotdata_node_content_control_inspect(iotdata_node_content_t *const c, const uint32_t id) {
    uint32_t total = 0u, held = 0u;
    if (!iotdata_node_content_stat(c, id, &total, &held)) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %s", id, iotdata_node_content_status_name(IOTDATA_NODE_CONTENT_ERR_ABSENT));
        return;
    }
    /* THREE ANSWERS AND NOT TWO, because describe() returns 0 for two different situations and
       `orphan` was being printed for both: a file nobody owns, which on a build with no consumer
       registered is EVERY file and perfectly normal, and a file whose owner declines to say
       anything, which is an owned file and not an orphan at all. Asking who owns it separates
       them, and the honest wording for the common case says what is true rather than sounding an
       alarm about it. */
    char what[IOTDATA_NODE_CONTENT_DESCRIBE_MAX];
    const int n = iotdata_node_content_describe(c, id, what, sizeof(what));
    const char *const owner = iotdata_node_content_owner(c, id);
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " %" PRIu32 " of %" PRIu32 " bytes%s -- %s", id, held, total, iotdata_node_content_id_is_scratch(id) ? ", scratch" : "",
                                      (n > 0)           ? what
                                      : (owner != NULL) ? "claimed, and its owner says nothing about it"
                                                        : "nobody has claimed it");

    uint8_t peek[IOTDATA_NODE_CONTENT_CONTROL_PEEK_BYTES];
    const uint32_t want = (held < (uint32_t)sizeof(peek)) ? held : (uint32_t)sizeof(peek);
    if (want >= (uint32_t)IOTDATA_NODE_OTA_MAGIC_LEN && iotdata_node_content_read(c, id, 0u, peek, want) == (int)want)
        _iotdata_node_content_control_inspect_ota(id, peek, (size_t)want, held);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE OFFERS HALF
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * SAYING IT AGAIN, WITHOUT BEING ASKED. §3 makes periodic re-origination a requirement: an
 * advertisement's reach in time is the down path's broadcast slot expiry, not the manifest's own,
 * so once it ages out of the relays' slot tables nothing re-delivers it -- and an advertisement is
 * not requestable, so a node that joined or rebooted after that has no way to ask. Until this, an
 * offer only reached a late joiner if a person typed `content advertise` again.
 *
 * IT LIVES HERE AND NOT IN THE TABLE because only this layer has the originate callback. The table
 * answers which offer is due; this puts it on the air. A node with no callback -- a sensor -- ticks
 * and does nothing, which is the honest outcome for a node that cannot originate.
 *
 * ONE PER TICK, BROADCAST. One because four offers coming due together would otherwise go back to
 * back, which at 2.4 kbps is three seconds of self-collision; broadcast because that is what an
 * advertisement IS (§3), and because the whole point is reaching a node nobody knew was there.
 *
 * Returns the id originated, or 0.
 */
static inline uint32_t iotdata_node_content_control_readvertise(const uint32_t now_ms) {
    if (_iotdata_node_content_control_offers == NULL || _iotdata_node_content_control_originate == NULL)
        return 0u;
    const uint32_t id = iotdata_node_content_manifest_due(_iotdata_node_content_control_offers, now_ms);
    if (id == 0u)
        return 0u;
    uint8_t val[IOTDATA_CONTENT_MANIFEST_SIZE + IOTDATA_NODE_CONTENT_NAME_MAX + 1u];
    const size_t n = iotdata_node_content_manifest_advertise(_iotdata_node_content_control_offers, _iotdata_node_content_control_store, id, val, sizeof(val), now_ms);
    if (n == 0u)
        return 0u; /* the floor refused it, or it stopped being ours: either way, not now */
    if (!_iotdata_node_content_control_originate(_iotdata_node_content_control_originate_ctx, (uint16_t)IOTDATA_STATION_BROADCAST, val, n))
        return 0u;
    return id;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _iotdata_node_content_control_manifests(iotdata_node_content_manifests_t *const t, const uint32_t now_ms) {
    uint8_t cursor = 0u, offers = 0u;
    const iotdata_node_content_manifest_t *m = NULL;
    while (iotdata_node_content_manifest_iterate(t, &cursor, &m)) {
        _iotdata_node_content_say(iotdata_node_content_manifest_str(_iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), m, now_ms));
        offers++;
    }
    /* The totals last, for the reason the file listing gives: the line that says how many there
       were is the line that proves the walk finished. */
    _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %" PRIu8 " offer%s of %" PRIu8 " tracked", offers, (offers == 1u) ? "" : "s", (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX);
}

/* AUTHORING SAYS WHAT IT WROTE DOWN, and that is not politeness: the size and the hash are the
   node's own reading of its own store, so the one line that reports them is the only chance an
   operator has to see that the file they think they uploaded is the file the node summed. */
static inline iotdata_node_content_status_t _iotdata_node_content_control_author(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t id, const char *const name, const uint8_t encoding,
                                                                                 const uint16_t expiry_m, const uint32_t sig, const uint32_t now_ms) {
    const iotdata_node_content_status_t st = iotdata_node_content_manifest_author(t, c, id, name, encoding, expiry_m, sig, now_ms);
    if (st != IOTDATA_NODE_CONTENT_OK) {
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " not offered -- %s", id, iotdata_node_content_status_name(st));
        return st;
    }
    _iotdata_node_content_say(iotdata_node_content_manifest_str(_iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), iotdata_node_content_manifest_find(t, id), now_ms));
    return st;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE SAY AND THE RESULT ARE TWO AUDIENCES. The SAY lines are for whoever is at the console or on
   the gateway's MQTT channel: they can be long and can name an id. The result is for the counters
   and the one-line verdict -- and that is why the refusals below are REFUSED and not DONE. With a
   bool, "updates are switched off" counted as a serviced command, so the console printed the real
   reason and then "ok" underneath it. Both lines now agree. */
static inline iotdata_node_control_result_t iotdata_node_content_control(const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen, const uint32_t now_ms, const char **const reason) {
    iotdata_node_content_t *const c = _iotdata_node_content_control_store;
    if (c == NULL || subject != IOTDATA_NODE_TLV_CONTENT)
        return IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
#define _REFUSE(why) \
    do { \
        if (reason != NULL) \
            *reason = (why); \
        return IOTDATA_NODE_CONTROL_RESULT_REFUSED; \
    } while (0)
    uint32_t id = 0u;
    switch (action) {
    case IOTDATA_NODE_ACTION_CONTENT_LIST:
        _iotdata_node_content_control_list(c);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    case IOTDATA_NODE_ACTION_CONTENT_DELETE:
        if (!_iotdata_node_content_control_id(args, arglen, &id)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        _iotdata_node_content_control_delete(c, id);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    case IOTDATA_NODE_ACTION_CONTENT_INSPECT:
        if (!_iotdata_node_content_control_id(args, arglen, &id)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        _iotdata_node_content_control_inspect(c, id);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    case IOTDATA_NODE_ACTION_CONTENT_VALIDATE:
        if (!_iotdata_node_content_control_id(args, arglen, &id)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        _iotdata_node_content_control_validate(c, id);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    /* THE CONSUMER IS REACHED THROUGH ITS OWN MODULE POINTER, which its begin() sets, rather than
       being registered here a second time. A build with no firmware consumer answers honestly and
       the words still resolve -- which is better than a verb that exists on some nodes and is
       "unknown command" on others. REFUSED is how that honesty reaches the wire: the verb IS
       implemented here, there is simply nothing behind it on this build. */
    case IOTDATA_NODE_ACTION_CONTENT_OTA_STATUS:
        if (_iotdata_node_ota_apply == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: no consumer -- this build does not apply updates");
            _REFUSE("this build does not apply updates");
        }
        iotdata_node_content_ota_apply_report(_iotdata_node_ota_apply, _iotdata_node_content_control_line, sizeof(_iotdata_node_content_control_line), _iotdata_node_content_say);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    case IOTDATA_NODE_ACTION_CONTENT_OTA_APPLY:
        if (_iotdata_node_ota_apply == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: no consumer -- this build does not apply updates");
            _REFUSE("this build does not apply updates");
        }
        if (!_iotdata_node_content_control_id(args, arglen, &id)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        if (!iotdata_node_content_ota_apply_retry(_iotdata_node_ota_apply, id)) {
            const bool busy = iotdata_node_content_ota_apply_busy(_iotdata_node_ota_apply);
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: %06" PRIx32 " not armed -- %s", id, busy ? "an update is already running" : "updates are switched off");
            _REFUSE(busy ? "an update is already running" : "updates are switched off");
        }
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("firmware: %06" PRIx32 " armed -- it will be taken on the next offer", id);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    /* --- THE OFFERS. A build with no offers table answers honestly rather than going quiet: these
           verbs exist on every node that has the module, and what differs is whether the node was
           given somewhere to keep an offer and a way to put one on the air. */
    case IOTDATA_NODE_ACTION_CONTENT_MANIFEST_LIST:
        if (_iotdata_node_content_control_offers == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no offers table -- this build tracks none");
            _REFUSE("this build tracks no offers");
        }
        _iotdata_node_content_control_manifests(_iotdata_node_content_control_offers, now_ms);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    case IOTDATA_NODE_ACTION_CONTENT_MANIFEST_SET: {
        if (_iotdata_node_content_control_offers == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no offers table -- this build tracks none");
            _REFUSE("this build tracks no offers");
        }
        uint8_t encoding = 0u;
        uint16_t expiry_m = 0u;
        uint32_t sig = 0u;
        const char *name = NULL;
        if (!_iotdata_node_content_control_manifest(args, arglen, &id, &encoding, &expiry_m, &sig, &name)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: a manifest needs an id, an encoding, an expiry, a signature id and a name");
            _REFUSE("malformed manifest");
        }
        /* THE REASON, NOT A GUESS AT IT. This used to say "the store could not describe that id"
           for every refusal, including the ones that have nothing to do with the store -- an id
           belonging to another station being the one that would send an operator looking hardest
           in the wrong place. The line above already prints the real answer; this is the summary
           a control reply carries, so it says the same thing. */
        const iotdata_node_content_status_t arc = _iotdata_node_content_control_author(_iotdata_node_content_control_offers, c, id, name, encoding, expiry_m, sig, now_ms);
        if (arc != IOTDATA_NODE_CONTENT_OK)
            _REFUSE(iotdata_node_content_status_name(arc));
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    }
    case IOTDATA_NODE_ACTION_CONTENT_ADVERTISE: {
        uint16_t audience = (uint16_t)IOTDATA_STATION_BROADCAST;
        if (_iotdata_node_content_control_offers == NULL || _iotdata_node_content_control_originate == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: this build cannot originate -- it has no down path of its own");
            _REFUSE("this build cannot originate an advertisement");
        }
        if (!_iotdata_node_content_control_audience(args, arglen, &id, &audience)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        /* THE CALLBACK IS CHECKED FIRST, above, so that a generation is never spent on a frame this
           node was never going to be able to send -- the next origination would then look like a
           refresh of something nobody ever heard. A transmit that fails AFTER the pack still costs
           one, which is four bits wrapping at sixteen and cheap against the alternative. */
        uint8_t val[IOTDATA_CONTENT_MANIFEST_SIZE + IOTDATA_NODE_CONTENT_NAME_MAX + 1u];
        /* TAKEN BEFORE THE CALL, because the fourth reason destroys the evidence for itself: an
           offer whose bytes have been replaced is DROPPED by advertise(), so the row is gone by the
           time we look and the reason below reads it as "no offer of ours for that id" -- true,
           and the one thing an operator must not be told, since the id was right and the bytes are
           what moved. A system test found this by being unable to tell the two apart. */
        const uint32_t stale_before = _iotdata_node_content_control_offers->stat_stale;
        const size_t n = iotdata_node_content_manifest_advertise(_iotdata_node_content_control_offers, c, id, val, sizeof(val), now_ms);
        if (n == 0u) {
            /* THREE REASONS, AND THEY WANT DIFFERENT THINGS DOING. Only the advertiser originates,
               so a cache may not re-state what it merely heard -- that is a rule, not a fault, and
               nothing will change it. Coming round too fast IS a fault and clears by waiting. An id
               we know nothing about is a typo. An operator told "not advertised" without which of
               the three would go looking in the wrong place. */
            const iotdata_node_content_manifest_t *const row = iotdata_node_content_manifest_find(_iotdata_node_content_control_offers, id);
            const char *why = NULL;
            if (_iotdata_node_content_control_offers->stat_stale != stale_before)
                why = "the bytes under it have been replaced -- describe them again";
            else if (row == NULL)
                why = "no offer of ours for that id";
            else if (!row->local)
                why = "that offer is not ours to originate";
            else
                why = "originated too recently -- the generation is four bits and a loop would exhaust it";
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " not advertised -- %s", id, why);
            _REFUSE(why);
        }
        if (!_iotdata_node_content_control_originate(_iotdata_node_content_control_originate_ctx, audience, val, n)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " could not be put on the air", id);
            _REFUSE("the frame could not be sent");
        }
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " advertised to %03" PRIX16 " (%zu bytes)", id, audience, n);
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    }
    case IOTDATA_NODE_ACTION_CONTENT_REVOKE: {
        if (_iotdata_node_content_control_offers == NULL) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no offers table -- this build tracks none");
            _REFUSE("this build tracks no offers");
        }
        if (!_iotdata_node_content_control_id(args, arglen, &id)) {
            _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: no id given");
            _REFUSE("no id given");
        }
        /* SAY IT BEFORE DOING IT. The frame needs no row to build from -- a revocation is an id and
           a zero expiry and nothing else worth stating -- but telling the fleet first is the order
           that cannot leave a holder serving bytes this node has already destroyed. */
        uint8_t val[IOTDATA_CONTENT_MANIFEST_SIZE];
        const size_t n = iotdata_node_content_manifest_advertise_revoke(id, val, sizeof(val));
        bool aired = false;
        if (n > 0u && _iotdata_node_content_control_originate != NULL)
            aired = _iotdata_node_content_control_originate(_iotdata_node_content_control_originate_ctx, (uint16_t)IOTDATA_STATION_BROADCAST, val, n);
        const iotdata_node_content_status_t st = iotdata_node_content_manifest_revoke(_iotdata_node_content_control_offers, c, id, now_ms);
        _IOTDATA_NODE_CONTENT_CONTROL_SAY("content: %06" PRIx32 " revoked here%s -- %s", id, aired ? " and announced" : " only (nothing was put on the air)", iotdata_node_content_status_name(st));
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    }
    default:
        break;
    }
#undef _REFUSE
    return IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
}

static inline iotdata_node_control_result_t iotdata_node_content_node_control(__attribute__((unused)) const uint16_t station, const uint8_t subject, const uint8_t action, const uint8_t *const args, const uint8_t arglen,
                                                                              const uint32_t now_ms, const char **const reason) {
    return iotdata_node_content_control(subject, action, args, arglen, now_ms, reason);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_CONTROL_H */
