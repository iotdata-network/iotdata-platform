#ifndef IOTDATA_NODE_CONTENT_MANIFEST_H
#define IOTDATA_NODE_CONTENT_MANIFEST_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// iotdata_node_content_manifest.h - what this node knows is ON OFFER, which is a different question
// from what it is holding. NOTES_CONTENT §3.
//
// The two halves of content are the MANIFEST -- advertising a transfer, with the metadata that lets
// the other end decide whether it wants it -- and the TRANSFER itself. iotdata_node_content.h is
// the store and knows only bytes; this is the offers, and it is the half that says what those bytes
// ARE: how many of them there should be, what they must hash to, how they are wrapped, what they
// are called, and for how long the offer stands.
//
// WHY NOT IN iotdata_node_content.h, which is where the bytes live. The store is deliberately free
// of this library: it is written against a backend and nothing else, and is tested on a host with
// no protocol in sight. A manifest is protocol through and through -- its codec, its expiry
// semantics and its revocation sentinel are all iotdata_node.h's -- so putting the table there
// would cost the store the one property that makes it cheap to test. The split is the same one
// iotdata_node_content_control.h already makes for the CONTROL words.
//
// REQUIRES, from the includer and in this order: iotdata_node.h (the codec and the sentinels),
// device/d_module_filestore.h, device/d_format.h (snprintf_inline) and iotdata_node_content.h.
// Nothing here includes them, for the reason the rest of the node layer does not either.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// TWO WAYS IN AND ONE CODE PATH, which is the shape iotdata_node_content_announce() has and for
// the same reason: a manifest that arrives over the air and one an operator authors must not be
// two implementations of "this is what that id is".
//
//     author()    the operator names what the bytes cannot say -- the name, the encoding, the
//                 expiry, the signature stream -- and the SIZE AND HASH COME FROM THE STORE. There
//                 is nothing to mistype that matters, and a manifest for a file this node is not
//                 holding, or is holding only part of, is refused rather than invented.
//
//                 IT TAKES ANY SIGNATURE ID, 0 INCLUDED, and that is deliberate: §3 makes the
//                 field mandatory to PROVIDE, and the place that provides is the command, not
//                 this. The control vocabulary's parser and the control tool have no default for
//                 it and refuse to build an offer without one, so by the time a value arrives
//                 here somebody has stated it -- possibly as `self`, which is how a signature
//                 stream's own manifest says it names none. Enforcing it a second time here would
//                 also reject what arrives over the air, which is not ours to judge.
//
//     on_wire()   a MANIFEST TLV arrived. Parsed, then the same apply().
//
// AUTHORING IS WHAT MAKES THE GATEWAY AN ADVERTISER. §3 leaves provisioning out of the protocol --
// "how a content-id comes to exist is NOT the protocol's business" -- which left the manifest with
// no source at all: the store is files named as ids and nothing else. So the source is a person,
// reached either by a console or by a CONTROL command over MQTT, and the fields they supply are
// exactly the ones no amount of reading the file would reveal.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// ONLY THE ADVERTISER ORIGINATES, AND THAT IS ENFORCED HERE
//
// §3 forbids a cache from re-stating an offer: the expiry is an offset from receipt and is NOT
// decremented when forwarded, so a relay that re-originated from its cache would extend the
// lifetime indefinitely. What keeps that safe is the constraint rather than any arithmetic, so
// advertise() refuses a row this node did not author. A node repeats an advertisement it heard by
// flooding the frame, which is the down path's job and not this module's.
//
// RE-ORIGINATION IS A REQUIREMENT, NOT AN OPTIMISATION. An advertisement's reach in time is the
// down path's broadcast slot expiry, not the manifest's own: once it ages out of the relays' slot
// tables nothing re-delivers it, so a node that joins, reboots or comes back into range after that
// cannot discover an offer that is sitting there indefinitely -- and an advertisement is not
// requestable, so it has no way to ask. Hence advertise() BUMPS THE GENERATION every time: the
// refresh interval has to be shorter than the slot expiry, which puts every refresh inside the
// dedup lifetime by construction, so a byte-identical re-advert would be thrown by every relay
// that still remembered the last one. The four idle bits of the shared header are what make the
// frame differ.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// EXPIRY IS KEPT AS STATED AND ANSWERED AS REMAINING
//
// The row holds the minutes the advertiser stated and the moment we heard it, never a deadline.
// That is what lets §13's two different meanings share one field: an advertisement STATES an
// expiry from receipt, and a `request(CONTENT)` response reports what it has OBSERVED remaining.
// A deadline would have thrown away the first.
//
// REVOCATION DESTROYS; EXPIRY MERELY FORGETS. §3 draws that line explicitly and it is the one
// asymmetry here: a revoked entry must actively evict the cached copy -- and destroy it rather than
// mark it, because a cache that survives a reboot and kept the revocation as a live flag comes back
// serving revoked bytes -- where an expired entry need only stop being forwarded. So a revocation
// deletes the content and the row; an expiry drops the row and leaves the bytes where they are.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// AN AUTHORED OFFER PERSISTS IN A SIDECAR; A HEARD ONE DOES NOT
//
// A HEARD offer is RAM and should be: its expiry is an offset on a monotonic clock, which means
// nothing across a restart, and the advertiser re-originates anyway. An AUTHORED one has to
// survive, or an operator re-types it every time a node reboots -- which is not hypothetical, it
// cost the first bench run its offer within minutes.
//
// SO WHAT A PERSON SAID GOES BESIDE THE CONTENT, through d_filestore_t's meta_write/meta_read. The
// reasoning for that home rather than a node state block is in the seam; the shape here is what
// matters: THE SIDECAR IS A MANIFEST TLV VALUE. The same packer that builds an advertisement writes
// it and the same parser reads it, so there is no second format to keep honest, no version byte,
// and the forward-compatibility rule (§3: a field appended after the name is skipped by a decoder
// that stops when it has consumed what it knows) applies to the stored copy for nothing.
//
// SIZE AND HASH ARE RE-DERIVED AND NEVER TRUSTED FROM IT. The sidecar is stored with them because
// that is what the pack emits, and restore reads them back out of the STORE instead -- so an offer
// always describes the bytes that are actually there, and a file that decayed or was replaced by
// hand cannot be advertised under a stale hash. That makes the restore O(n) in what is held, which
// is a boot cost and is why nothing but begin() does it.
//
// THE GENERATION IS THE ONE THING THAT MUST SURVIVE EXACTLY. After a restart a table starting from
// zero would re-use a generation that relays still remember, and a byte-identical advertisement is
// thrown by every one of them -- the precise failure the generation exists to prevent. So
// advertise() rewrites the sidecar when it bumps. That is a store write per origination, and
// originations are hours or days apart (§3: "a few frames a week"), so the cost is nothing.
//
// A STORE THAT CANNOT KEEP ONE leaves the two seam calls NULL, and then nothing is written and
// nothing is restored. Absent is a real answer: such a node simply behaves as it did before.
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

/* How many distinct content-ids this node keeps an offer for -- §12's "manifests tracked". Offers,
   not files: a node can be holding bytes for an id it has no manifest for (a file put there by
   cable, or baked in) and can have a manifest for an id it holds nothing of, which is exactly the
   state a node is in between hearing an advertisement and deciding to fetch it. */
#ifndef IOTDATA_NODE_CONTENT_MANIFESTS_MAX
#define IOTDATA_NODE_CONTENT_MANIFESTS_MAX 8
#endif

/* §3 calls 32 bytes a recommendation and not a limit, and on the wire it is neither enforced nor
   needed -- the frame's own space is the only bound. A stored row has to pick a number, so this is
   that number and a longer name is TRUNCATED rather than refused: the name is descriptive, what it
   MEANS is the application's, and losing its tail is a worse listing rather than a wrong offer.
   The id, the size and the hash are what anything acts on, and none of them are affected. */
#ifndef IOTDATA_NODE_CONTENT_NAME_MAX
#define IOTDATA_NODE_CONTENT_NAME_MAX 32
#endif

/* The widest the union of the two sources can be: every file the store will show in one pass, plus
   every offer the table can hold. It sizes a gathered row listing and bounds the restore's
   snapshot of ids, so it is declared with the other dimensions rather than beside one use. */
#define IOTDATA_NODE_CONTENT_TABLE_MAX (IOTDATA_NODE_CONTENT_MANIFESTS_MAX + IOTDATA_NODE_CONTENT_SCAN_MAX)

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE SAFETY CAPS
//
// An offer is a statement by SOMEBODY ELSE that costs this node table space, airtime, and -- once
// the transfer exists -- flash and battery. §8 is explicit that the protocol has no authenticity:
// "anyone can advertise a content-id and serve garbage". None of these caps exist to make a
// correct advertiser work; they exist so an incorrect one cannot do damage. Every one of them is
// therefore applied by the RECEIVER, except the first, which is a guard on our own origination.
// -----------------------------------------------------------------------------------------------------------------------------------------

/* HOW OFTEN AN ADVERTISER RE-ORIGINATES ITS OWN OFFERS, and §3 makes this a REQUIREMENT rather
   than an optimisation. An advertisement's reach in time is the down path's BROADCAST SLOT EXPIRY,
   not the manifest's own: it is a broadcast down frame, so once it ages out of the relays' slot
   tables nothing re-delivers it to anybody. A node that joins the network, reboots, or comes back
   into range after that point cannot see an offer that is sitting there indefinitely -- and an
   advertisement is not requestable, so it has no way to ask. The cost of getting this wrong is a
   node that can never discover content that is right there.
 *
 * TWELVE HOURS, AND THE NUMBER IS BOUNDED AT BOTH ENDS. It has to be well SHORTER than the
   broadcast slot expiry (a week by default) or a late joiner never hears it. It also has to be
   long enough that SIXTEEN of them -- the whole four-bit generation -- outlast that same expiry,
   or a generation comes round again while a relay still remembers the frame that carried it and
   the refresh is deduped away. Twelve hours gives eight days against a seven-day lifetime, which
   is the narrower of the two constraints and the one that sets this.
 *
 * 0 TURNS IT OFF, for a node that would rather be asked. */
#ifndef IOTDATA_NODE_CONTENT_READVERTISE_S
#define IOTDATA_NODE_CONTENT_READVERTISE_S (12u * 60u * 60u)
#endif

/* THE FLOOR UNDER RE-ORIGINATION, and the reason is not airtime. THE GENERATION IS FOUR BITS, so
   sixteen originations exhaust it and the seventeenth reuses a value that may still be in a relay's
   dedup memory -- after which the offer is suppressed, and the one message whose whole job is to
   keep arriving stops arriving. The safe interval is therefore long against the DEDUP LIFETIME
   rather than long in itself: a week-long broadcast lifetime divided by sixteen is about ten hours,
   which is why §3 describes the real cadence as "a few frames a week".
 *
 * THIS IS NOT THAT CADENCE. It is the floor that stops a runaway -- a script in a loop, a stuck
   retry -- burning the generation in under a minute and flooding the air doing it. Choosing the
   cadence is the advertiser's policy; refusing an absurd one is this module's. A bench defines it
   lower, which is what the gitignored host config is for. */
#ifndef IOTDATA_NODE_CONTENT_READVERTISE_MIN_S
#define IOTDATA_NODE_CONTENT_READVERTISE_MIN_S 60u
#endif

/* THE LONGEST EXPIRY THIS NODE WILL HONOUR, applied to what ARRIVES and never to what we author.
   §3 makes 0xFFFF legitimate and likely common -- "an OTA should stay available until it is
   replaced" -- and names the consequence in the same breath: "an indefinite entry never ages out,
   so eviction is its only backstop". An advertiser that dies without revoking therefore holds a
   row on every node that ever heard it, for as long as that node runs.
 *
 * CLAMPING WHAT WE ACCEPT COSTS NOTHING, because §3 already requires periodic re-origination: a
   live offer is re-stated far inside any cap, so the clamp can only ever bite an advertiser that
   has gone quiet -- which is precisely the case it is for. The effect is that RE-ORIGINATION keeps
   an offer alive rather than the advertiser's say-so, and for a statement we cannot verify that is
   the right way round.
 *
 * NOT APPLIED TO OUR OWN. What we author is what we put on the wire, and `indefinite` is a value
   the protocol has; taking it away here would be this node protecting itself from itself. A
   receiver clamps it, which is where the judgement belongs. */
#ifndef IOTDATA_NODE_CONTENT_EXPIRY_MAX_M
#define IOTDATA_NODE_CONTENT_EXPIRY_MAX_M (7u * 24u * 60u) /* a week */
#endif

/* HOW MANY ROWS ONE ORIGINATOR MAY OCCUPY. §4's allocation puts the originating station in the top
   twelve bits of a content id, so this costs nothing to work out -- and without it one chatty or
   broken node mints ids until the table is full and a legitimate offer from the gateway cannot get
   in at all. Our OWN rows are exempt: the table is ours, and a node is not its own attacker.
   0 means no limit. */
/* AND IT IS NEVER LESS THAN TWO, which is not a rounding convenience. §3 has one advertiser
   offering SEVERAL FLAVOURS of the same thing at once -- a whole image and a patch against it, each
   its own content-id -- and the endpoint choosing between them. A share below two would stop a node
   holding both halves of that offer and the choice would never be available. Four, on the default
   eight-row table, is also what §3's own arithmetic puts in one frame. */
#define _IOTDATA_NODE_CONTENT_PER_ORIGIN_HALF ((IOTDATA_NODE_CONTENT_MANIFESTS_MAX + 1u) / 2u)
#ifndef IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX
#define IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX ((_IOTDATA_NODE_CONTENT_PER_ORIGIN_HALF < 2u) ? 2u : _IOTDATA_NODE_CONTENT_PER_ORIGIN_HALF)
#endif

/* Long enough for every field a describe() states, with the name at its full width. */
#define IOTDATA_NODE_CONTENT_MANIFEST_STR_MAX (96 + IOTDATA_NODE_CONTENT_NAME_MAX)

/* THE DUPLICATE THAT CANNOT SILENTLY DIVERGE. The chunk size is stated twice on purpose: the
   library states it because the protocol owns it, and d_module_filestore.h states it because a d_
   module has to be includable without the library. Neither can include the other, so this -- the
   one place that sees both -- is where they are made to agree. */
_Static_assert((uint32_t)D_FILESTORE_CHUNK_BYTES == (uint32_t)IOTDATA_CONTENT_CHUNK_BYTES, "the store's chunk size and the protocol's have drifted apart");

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint32_t id; /* 0 is not a content-id, so it marks an empty row */
    uint32_t size;
    uint64_t hash;
    uint32_t sig_id;
    uint16_t expiry_m;  /* AS STATED, from receipt. 0xFFFF indefinite; 0 never reaches a row */
    uint8_t generation; /* the header's tag, four bits, wrapping */
    uint8_t encoding;
    uint32_t stated_ms; /* our own clock when we heard it, or authored it */
    bool local;         /* authored here: only these may be advertised */
    /* WHEN WE LAST PUT IT ON THE AIR, which is not stated_ms: authoring restamps that, and an
       operator who describes an offer and advertises it in the same breath must not be told to
       wait. 0 is "never originated", so the first advertisement after an author -- or after a
       restart, where the row came back from a sidecar -- is always allowed. */
    uint32_t advertised_ms;
    char name[IOTDATA_NODE_CONTENT_NAME_MAX + 1];
} iotdata_node_content_manifest_t;

/* THE CADENCE CANNOT BE FASTER THAN THE FLOOR, or every automatic refresh would be refused and the
   offer would silently stop being advertised -- the exact failure both numbers exist to prevent.
   Zero means off at either end and is excluded from the comparison. */
#if IOTDATA_NODE_CONTENT_READVERTISE_S > 0u && IOTDATA_NODE_CONTENT_READVERTISE_MIN_S > 0u
_Static_assert((unsigned)IOTDATA_NODE_CONTENT_READVERTISE_S >= (unsigned)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S, "the re-advertise cadence is faster than the floor that refuses it");
#endif

typedef struct {
    iotdata_node_content_manifest_t m[IOTDATA_NODE_CONTENT_MANIFESTS_MAX];
    /* Whether THIS node's store survives a power cycle, which only the application knows: a
       directory may be a tmpfs, a SPIFFS partition is durable, and the harness's RAM store is not.
       It is here rather than in the seam because it is the one thing a STATUS row needs that no
       backend call can answer, and widening d_filestore_t for one bool would touch four stores. */
    bool persistent;
    uint32_t stat_authored, stat_heard, stat_refreshed, stat_superseded, stat_revoked, stat_expired, stat_advertised;
    /* WHAT THE CAPS DID, counted rather than logged: a run of these is a neighbour behaving badly
       or an advertiser in a loop, and that is a thing to be able to read off a node. */
    uint32_t stat_capped;  /* an arriving expiry clamped to what this node will honour */
    uint32_t stat_crowded; /* refused: that originator already has its share of the table */
    uint32_t stat_tooSoon; /* an origination refused for coming round again too fast */
    uint32_t stat_stale;   /* an offer dropped because the bytes under it had been replaced */
    uint32_t stat_foreign; /* an id claimed by the wrong station -- either way round */
    uint16_t station;      /* who we are, for §4's allocation; 0 = not told, checks off */
    /* Both mean the node is misdimensioned or is being told nonsense, rather than that something
       went wrong, so they are counted and visible instead of logged once and lost. */
    uint32_t stat_full, stat_refused;
} iotdata_node_content_manifests_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * THE STATION IS WHO WE ARE ALLOWED TO BE, and §4 already decided it: a content id is
 * `(station << 12) | counter`, so the top twelve bits name the ORIGINATING station and nothing
 * else in the system may put them there. The architecture uses that shape everywhere; this is the
 * offers table holding up its end of it, in both directions:
 *
 *   WE MAY NOT AUTHOR an id that is not ours -- see author(). Without that, a node could describe
 *   content under somebody else's station, and would then refuse that station's own updates and
 *   revocations for it while going on serving the bytes. Immovable, and exactly the kind of thing
 *   that only shows up months later.
 *
 *   NOBODY ELSE MAY ADVERTISE one of ours -- see apply(). This is the rule that matters, because it
 *   does not depend on what is in the table: our own advertisement floods and comes back off every
 *   relay that heard it, and a down frame names its TARGET and carries no sender, so a manifest for
 *   our id arriving off the air is indistinguishable from the echo of one we sent. Taking it would
 *   hand our own generation and clock back to us from one hop away, mark the row not-ours, and
 *   since only a local row is ever originated, we would stop advertising our own content for good.
 *
 * 0 MEANS WE HAVE NOT BEEN TOLD, and then neither check is enforced -- a host harness, or a node
 * whose station is not settled when the store is mounted. The weaker table-state guard in apply()
 * still stands in that case, which is why it is kept rather than replaced.
 */
static inline void iotdata_node_content_manifests_init(iotdata_node_content_manifests_t *const t, const bool store_is_persistent, const uint16_t station) {
    if (t == NULL)
        return;
    *t = (iotdata_node_content_manifests_t){ 0 };
    t->persistent = store_is_persistent;
    t->station = station;
}

/* True if this id belongs to the station we are. */
static inline bool _iotdata_node_content_manifest_is_ours(const iotdata_node_content_manifests_t *const t, const uint32_t id) {
    return t != NULL && t->station != 0u && (uint16_t)(id >> 12) == t->station;
}

static inline uint8_t iotdata_node_content_manifests_count(const iotdata_node_content_manifests_t *const t) {
    uint8_t n = 0;
    if (t != NULL)
        for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++)
            if (t->m[i].id != 0u)
                n++;
    return n;
}

static inline const iotdata_node_content_manifest_t *iotdata_node_content_manifest_find(const iotdata_node_content_manifests_t *const t, const uint32_t id) {
    if (t == NULL || id == 0u)
        return NULL;
    for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++)
        if (t->m[i].id == id)
            return &t->m[i];
    return NULL;
}

/* Walk the occupied rows. *cursor starts at 0 and is the caller's; the row is handed out by
   pointer into the table, so it is valid until the next apply(). */
static inline bool iotdata_node_content_manifest_iterate(const iotdata_node_content_manifests_t *const t, uint8_t *const cursor, const iotdata_node_content_manifest_t **const out) {
    if (t == NULL || cursor == NULL || out == NULL)
        return false;
    while (*cursor < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX) {
        const iotdata_node_content_manifest_t *const m = &t->m[(*cursor)++];
        if (m->id != 0u) {
            *out = m;
            return true;
        }
    }
    return false;
}

/* What is LEFT of the offer, in minutes, which is the form §13's response reports. Indefinite stays
   indefinite rather than counting down from 45 days, because the two are different statements.
 *
 * IT ROUNDS UP, AND THAT IS NOT A ROUNDING PREFERENCE. 0 in this field is the REVOCATION sentinel,
   and §13 has a holder re-reporting its observed remainder in the same field an advertisement
   states one in -- so a row with fifty seconds left that answered 0 would be passing on "destroy
   your copy". Truncating the ELAPSED minutes is what keeps 0 meaning expired and nothing else; the
   cost is over-stating a live offer by under a minute, against a field that spans 45 days.
 *
 * The difference is read as SIGNED, which is the same idiom iotdata_node_down.h orders its own
   thresholds with, and here it is load-bearing rather than tidy: a stamp that is ahead of the
   clock being compared against it makes an unsigned subtraction 4.29 billion milliseconds, every
   expiry instantly past, and a 24-hour offer vanishes on the next tick. Threading the pass's clock
   (see iotdata_node_control_fn) is what should stop a stamp ever being ahead; this is what stops
   the consequence being catastrophic if one ever is -- and the gateway's own control chain does
   not thread one yet. A stamp in the future reads as "just stated", which is what it is.
 *
 * It survives one wrap of the millisecond clock and does not have to survive two, because the
   widest statable expiry (0xFFFE minutes, 45.5 days) is inside the 49.7 days the clock orders. An
   indefinite row never does the arithmetic at all. */
static inline uint16_t iotdata_node_content_manifest_remaining_m(const iotdata_node_content_manifest_t *const m, const uint32_t now_ms) {
    if (m == NULL)
        return 0u;
    if (m->expiry_m == (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE)
        return (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE;
    const int32_t since_ms = (int32_t)(now_ms - m->stated_ms);
    const uint32_t elapsed_m = (since_ms <= 0) ? 0u : ((uint32_t)since_ms / 60000u);
    return (elapsed_m >= (uint32_t)m->expiry_m) ? 0u : (uint16_t)((uint32_t)m->expiry_m - elapsed_m);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE ONE CODE PATH
// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * A NAME INTO A ROW, CLIPPED ON PURPOSE. §3 calls 32 bytes a recommendation and not a limit, so a
 * name arriving off the air or typed at a console can be longer than a row holds, and losing its
 * tail is a worse listing rather than a wrong offer -- the id, the size and the hash are what
 * anything acts on.
 *
 * WRITTEN AS A BOUNDED COPY AND NOT AS snprintf("%s"), because -Wformat-truncation is right to
 * object to the latter and cannot tell "deliberately clipped" from "oops": the aarch64 -O3 build
 * refused it the moment restore() started handing it a name out of a 96-byte sidecar buffer. The
 * intent belongs in the code rather than in a suppression.
 */
static inline void _iotdata_node_content_manifest_name(char *const dst, const char *const src) {
    size_t n = 0;
    if (src != NULL)
        while (src[n] != '\0' && n < (size_t)IOTDATA_NODE_CONTENT_NAME_MAX)
            n++;
    if (n > 0u)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline iotdata_node_content_manifest_t *_iotdata_node_content_manifest_slot(iotdata_node_content_manifests_t *const t, const uint32_t id) {
    for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++)
        if (t->m[i].id == id)
            return &t->m[i];
    return NULL;
}

/*
 * Apply one manifest, however it reached us.
 *
 * WHAT IT CHECKS AGAINST THE STORE, and what it leaves to whoever reads the file. §3: "if an entry
 * arrives for an id a node holds bytes for, and its total size or gross hash differs, the held
 * bytes are destroyed" -- which is what makes id reuse safe, since the recommended allocation
 * wraps after 4096. The SIZE is asked of the store, which costs a stat. The HASH is compared only
 * against a manifest we already had, which is free; comparing it against the bytes would mean
 * reading the whole file, and iotdata_node_content_checksum() says plainly why a tick may not do
 * that. So an offer that agrees on size and arrives for bytes nothing has described before is
 * recorded and the bytes kept -- and the hash still has to hold, at the one moment the full read
 * belongs: when something verifies the file in order to use it.
 */
static inline iotdata_node_content_status_t _iotdata_node_content_manifest_apply(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const iotdata_node_content_manifest_t *const in, const uint32_t now_ms) {
    if (t == NULL || in == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (!iotdata_node_content_id_is_valid(in->id)) {
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_ID;
    }

    iotdata_node_content_manifest_t *row = _iotdata_node_content_manifest_slot(t, in->id);

    /*
     * NOTHING OFF THE AIR MAY TOUCH AN OFFER OF OURS. This is first because everything below it
     * can reach the store: a revocation DELETES, and a manifest whose size or hash disagrees with
     * what we hold DELETES. The refusal used to sit after both of them, where it correctly kept
     * the row and had already lost the bytes.
     *
     * §8 IS WHAT MAKES THAT SERIOUS: there is no authenticity in a manifest, so "anyone can
     * advertise a content-id" also meant anyone could destroy the content behind one -- a single
     * 38-byte frame naming an id a gateway was distributing, and the image was gone from it. The
     * ordinary case is more mundane and just as wrong: our own advertisement coming back off a
     * relay carries our id, and a hash that has moved on since would have had us delete our own
     * bytes on hearing ourselves.
     *
     * AN OPERATOR STILL WITHDRAWS THEIR OWN CONTENT, through the local entry points -- revoke()
     * and the console and MQTT verbs that reach it, all of which arrive here with local set. What
     * is refused is a frame off the air deciding the fate of bytes this node stands behind.
     */
    if (!in->local && _iotdata_node_content_manifest_is_ours(t, in->id)) {
        /* §4'S RULE, AND IT DOES NOT CONSULT THE TABLE -- which is the point. The guard below can
           only speak for ids we currently have a row for, so an offer of ours that had expired, or
           been crowded out, left our own content reachable from the air again: a revocation naming
           it would have deleted the bytes. This holds whether or not we remember the offer. */
        t->stat_foreign++;
        return IOTDATA_NODE_CONTENT_ERR_STATION;
    }
    if (row != NULL && row->local && !in->local) {
        /* THE SAME RULE FROM TABLE STATE, for a node that has not been told its station. Weaker,
           because it needs the row to still be there, and kept for exactly that case. */
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    }

    /* A REVOCATION IS NOT A ROW. Destroy the bytes, drop whatever we knew, and keep no trace: a
       stored "this was revoked" is the live flag §3 warns against. */
    if (in->expiry_m == (uint16_t)IOTDATA_CONTENT_EXPIRY_REVOKED) {
        (void)iotdata_node_content_delete(c, in->id);
        if (row != NULL)
            *row = (iotdata_node_content_manifest_t){ 0 };
        t->stat_revoked++;
        return IOTDATA_NODE_CONTENT_OK;
    }

    if (in->size == 0u) {
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_SIZE;
    }

    /* THE CAPS, on what ARRIVES. See THE SAFETY CAPS above for why each one is here and why none
       of them touches a row we authored ourselves. */
    uint16_t expiry_m = in->expiry_m;
    if (!in->local && expiry_m > (uint16_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M && expiry_m != (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE) {
        expiry_m = (uint16_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M;
        t->stat_capped++;
    } else if (!in->local && expiry_m == (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE) {
        /* INDEFINITE BECOMES THE CAP rather than being refused: the offer is honoured, for as long
           as this node is willing to honour one, and the advertiser keeps it alive by saying so
           again -- which §3 requires of it anyway. */
        expiry_m = (uint16_t)IOTDATA_NODE_CONTENT_EXPIRY_MAX_M;
        t->stat_capped++;
    }

    uint32_t total = 0u, held = 0u;
    const bool holds = iotdata_node_content_stat(c, in->id, &total, &held);
    const bool size_differs = holds && total != in->size;
    const bool hash_differs = (row != NULL) && row->hash != in->hash;
    if (holds && (size_differs || hash_differs)) {
        (void)iotdata_node_content_delete(c, in->id);
        t->stat_superseded++;
    }

    if (row == NULL) {
        /* ONE ORIGINATOR'S SHARE, counted before a slot is taken: §4 puts the originating station
           in the top twelve bits, so this is free to work out, and without it one broken node
           fills the table and locks everybody else out. */
        if (!in->local && (uint32_t)IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX > 0u) {
            uint32_t mine = 0u;
            for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++)
                if (t->m[i].id != 0u && !t->m[i].local && (t->m[i].id >> 12) == (in->id >> 12))
                    mine++;
            if (mine >= (uint32_t)IOTDATA_NODE_CONTENT_PER_ORIGIN_MAX) {
                t->stat_crowded++;
                return IOTDATA_NODE_CONTENT_ERR_FULL;
            }
        }
        row = _iotdata_node_content_manifest_slot(t, 0u);
        if (row == NULL) {
            /* NO EVICTION POLICY, deliberately. §9 gives the module the choice of which content to
               give up, by least-recent DEMAND -- and demand is a signal the requester supplies and
               nothing here has yet. Refusing and counting is the honest answer until it does;
               inventing an order now would be inventing the wrong one. */
            t->stat_full++;
            return IOTDATA_NODE_CONTENT_ERR_FULL;
        }
    } else {
        t->stat_refreshed++;
    }

    const uint32_t was_id = row->id;
    /* WHEN WE LAST ORIGINATED IT IS THE ROW'S, NOT THE STATEMENT'S. It is how the floor under
       re-origination is measured, so letting an incoming manifest carry it would let an advertiser
       reset our rate limit by talking to us. */
    const uint32_t was_advertised_ms = row->advertised_ms;
    *row = *in;
    row->stated_ms = now_ms;
    /* THE CLAMPED EXPIRY, NOT THE ONE THAT ARRIVED. The cap above computes it; this is where it
       takes effect, and writing *in wholesale without it would honour the advertiser's figure
       after all. */
    row->expiry_m = expiry_m;
    /* A ROW'S OWN FLOOR SURVIVES A RE-DESCRIPTION. author() clears the slot before applying, so
       the carry-over has to come through `in` when there is no row left to read it from -- and an
       incoming manifest carries 0 here, which is why the row's value wins whenever it has one. */
    row->advertised_ms = (was_advertised_ms != 0u) ? was_advertised_ms : in->advertised_ms;
    row->name[IOTDATA_NODE_CONTENT_NAME_MAX] = '\0';
    if (was_id == 0u) {
        if (in->local)
            t->stat_authored++;
        else
            t->stat_heard++;
    }
    return IOTDATA_NODE_CONTENT_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* A ROW AS A MANIFEST TLV VALUE, which is both what goes on the air and what goes in the sidecar.
   One encoder for the two, so a stored description and an advertised one cannot drift, and the
   generation is a parameter because advertise() needs to pack the NEXT one without committing to
   it until the frame has been built. */
static inline size_t _iotdata_node_content_manifest_wire(const iotdata_node_content_manifest_t *const m, const uint8_t generation, uint8_t *const buf, const size_t size) {
    if (m == NULL || buf == NULL || m->id == 0u)
        return 0u;
    const iotdata_content_manifest_t w = {
        .id = m->id,
        .generation = generation,
        .size = m->size,
        .hash = m->hash,
        .sig_id = m->sig_id,
        .expiry_m = m->expiry_m,
        .encoding = m->encoding,
        .name = m->name,
    };
    return iotdata_content_manifest_pack(buf, size, &w);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE SIDECAR. A description beside the bytes, so it outlives a restart -- and it IS a MANIFEST
   TLV value, which means one encoder serves the air and the store both, and §3's forward
   compatibility comes free. The size and the hash are re-derived on restore and never trusted
   from here; the NAME and the GENERATION are what only this can carry. */
static inline void _iotdata_node_content_manifest_save(const iotdata_node_content_t *const c, const iotdata_node_content_manifest_t *const m) {
    if (c == NULL || c->backend == NULL || c->backend->meta_write == NULL || m == NULL || m->id == 0u)
        return;
    uint8_t buf[D_FILESTORE_META_MAX];
    const size_t n = _iotdata_node_content_manifest_wire(m, m->generation, buf, sizeof(buf));
    if (n > 0u)
        (void)c->backend->meta_write(c->backend->ctx, m->id, buf, n);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* STOPS DESCRIBING WHAT WE NO LONGER OFFER, without touching the bytes. The counterpart of
   _save(), and the reason it exists is a restart: a local offer that expired out of the table came
   straight back from its sidecar on the next boot, so the SAME offer lived for ten minutes or
   forever depending on whether anybody power-cycled the node. Withdrawing has to be durable or it
   is not withdrawing. */
static inline void _iotdata_node_content_manifest_forget(const iotdata_node_content_t *const c, const uint32_t id) {
    if (c == NULL || c->backend == NULL || c->backend->meta_write == NULL || id == 0u)
        return;
    (void)c->backend->meta_write(c->backend->ctx, id, NULL, 0u); /* a zero-length write clears it */
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_status_t iotdata_node_content_manifest_author(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t id, const char *const name, const uint8_t encoding,
                                                                                 const uint16_t expiry_m, const uint32_t sig_id, const uint32_t now_ms) {
    if (t == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (!iotdata_node_content_id_is_valid(id) || sig_id > (uint32_t)IOTDATA_CONTENT_ID_MAX || (sig_id != (uint32_t)IOTDATA_CONTENT_SIG_NONE && !iotdata_node_content_id_is_valid(sig_id))) {
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_ID;
    }
    /* AND IT HAS TO BE AN ID WE ARE ENTITLED TO. §4 gives the top twelve bits to the originating
       station, so describing content under another station's id is claiming to be that station --
       after which we would refuse its own updates and revocations and serve the bytes anyway. */
    if (t->station != 0u && !_iotdata_node_content_manifest_is_ours(t, id)) {
        t->stat_foreign++;
        return IOTDATA_NODE_CONTENT_ERR_STATION;
    }
    uint64_t crc = 0u;
    uint32_t len = 0u;
    const iotdata_node_content_status_t rc = iotdata_node_content_checksum(c, id, &crc, &len);
    if (rc != IOTDATA_NODE_CONTENT_OK) {
        t->stat_refused++;
        return rc;
    }
    if (len > (uint32_t)IOTDATA_CONTENT_SIZE_MAX) {
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_SIZE;
    }
    /* THE GENERATION CARRIES OVER from whatever we said last, so re-authoring an offer -- a new
       name, a longer expiry -- does not reset the counter that is there to make each origination
       differ from the last. */
    const iotdata_node_content_manifest_t *const was = iotdata_node_content_manifest_find(t, id);
    iotdata_node_content_manifest_t in = {
        .id = id,
        .size = len,
        .hash = crc,
        .sig_id = sig_id,
        .expiry_m = expiry_m,
        .generation = (was != NULL) ? was->generation : 0u,
        /* AND SO DOES WHEN WE LAST SAID IT, or re-describing an offer would reset the floor and a
           loop that re-authors could originate as fast as it liked. */
        .advertised_ms = (was != NULL) ? was->advertised_ms : 0u,
        .encoding = encoding,
        .local = true,
    };
    if (name != NULL)
        _iotdata_node_content_manifest_name(in.name, name);
    /* A row we already own would be refused by apply()'s own-offer rule, which is about hearing it
       back off the air rather than about re-authoring it. Clearing it first makes the re-author the
       ordinary path it is. */
    iotdata_node_content_manifest_t *const slot = _iotdata_node_content_manifest_slot(t, id);
    if (slot != NULL && slot->local)
        *slot = (iotdata_node_content_manifest_t){ 0 };
    const iotdata_node_content_status_t arc = _iotdata_node_content_manifest_apply(t, c, &in, now_ms);
    /* AND IT SURVIVES A RESTART. The description is the one thing here that cannot be re-derived
       from the bytes, so it goes beside them -- see THE SIDECAR, below. */
    if (arc == IOTDATA_NODE_CONTENT_OK)
        _iotdata_node_content_manifest_save(c, iotdata_node_content_manifest_find(t, id));
    return arc;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_status_t iotdata_node_content_manifest_on_wire(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint8_t *const val, const size_t vlen, const uint32_t now_ms,
                                                                                  uint32_t *const id_out) {
    if (id_out != NULL)
        *id_out = 0u;
    iotdata_content_manifest_t w;
    if (!iotdata_content_manifest_parse(val, vlen, &w)) {
        if (t != NULL)
            t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    }
    if (id_out != NULL)
        *id_out = w.id;
    iotdata_node_content_manifest_t in = {
        .id = w.id,
        .size = w.size,
        .hash = w.hash,
        .sig_id = w.sig_id,
        .expiry_m = w.expiry_m,
        .generation = w.generation,
        .encoding = w.encoding,
        .local = false,
    };
    /* The wire name points into the frame; a row has to own its own copy, truncated to what a row
       can hold. */
    _iotdata_node_content_manifest_name(in.name, (w.name != NULL) ? w.name : "");
    return _iotdata_node_content_manifest_apply(t, c, &in, now_ms);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* WITHDRAWING OUR OWN OFFER AND DESTROYING WHAT IS BEHIND IT, which is the one operation that
   does both. It goes through apply() like everything else, with local set -- which is also what
   gets it past the rule that nothing off the air may touch an offer of ours. */
static inline iotdata_node_content_status_t iotdata_node_content_manifest_revoke(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t id, const uint32_t now_ms) {
    if (t == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    if (!iotdata_node_content_id_is_valid(id)) {
        t->stat_refused++;
        return IOTDATA_NODE_CONTENT_ERR_ID;
    }
    const iotdata_node_content_manifest_t in = { .id = id, .size = 0u, .expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_REVOKED, .local = true };
    return _iotdata_node_content_manifest_apply(t, c, &in, now_ms);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline iotdata_node_content_status_t iotdata_node_content_manifest_verify(const iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t id) {
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(t, id);
    if (m == NULL)
        return IOTDATA_NODE_CONTENT_ERR_STATE;
    uint64_t crc = 0u;
    uint32_t len = 0u;
    const iotdata_node_content_status_t rc = iotdata_node_content_checksum(c, id, &crc, &len);
    if (rc == IOTDATA_NODE_CONTENT_ERR_ABSENT)
        return rc;
    /* SIZE BEFORE CHECKSUM, and the order is the whole point: a fragment sums to something, and
       reporting that as a checksum failure would tell a node to throw away a transfer that is
       merely unfinished. checksum() already distinguishes the two; this adds the manifest's own
       size to the comparison, since the store's idea of `total` came from whoever created the file
       and need not be the offer's. */
    if (rc != IOTDATA_NODE_CONTENT_OK || len != m->size)
        return IOTDATA_NODE_CONTENT_ERR_SIZE;
    return (crc == m->hash) ? IOTDATA_NODE_CONTENT_OK : IOTDATA_NODE_CONTENT_ERR_CHECKSUM;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline size_t iotdata_node_content_manifest_advertise(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t id, uint8_t *const buf, const size_t size, const uint32_t now_ms) {
    if (t == NULL || buf == NULL)
        return 0u;
    iotdata_node_content_manifest_t *const row = _iotdata_node_content_manifest_slot(t, id);
    if (row == NULL || row->id == 0u || !row->local)
        return 0u;
    /* THE FLOOR, and it is not airtime: four bits of generation exhaust in sixteen originations,
     * and the seventeenth reuses a value a relay may still be holding -- after which the offer is
     * deduped away. `advertised_ms` of 0 is "never", so the first origination after an author or a
     * restart is always allowed; what is refused is coming round again too fast.
     *
     * COMPILED OUT WHEN THE FLOOR IS ZERO, which is how a build says it does not want one -- a
     * bench re-originates several times a second to test reach. Written as #if rather than a
     * runtime 0 because an unsigned `< 0` is a warning, and the warning is right. */
#if IOTDATA_NODE_CONTENT_READVERTISE_MIN_S > 0u
    if (row->advertised_ms != 0u) {
        const int32_t since_ms = (int32_t)(now_ms - row->advertised_ms);
        if (since_ms >= 0 && (uint32_t)since_ms < (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_MIN_S * 1000u) {
            t->stat_tooSoon++;
            return 0u;
        }
    }
#endif
    /*
     * THE BYTES CAN BE REPLACED UNDER AN OFFER, and until this check they were: a cable insert over
     * an id that was already offered left the row describing the OLD object's size and hash, and
     * the node went on advertising that description of bytes it no longer holds. Every requester
     * would fetch, check the advertised hash against what arrived, and throw the transfer away --
     * a failure that looks like a radio problem and is not one.
     *
     * DROPPED, NOT QUIETLY CORRECTED. Re-deriving the size and hash here would be easy and would be
     * wrong: the NAME cannot be re-derived, and carrying the old one over is exactly the mistake
     * d_module_filestore.h forbids where it makes create() clear the sidecar -- "calling a bundle
     * somebody just dropped there by the old one's name". Whoever replaces the bytes describes them
     * again. This is that rule, one layer up, for the copy of the description that lives in RAM.
     *
     * ERR_STATE is not a mismatch and cannot happen here: it means verify() had no row to compare,
     * and we are holding the row.
     */
    const iotdata_node_content_status_t fresh = iotdata_node_content_manifest_verify(t, c, id);
    if (fresh == IOTDATA_NODE_CONTENT_ERR_ABSENT || fresh == IOTDATA_NODE_CONTENT_ERR_SIZE || fresh == IOTDATA_NODE_CONTENT_ERR_CHECKSUM) {
        _iotdata_node_content_manifest_forget(c, id);
        *row = (iotdata_node_content_manifest_t){ 0 };
        t->stat_stale++;
        return 0u;
    }
    const uint8_t next = (uint8_t)((row->generation + 1u) & (uint8_t)IOTDATA_CONTENT_TAG_MAX);
    const size_t n = _iotdata_node_content_manifest_wire(row, next, buf, size);
    if (n == 0u)
        return 0u;
    /* Only once the frame is actually built: a bump spent on a manifest that would not fit the
       buffer would make the next one look like a refresh of something nobody ever heard. */
    row->generation = next;
    row->stated_ms = now_ms;      /* we are re-stating the offer, so the offset runs from now */
    row->advertised_ms = now_ms;  /* and the floor runs from here, not from when it was described */
    if (row->advertised_ms == 0u) /* a clock of exactly 0 would read as "never originated" */
        row->advertised_ms = 1u;
    t->stat_advertised++;
    /* AND THE SIDECAR FOLLOWS THE BUMP, which is the one thing about an offer that must survive a
       restart exactly: a table starting from zero would re-use a generation relays still remember,
       and a byte-identical advertisement is thrown by every one of them. One store write per
       origination, and originations are hours apart (§3: "a few frames a week"). */
    _iotdata_node_content_manifest_save(c, row);
    return n;
}

static inline size_t iotdata_node_content_manifest_advertise_revoke(const uint32_t id, uint8_t *const buf, const size_t size) {
    const iotdata_content_manifest_t w = { .id = id, .size = 0u, .expiry_m = (uint16_t)IOTDATA_CONTENT_EXPIRY_REVOKED };
    return iotdata_content_manifest_pack(buf, size, &w);
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline int iotdata_node_content_manifests_restore(iotdata_node_content_manifests_t *const t, iotdata_node_content_t *const c, const uint32_t now_ms) {
    if (t == NULL || c == NULL || c->backend == NULL || c->backend->meta_read == NULL)
        return 0;
    int back = 0;
    uint32_t cursor = 0u, id = 0u, total = 0u, held = 0u;
    const char *owner = NULL;
    /* A COPY OF THE IDS FIRST, for begin()'s reason: author() writes to the store, and a backend's
       iterate() cannot be expected to survive that mid-walk. */
    uint32_t ids[IOTDATA_NODE_CONTENT_TABLE_MAX];
    uint8_t n = 0;
    while (n < (uint8_t)IOTDATA_NODE_CONTENT_TABLE_MAX && iotdata_node_content_iterate(c, &cursor, &id, &total, &held, &owner))
        if (!iotdata_node_content_id_is_scratch(id))
            ids[n++] = id;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t buf[D_FILESTORE_META_MAX];
        const int got = c->backend->meta_read(c->backend->ctx, ids[i], buf, sizeof(buf));
        if (got <= 0)
            continue; /* no sidecar: content nobody has described, which is the common case */
        iotdata_content_manifest_t w;
        if (!iotdata_content_manifest_parse(buf, (size_t)got, &w) || w.id != ids[i])
            continue; /* not one of ours, or for somebody else's id: leave it alone */
        /* THE GENERATION CARRIES OVER AND THE REST IS RE-DERIVED. author() re-reads the size and
           the hash from the store, so passing the stored ones would be pointless -- and passing
           them would be the bug this is written to avoid. */
        iotdata_node_content_manifest_t *const slot = _iotdata_node_content_manifest_slot(t, ids[i]);
        if (slot != NULL)
            continue; /* already known -- a heard offer arrived first; do not overwrite it */
        if (iotdata_node_content_manifest_author(t, c, ids[i], w.name, w.encoding, w.expiry_m, w.sig_id, now_ms) != IOTDATA_NODE_CONTENT_OK)
            continue;
        iotdata_node_content_manifest_t *const row = _iotdata_node_content_manifest_slot(t, ids[i]);
        if (row != NULL)
            row->generation = w.generation;
        back++;
    }
    return back;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * WHICH OF OUR OWN OFFERS IS DUE TO BE SAID AGAIN, or 0 if none is. The caller originates it --
 * only the layer holding the originate callback can -- and this is the half that can be decided
 * from the table alone.
 *
 * ONE AT A TIME, OLDEST FIRST, because the alternative is a burst: a node with four offers coming
 * up at once would put four frames on the air back to back, which at 2.4 kbps is three seconds of
 * self-collision. The caller ticks, gets one, and comes back.
 *
 * A ROW THAT HAS NEVER BEEN ORIGINATED IS DUE IMMEDIATELY. That is what makes an offer restored
 * from a sidecar announce itself after a restart without anybody asking, which is the whole point
 * of persisting it.
 */
static inline uint32_t iotdata_node_content_manifest_due(const iotdata_node_content_manifests_t *const t, const uint32_t now_ms) {
    if (t == NULL || (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_S == 0u)
        return 0u;
    uint32_t id = 0u, oldest_ms = 0u;
    for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++) {
        const iotdata_node_content_manifest_t *const m = &t->m[i];
        if (m->id == 0u || !m->local)
            continue; /* only the advertiser originates (§3), so only our own rows are ever due */
        if (m->advertised_ms != 0u) {
            const int32_t since_ms = (int32_t)(now_ms - m->advertised_ms);
            if (since_ms < 0 || (uint32_t)since_ms < (uint32_t)IOTDATA_NODE_CONTENT_READVERTISE_S * 1000u)
                continue;
            if (id != 0u && (uint32_t)since_ms <= oldest_ms)
                continue;
            oldest_ms = (uint32_t)since_ms;
        } else {
            /* never originated: due now, and ahead of anything merely overdue */
            if (id != 0u && oldest_ms == 0xFFFFFFFFu)
                continue;
            oldest_ms = 0xFFFFFFFFu;
        }
        id = m->id;
    }
    return id;
}

/*
 * EXPIRY, AND WHAT IT MEANS DEPENDS ON WHOSE OFFER IT IS.
 *
 * A HEARD offer expiring is the ordinary case: we stop believing a statement somebody else made,
 * and §3 puts a clock on it precisely so a cache cannot keep an offer alive by remembering it. The
 * bytes stay -- we may well have fetched them, and nobody asked us to throw them away.
 *
 * OUR OWN offer expiring is a WITHDRAWAL, and the operator meant it: "offer this for ten minutes"
 * is not advice to recipients, it is an instruction to us. So the description goes from the
 * sidecar too, or the next boot restores the very offer that just lapsed -- which is what used to
 * happen, making an offer's life depend on whether anybody power-cycled the node. That is the
 * whole difference from a revocation, which also destroys the bytes and tells everybody:
 *
 *     revoke   withdraw + destroy the bytes + announce it
 *     expire   withdraw + keep the bytes + say nothing
 *
 * The bytes surviving both is deliberate: what we HOLD and what we OFFER are different questions,
 * and only revoke answers the first one.
 */
static inline int iotdata_node_content_manifests_tick(iotdata_node_content_manifests_t *const t, const iotdata_node_content_t *const c, const uint32_t now_ms) {
    int gone = 0;
    if (t == NULL)
        return 0;
    for (uint8_t i = 0; i < (uint8_t)IOTDATA_NODE_CONTENT_MANIFESTS_MAX; i++)
        if (t->m[i].id != 0u && iotdata_node_content_manifest_remaining_m(&t->m[i], now_ms) == 0u) {
            if (t->m[i].local)
                _iotdata_node_content_manifest_forget(c, t->m[i].id);
            t->m[i] = (iotdata_node_content_manifest_t){ 0 };
            t->stat_expired++;
            gone++;
        }
    return gone;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/* THE WORDS, IN ONE PLACE, so four applications report an offer the same way -- and the caller
   supplies only the level, because only it knows whether an offer is worth a line on its box. */
static inline const char *iotdata_node_content_manifest_str(char *const out, const size_t size, const iotdata_node_content_manifest_t *const m, const uint32_t now_ms) {
    if (out == NULL || size == 0u)
        return "";
    if (m == NULL || m->id == 0u)
        return snprintf_inline(out, size, "no offer");
    const uint16_t left = iotdata_node_content_manifest_remaining_m(m, now_ms);
    char expiry[16];
    if (left == (uint16_t)IOTDATA_CONTENT_EXPIRY_INDEFINITE)
        (void)snprintf(expiry, sizeof(expiry), "indefinite");
    else
        (void)snprintf(expiry, sizeof(expiry), "%" PRIu16 "m", left);
    return snprintf_inline(out, size, "%06" PRIX32 " %" PRIu32 " bytes, %" PRIu32 " chunks, crc64 0x%016" PRIX64 ", enc %02" PRIX8 ", sig %06" PRIX32 ", gen %" PRIu8 ", %s, %s%s%s", m->id, m->size, iotdata_content_chunks_for(m->size),
                           m->hash, m->encoding, m->sig_id, m->generation, expiry, m->local ? "ours" : "heard", (m->name[0] != '\0') ? ", " : "", m->name);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// THE STATUS TABLE -- what this node has, offered or held, as 13-byte rows
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// ONE TABLE OVER TWO SOURCES, because "what do you know about content" is one question. A row comes
// from the store, from the offers, or from both:
//
//   held, no offer      a file put there by cable or baked in. `chunks total` is 0, which is what
//                       the field documents 0 to mean, and is the honest answer: nothing has told
//                       this node how big that object is supposed to be.
//   offer, nothing held just heard about. The row is what a manager reads to see an advertisement
//                       landed -- which is the whole of the first bring-up step.
//   both                the ordinary case.
//
// IN ID ORDER, so the index a request asks for means the same thing twice running. The ids are
// gathered and sorted on each call rather than kept sorted, because the table is tiny and a
// rank-by-comparison walk is what the other tables in this tree already do.
// -----------------------------------------------------------------------------------------------------------------------------------------

static inline uint8_t _iotdata_node_content_table_gather(iotdata_node_content_t *const c, const iotdata_node_content_manifests_t *const t, uint32_t *const ids, const uint8_t cap) {
    uint8_t n = 0;
    uint32_t cursor = 0u, id = 0u, total = 0u, held = 0u;
    const char *owner = NULL;
    while (n < cap && iotdata_node_content_iterate(c, &cursor, &id, &total, &held, &owner))
        if (!iotdata_node_content_id_is_scratch(id)) /* a half-built file is nobody's business */
            ids[n++] = id;
    /* AND THE ID BEING TAKEN RIGHT NOW, which the walk above cannot see: an insert stages into a
       scratch file and renames on proof, so the store knows nothing of the real id until the
       checksum has agreed. Without this, a transfer with no manifest behind it -- a console
       insert -- is invisible in the one table that is supposed to say what a node is doing. */
    uint32_t rx_id = 0u;
    iotdata_node_content_insert_progress(c, &rx_id, NULL, NULL);
    if (n < cap && iotdata_node_content_insert_active(c) && iotdata_node_content_id_is_valid(rx_id)) {
        bool seen = false;
        for (uint8_t i = 0; i < n; i++)
            if (ids[i] == rx_id)
                seen = true;
        if (!seen)
            ids[n++] = rx_id;
    }
    if (t != NULL) {
        uint8_t mc = 0;
        const iotdata_node_content_manifest_t *m = NULL;
        while (n < cap && iotdata_node_content_manifest_iterate(t, &mc, &m)) {
            bool seen = false;
            for (uint8_t i = 0; i < n; i++)
                if (ids[i] == m->id)
                    seen = true;
            if (!seen)
                ids[n++] = m->id;
        }
    }
    /* Insertion sort: n is at most a couple of dozen and this is read by a person or a manager, not
       by anything in a hurry. */
    for (uint8_t i = 1; i < n; i++) {
        const uint32_t v = ids[i];
        uint8_t j = i;
        while (j > 0 && ids[j - 1] > v) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = v;
    }
    return n;
}

static inline uint8_t iotdata_node_content_table_count(iotdata_node_content_t *const c, const iotdata_node_content_manifests_t *const t) {
    uint32_t ids[IOTDATA_NODE_CONTENT_TABLE_MAX];
    return _iotdata_node_content_table_gather(c, t, ids, (uint8_t)IOTDATA_NODE_CONTENT_TABLE_MAX);
}

static inline bool iotdata_node_content_table_row(iotdata_node_content_t *const c, const iotdata_node_content_manifests_t *const t, const uint8_t index, uint8_t *const row, const uint32_t now_ms) {
    uint32_t ids[IOTDATA_NODE_CONTENT_TABLE_MAX];
    const uint8_t n = _iotdata_node_content_table_gather(c, t, ids, (uint8_t)IOTDATA_NODE_CONTENT_TABLE_MAX);
    if (row == NULL || index >= n)
        return false;
    const uint32_t id = ids[index];
    const iotdata_node_content_manifest_t *const m = iotdata_node_content_manifest_find(t, id);

    uint32_t total = 0u, held = 0u;
    const bool holds = iotdata_node_content_stat(c, id, &total, &held);

    uint32_t rx_id = 0u, rx_have = 0u, rx_want = 0u;
    iotdata_node_content_insert_progress(c, &rx_id, &rx_have, &rx_want);
    const bool fetching = iotdata_node_content_insert_active(c) && rx_id == id;

    /* WHILE A TRANSFER RUNS, THE BYTES ARE IN A SCRATCH FILE and not under this id: an insert
       stages and renames on proof, precisely so that a file at a real id is always one that passed
       its checksum. So the store reports nothing held for the id being fetched, and the progress
       has to be asked of the insert instead. Reading the store here reported 0 of 14 chunks all
       the way through a transfer -- which is the one number anybody watches to see it moving. */
    const uint32_t bytes_held = fetching ? rx_have : (holds ? held : 0u);
    const uint32_t bytes_total = fetching ? rx_want : (holds ? total : 0u);

    /* WHOLE CHUNKS, except for the last one of a complete object, which is short whenever the size
       is not a multiple of 32 -- the same rule the store's own have() applies, so a count here and
       a have() there cannot disagree about whether a chunk is present. */
    const uint32_t chunks_held = (bytes_total > 0u && bytes_held == bytes_total) ? iotdata_content_chunks_for(bytes_total) : (bytes_held / (uint32_t)IOTDATA_CONTENT_CHUNK_BYTES);
    /* STRICTLY THE OFFER'S, because 0 in this field is defined as "no manifest seen" and the
       insert's own declared size is not an offer -- a console upload in flight reads as 0 of 0,
       which is the truth: nothing advertised it. */
    const uint32_t chunks_total = (m != NULL) ? iotdata_content_chunks_for(m->size) : 0u;

    uint8_t state = (uint8_t)IOTDATA_NODE_STATUS_CONTENT_STATE_CACHING;
    if (fetching)
        state = (uint8_t)IOTDATA_NODE_STATUS_CONTENT_STATE_FETCHING;
    else if (holds && total > 0u && held == total)
        state = (uint8_t)IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING;

    /* SINCE THE LAST THING THAT MOVED, which is what makes a stall visible: a transfer's last
       write, or failing that the moment the offer was stated. A settled file has nothing to be
       stale about and reports 0. */
    uint32_t since_s = 0u;
    {
        const int32_t since_ms = fetching ? (int32_t)(now_ms - c->rx.last_ms) : (m != NULL && state != (uint8_t)IOTDATA_NODE_STATUS_CONTENT_STATE_HOLDING) ? (int32_t)(now_ms - m->stated_ms) : 0;
        since_s = (since_ms <= 0) ? 0u : ((uint32_t)since_ms / 1000u); /* signed, for remaining_m's reason */
    }

    iotdata_node_entry_put_u24(row, 0, id);
    iotdata_node_entry_put_u8(row, 3, state);
    iotdata_node_entry_put_u8(row, 4, (uint8_t)((t != NULL && t->persistent) ? IOTDATA_NODE_STATUS_CONTENT_FLAG_PERSISTENT : 0));
    iotdata_node_entry_put_u16(row, 5, (chunks_held > 0xFFFFu) ? 0xFFFFu : (uint16_t)chunks_held);
    iotdata_node_entry_put_u16(row, 7, (chunks_total > 0xFFFFu) ? 0xFFFFu : (uint16_t)chunks_total);
    iotdata_node_entry_put_u16(row, 9, (m != NULL) ? iotdata_node_content_manifest_remaining_m(m, now_ms) : 0u);
    iotdata_node_entry_put_u16(row, 11, (since_s > 0xFFFFu) ? 0xFFFFu : (uint16_t)since_s);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#endif /* IOTDATA_NODE_CONTENT_MANIFEST_H */
