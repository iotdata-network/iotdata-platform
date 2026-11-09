
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_runtime.c - host tests for iotdata_node_runtime.h, the running state behind the node
// protocol: identity and sequence, the receive window, the report builders, frame dispatch.
//
// It is tested here rather than in a project because it belongs to no one project -- every role
// uses it, a sensor and a simulator as much as a relay and a gateway -- and most of those are
// ESP-IDF applications a host toolchain cannot build. Without this the shared header has no
// coverage at all and a signature change is discovered by flashing.
//
// What is pinned down: STATUS scoping (including the case that a node in no mesh answers the mesh
// scope EMPTY rather than falling back to the node group), CONTROL advertising exactly what the
// application implements, the application control hook, and the always-listening receive mode.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "iotdata_config.h"
#include "iotdata_variant.h"
#include "iotdata.h"
#include "iotdata_node.h"
#include "device/d_format.h" /* snprintf_inline, used by the host branch of the version header */
#include "iotdata_node_version.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"
#include "iotdata_node_partial.h"
#include "iotdata_node_variant.h"
#include "device/d_module_datastore_linux.h"
#include "iotdata_node_state.h"
#include "iotdata_node_settings.h"
#include "iotdata_node_firmware.h" /* runtime.h resolves NODE_ROLLBACK through it */
#include "iotdata_node_runtime.h"

/* --- stubs standing in for an application ------------------------------------------------- */

static void st_cb(uint16_t s, iotdata_node_status_t *o) {
    (void)s;
    o->uptime_s = 99;
}
static bool tx_cb(const uint8_t *p, size_t n) {
    (void)p;
    (void)n;
    return true;
}
/* A TABLE AN END DEVICE KEEPS. Scripted rather than real: what is under test is that the shared
   builder asks, pages and packs -- the content module's own rows have their own suite. */
static uint8_t tbl_rows = 0;
static uint8_t tbl_count_cb(const uint8_t entry) {
    return (entry == IOTDATA_NODE_STATUS_CONTENT_ENTRY) ? tbl_rows : 0u;
}
static bool tbl_row_cb(const uint8_t entry, const uint8_t index, uint8_t *const row) {
    if (entry != IOTDATA_NODE_STATUS_CONTENT_ENTRY || index >= tbl_rows)
        return false;
    iotdata_node_entry_put_u24(row, 0, 0x001000u + index); /* the id is enough to tell rows apart */
    return true;
}

/* How many entries of `key` a report carries. */
static int count_key(const uint8_t *const kv, const uint8_t len, const uint8_t key) {
    size_t cur = 0;
    uint8_t k, vlen;
    const uint8_t *val;
    int n = 0;
    while (iotdata_kvr_next(kv, len, &cur, &k, &val, &vlen))
        if (k == key)
            n++;
    return n;
}
/* a node that senses AND relays fills the mesh group in the same struct: no second callback */
static void both_cb(uint16_t s, iotdata_node_status_t *o) {
    st_cb(s, o);
    o->mesh.present = true;
    o->mesh.state = IOTDATA_NODE_STATUS_MESH_STATE_JOINED;
    o->mesh.cost = 3;
}
static uint8_t seen_key = 0;
static uint32_t seen_now_ms = 0xFFFFFFFFu;
static iotdata_node_control_result_t ctl_cb(uint16_t s, uint8_t subject, uint8_t action, const uint8_t *v, uint8_t n, uint32_t now_ms, const char **reason) {
    (void)s;
    (void)v;
    (void)n;
    seen_now_ms = now_ms; /* recorded unconditionally: what a handler is handed is the point */
    if (subject != IOTDATA_NODE_SUBJECT_MESH)
        return IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
    if (action == IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR) {
        seen_key = action;
        return IOTDATA_NODE_CONTROL_RESULT_DONE;
    }
    /* A VERB THIS APP HAS AND WILL NOT DO, which is the third answer the seam exists for: it is
       neither serviced nor unheard-of, and before the enum both of those were the only things it
       could say. */
    if (action == IOTDATA_NODE_ACTION_MESH_PEERS_DUMP) {
        *reason = "the mesh is not up yet";
        return IOTDATA_NODE_CONTROL_RESULT_REFUSED;
    }
    return IOTDATA_NODE_CONTROL_RESULT_UNKNOWN;
}
/* (subject, action) pairs, which is the shape the inventory and the wire both use */
static const uint8_t keys[] = { IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR };
static size_t diag_cb(size_t *cursor, char *out, size_t outsize) { /* two records, then done */
    if (*cursor >= 2)
        return 0;
    const int n = snprintf(out, outsize, "rec%u", (unsigned)(*cursor)++);
    return (n < 0) ? 0 : (size_t)n;
}
/* what a sensor declares: the rest of VERSION is detected, so there is nothing else to stub */
static iotdata_node_version_caps_t caps;

static int count_group(const uint8_t *raw, uint8_t rlen, int mesh) {
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    int n = 0;
    while (iotdata_kvr_next(raw, rlen, &cur, &key, &val, &vlen))
        /* the mesh half is 0x20..0x7F: its scalars and its table */
        if ((key >= 0x20 && key < 0x80) == (mesh != 0))
            n++;
    return n;
}
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s\n", m); \
            fails++; \
        } else \
            printf("  ok: %s\n", m); \
    } while (0)

int main(void) {
    printf("iotdata_node_endpoint: status scoping, control advertisement, app hook, always-on\n\n");
    int fails = 0;
    iotdata_node_t n;
    uint8_t buf[IOTDATA_MAX_PACKET_SIZE];
    int len;

    /* a node that senses AND relays: both groups, scopable */
    const iotdata_node_params_t both = { .caps = &caps, .status = both_cb, .tx = tx_cb, .control = ctl_cb, .control_actions = keys, .control_actions_count = 1, .receive_always = true };
    iotdata_node_init(&n, 0x0537, NULL, 0u);
    iotdata_node_attach(&n, &both);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), 0, NULL);
    CHECK(len > 0 && count_group(buf, (uint8_t)len, 0) > 0 && count_group(buf, (uint8_t)len, 1) > 0, "no scope = both groups");
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_MESH, NULL);
    CHECK(len > 0 && count_group(buf, (uint8_t)len, 0) == 0 && count_group(buf, (uint8_t)len, 1) > 0, "scope=mesh = mesh only");
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_NODE, NULL);
    CHECK(len > 0 && count_group(buf, (uint8_t)len, 0) > 0 && count_group(buf, (uint8_t)len, 1) == 0, "scope=node = node only");

    /* THE TABLE SEAM, which an end device had no way to answer until the content store gave it a
       table: `stat content` came back with the scalars and silence where the rows should have been.
       The relay and the gateway have had this pair all along; this is the same algorithm. */
    {
        const iotdata_node_params_t tabled = { .caps = &caps, .status = st_cb, .tx = tx_cb, .table_count = tbl_count_cb, .table_row = tbl_row_cb, .receive_always = true };
        iotdata_node_attach(&n, &tabled);
        tbl_rows = 3;
        len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_CONTENT, NULL);
        CHECK(len > 0 && count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_ENTRY) == 3, "the content scope answers with its rows");
        /* THE COUNT IS A SCALAR AND IS IN THE NODE GROUP, so asking for the rows alone does not
           carry it -- which is the caller's own choice, and the reason every scalar is emitted
           before any entry. */
        len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_NODE, NULL);
        CHECK(len > 0 && count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_COUNT) == 1, "the node group carries the count");
        CHECK(count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_ENTRY) == 0, "and not the rows");
        /* AN EMPTY TABLE STILL SAYS SO: a manager asking whether an offer landed wants "none"
           rather than silence, which is why has_content is set from having the table at all. */
        tbl_rows = 0;
        len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_NODE, NULL);
        CHECK(len > 0 && count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_COUNT) == 1, "a node with no content reports zero, not nothing");
        /* MORE ROWS THAN ONE REPORT HOLDS: paged, and the loop says so rather than stopping
           quietly, which would be a node under-reporting what it holds with nothing to show it. */
        tbl_rows = 200;
        iotdata_node_partial_t part = { 0 };
        int chunks = 0, rows = 0;
        do {
            len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_CONTENT, &part);
            CHECK(len > 0, "each page builds");
            rows += count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_ENTRY);
            chunks++;
        } while (part.more && chunks < 40);
        CHECK(chunks > 1, "two hundred rows take more than one report");
        CHECK(rows == 200, "and every row is carried exactly once");
        tbl_rows = 0;
    }

    /* a node with NO table seam says nothing at all about a table, rather than reporting zeroes
       for three it does not keep -- stations and filters belong to a node whose job is hearing
       other nodes. */
    {
        const iotdata_node_params_t untabled = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = true };
        iotdata_node_attach(&n, &untabled);
        len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_NODE, NULL);
        CHECK(len > 0 && count_key(buf, (uint8_t)len, IOTDATA_NODE_STATUS_CONTENT_COUNT) == 0, "no table, no count");
        len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_CONTENT, NULL);
        CHECK(len == 0, "and the content scope answers empty");
    }

    /* a plain end device: in no mesh, so the mesh scope yields an EMPTY status, not the node group */
    const iotdata_node_params_t plain = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = true };
    iotdata_node_attach(&n, &plain);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_STATUS, buf, sizeof(buf), IOTDATA_NODE_STATUS_SCOPE_MESH, NULL);
    CHECK(len == 0, "a sensor asked for the mesh group answers empty, not the node group");

    /* CONTROL advertises what the app implements, and only that */
    iotdata_node_attach(&n, &both);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_CONTROL, buf, sizeof(buf), 0, NULL);
    bool adv = false, adv_plain = false;
    size_t cur = 0;
    uint8_t k, vl;
    const uint8_t *v;
    /* the inventory lists (subject, action) pairs, in the same encoding a command would take */
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl))
        if (k == IOTDATA_NODE_CONTROL_CONTROL && vl == 2 && v[0] == IOTDATA_NODE_SUBJECT_MESH && v[1] == IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR)
            adv = true;
    CHECK(adv, "CONTROL advertises the app's action");
    iotdata_node_attach(&n, &plain);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_CONTROL, buf, sizeof(buf), 0, NULL);
    cur = 0;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl))
        if (k == IOTDATA_NODE_CONTROL_CONTROL && vl == 2 && v[0] == IOTDATA_NODE_SUBJECT_MESH)
            adv_plain = true;
    CHECK(!adv_plain, "a node with no hook advertises no app actions");

    /* VARIANT: a node that produces telemetry says which fields sit in which presence slot. Two
       repeated keys, ENTRY and NAMES, each carrying the variant number in its value -- and the
       DEFAULT answer is entries alone, because the names cost several times as much. */
    iotdata_node_partial_t vp = { 0 };
    iotdata_node_attach(&n, &both);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_VARIANT, buf, sizeof(buf), 0, &vp);
    CHECK(len > 0, "a variant suite was reported");
    cur = 0;
    bool v0 = false, named = false, sane = true;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl)) {
        if (k == IOTDATA_NODE_VARIANT_NAMES)
            named = true;
        if (k != IOTDATA_NODE_VARIANT_ENTRY)
            continue;
        if (iotdata_node_variant_of(v, vl) == 0)
            v0 = true;
        if (((vl - 1u) % 2u) != 0u)
            sane = false;
    }
    CHECK(v0, "variant 0 is declared, and says so inside its value");
    CHECK(!named, "names are NOT in the default answer");
    CHECK(sane, "every entry is a variant byte then whole u16 slots");
    CHECK(vp.total > 0 && vp.chunk > 0, "the partial counts the suite, not the chunk");
    /* the first slot must be the field the compiled-in definition puts there */
    const iotdata_variant_def_t *const d0 = iotdata_get_variant(0);
    cur = 0;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl))
        if (k == IOTDATA_NODE_VARIANT_ENTRY && iotdata_node_variant_of(v, vl) == 0 && d0 != NULL && vl >= 3)
            CHECK(iotdata_node_variant_field_at(v, vl, 0) == iotdata_node_variant_field_id(d0->fields[0].type), "slot 0 is the field the build put there");

    /* ask for the names and they come, still keyed per variant */
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_VARIANT, buf, sizeof(buf), IOTDATA_NODE_VARIANT_WANT_NAMES, &vp);
    CHECK(len > 0, "names reported when asked for");
    cur = 0;
    bool got_name = false, got_entry = false;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl)) {
        if (k == IOTDATA_NODE_VARIANT_ENTRY)
            got_entry = true;
        if (k == IOTDATA_NODE_VARIANT_NAMES && iotdata_node_variant_of(v, vl) == 0 && iotdata_node_variant_name_at(v, vl, 0) != NULL)
            got_name = true;
    }
    CHECK(got_name, "variant 0's own name is index 0 of its NAMES value");
    CHECK(!got_entry, "and asking for names alone does not also send the entries");

    /* SETTINGS: the protocol's own values, read and written as one TLV. A node with no settings
       block answers empty; one with a block reports what it holds. */
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_SETTINGS, buf, sizeof(buf), 0, NULL);
    CHECK(len == 0, "no settings block: an empty report, which is still a report");

    static iotdata_node_settings_t settings;
    iotdata_node_settings_defaults(&settings, 0x0111);
    const iotdata_node_params_t cfgset = { .caps = &caps, .status = st_cb, .tx = tx_cb, .settings = &settings, .receive_always = true };
    iotdata_node_attach(&n, &cfgset);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_SETTINGS, buf, sizeof(buf), 0, NULL);
    CHECK(len > 0, "a settings block reports");
    cur = 0;
    bool saw_station = false, saw_receive = false;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl)) {
        if (k == IOTDATA_NODE_SETTINGS_STATION && vl == 2 && iotdata_node_settings_get_u16(v, 0) == 0x0111)
            saw_station = true;
        if (k == IOTDATA_NODE_SETTINGS_RECEIVE && vl == IOTDATA_NODE_SETTINGS_RECEIVE_SIZE)
            saw_receive = true;
    }
    CHECK(saw_station, "an unwritten station reads as the one the hardware derived, not as a sentinel");
    CHECK(saw_receive, "and the receive triple is always the triple");

    /* a write assigns, and the answer is what is NOW TRUE -- which is the whole error channel */
    uint8_t wbuf[64];
    iotdata_kvr_t wk;
    iotdata_kvr_init(&wk, wbuf, sizeof(wbuf));
    iotdata_kvr_add_u16(&wk, IOTDATA_NODE_SETTINGS_STATION, 0x0537);
    uint8_t rep[IOTDATA_NODE_SETTINGS_REPORT_SIZE] = { IOTDATA_NODE_TLV_STATUS };
    iotdata_node_settings_put_u16(rep, 1, IOTDATA_NODE_REPORT_ON_PERIOD);
    iotdata_node_settings_put_u16(rep, 3, 600);
    iotdata_kvr_add(&wk, IOTDATA_NODE_SETTINGS_REPORT, rep, (uint8_t)sizeof(rep));
    CHECK(iotdata_node_settings_apply(&settings, wbuf, wk.len), "the write took");
    CHECK(settings.station == 0x0537, "station assigned");
    const iotdata_node_settings_report_t *const got = iotdata_node_settings_report_find(&settings, IOTDATA_NODE_TLV_STATUS);
    CHECK(got != NULL && got->period_s == 600 && (got->flags & IOTDATA_NODE_REPORT_ON_PERIOD) != 0, "and the schedule with it");
    CHECK(!iotdata_node_settings_apply(&settings, wbuf, wk.len), "the same write again changes nothing");

    /* a station outside the usable range does NOT take: 0 is unassignable and MAX is broadcast */
    iotdata_kvr_init(&wk, wbuf, sizeof(wbuf));
    iotdata_kvr_add_u16(&wk, IOTDATA_NODE_SETTINGS_STATION, IOTDATA_STATION_MAX);
    CHECK(!iotdata_node_settings_apply(&settings, wbuf, wk.len), "broadcast is not a station");
    CHECK(settings.station == 0x0537, "and the old value stands, which the read-back reports");

    /* a subject that cannot be reported cannot be scheduled either */
    iotdata_node_settings_report_t bad = { .subject = IOTDATA_NODE_TLV_DISCRIMINATOR, .flags = IOTDATA_NODE_REPORT_ON_PERIOD };
    CHECK(!iotdata_node_settings_report_set(&settings, &bad), "a modifier has nothing to report");
    bad.subject = 0x2A; /* a vendor's own type: schedulable with no coordination at all */
    CHECK(iotdata_node_settings_report_set(&settings, &bad), "a proprietary subject is schedulable");

    /* A written schedule OVERRIDES the node's default; an absent entry DEFERS to it. That is the
       whole composition rule, and it is why every reader takes the default as a parameter. */
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_STATUS) == 600, "a written period wins");
    CHECK(iotdata_node_settings_period_s(NULL, IOTDATA_NODE_TLV_STATUS) == 0, "and a node with no block at all schedules nothing");
    /* a stated entry with ON_PERIOD clear says NEVER, which is an answer and not an absence */
    iotdata_node_settings_report_t never = { .subject = IOTDATA_NODE_TLV_VERSION, .flags = IOTDATA_NODE_REPORT_AT_STARTUP };
    CHECK(iotdata_node_settings_report_set(&settings, &never), "stated");
    CHECK(iotdata_node_settings_period_s(&settings, IOTDATA_NODE_TLV_VERSION) == 0, "ON_PERIOD clear says NEVER, and never is a value");
    CHECK(iotdata_node_settings_at_startup(&settings, IOTDATA_NODE_TLV_VERSION), "and its startup flag is honoured");

    /* the station a node comes up as: seeded from what the hardware derived, replaced by a write */
    CHECK(iotdata_node_settings_station(&settings) == 0x0537, "a written station wins");
    iotdata_node_settings_t fresh;
    iotdata_node_settings_defaults(&fresh, 0x0111);
    CHECK(iotdata_node_settings_station(&fresh) == 0x0111, "an uncommissioned node comes up as what its hardware says");

    /* and it is seeded DENSE: every reportable subject has a record, so a report never leaves the
       far end guessing which default this particular build happens to hold */
    for (uint8_t t = 0; t < IOTDATA_NODE_TLV_SYSTEM_COUNT; t++)
        if (iotdata_node_tlv_is_reportable(t))
            CHECK(iotdata_node_settings_report_find(&fresh, t) != NULL, iotdata_node_tlv_name(t));
    CHECK(iotdata_node_settings_period_s(&fresh, IOTDATA_NODE_TLV_STATUS) == IOTDATA_NODE_SETTINGS_DEFAULT_PERIOD_STATUS_S, "status keeps its heartbeat");
    CHECK(iotdata_node_settings_at_startup(&fresh, IOTDATA_NODE_TLV_VERSION), "and a node announces what it is on coming up");

    /* adopting one restarts the stream: ONE STATION ONE SEQUENCE means a new id is a new sequence */
    iotdata_node_init(&n, 0x0111, NULL, 0u);
    n.sequence = 42;
    CHECK(iotdata_node_station_adopt(&n, 0x0537), "adopted");
    CHECK(iotdata_node_station(&n) == 0x0537 && n.sequence == 0, "identity and sequence move together");
    CHECK(!iotdata_node_station_adopt(&n, 0x0537), "adopting the same one changes nothing");
    CHECK(!iotdata_node_station_adopt(&n, IOTDATA_STATION_MAX), "and broadcast is not a station");

    /* DIAGNOSTICS is advertised by every node unconditionally, so every node must answer it. A
       device with no recorder answers EMPTY -- absent would look exactly like being ignored. */
    iotdata_node_attach(&n, &plain);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_DIAGNOSTICS, buf, sizeof(buf), 0, NULL);
    CHECK(len == 0, "no recorder: an empty report, which is still a report");
    const iotdata_node_params_t recorder = { .caps = &caps, .status = st_cb, .tx = tx_cb, .diag = diag_cb, .receive_always = true };
    iotdata_node_attach(&n, &recorder);
    len = iotdata_node_build(&n, IOTDATA_NODE_TLV_DIAGNOSTICS, buf, sizeof(buf), 0, NULL);
    int records = 0;
    bool typed = false;
    cur = 0;
    while (iotdata_kvr_next(buf, (uint8_t)len, &cur, &k, &v, &vl)) {
        if (k == IOTDATA_NODE_DIAGNOSTICS_TYPE)
            typed = true;
        if (k == IOTDATA_NODE_DIAGNOSTICS_DATA)
            records++;
    }
    CHECK(typed && records == 2, "a recorder: the type, then every record it had");

    /* the hook receives an unknown key; a key it refuses is counted unknown */
    iotdata_node_init(&n, 0x0537, NULL, 0u);
    uint8_t kv[8];
    iotdata_kvr_t b;
    iotdata_kvr_init(&b, kv, sizeof(kv));
    const uint8_t mine[2] = { IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR };
    iotdata_kvr_add(&b, IOTDATA_NODE_CONTROL_CONTROL, mine, (uint8_t)sizeof(mine));
    /* a subject the app does not claim: the hook refuses it and it is counted unknown rather than
       being fatal, which is the rule that lets an older node meet a newer manager */
    const uint8_t theirs[2] = { IOTDATA_NODE_SUBJECT_MESH, 0x7F };
    iotdata_kvr_add(&b, IOTDATA_NODE_CONTROL_CONTROL, theirs, (uint8_t)sizeof(theirs));
    bool reboot = false;
    iotdata_node_attach(&n, &both);
    iotdata_node_process_control(&n, kv, b.len, 7000u, &reboot);
    CHECK(seen_key == IOTDATA_NODE_ACTION_MESH_PEERS_CLEAR, "the hook got the action it claims");
    CHECK(n.stat_commands == 1, "a claimed action is a command");
    CHECK(n.stat_unknown == 1, "an UNCLAIMED action is unknown");

    /* THE THIRD ANSWER, COUNTED APART FROM BOTH. The hook has this verb and declines it, and the
       counters have to be able to say so: as a bool it came back false, was added to stat_unknown,
       and the console then told the operator the node had been built without the feature. */
    iotdata_kvr_t b2;
    uint8_t kv2[64];
    iotdata_kvr_init(&b2, kv2, sizeof(kv2));
    const uint8_t declined[2] = { IOTDATA_NODE_SUBJECT_MESH, IOTDATA_NODE_ACTION_MESH_PEERS_DUMP };
    iotdata_kvr_add(&b2, IOTDATA_NODE_CONTROL_CONTROL, declined, (uint8_t)sizeof(declined));
    iotdata_node_process_control(&n, kv2, b2.len, 7000u, &reboot);
    CHECK(n.stat_refused == 1, "a claimed-and-declined action is refused");
    CHECK(n.stat_commands == 1, "and is NOT counted as serviced");
    CHECK(n.stat_unknown == 1, "nor as unknown -- that was the misreport");
    CHECK(n.refusal != NULL && strcmp(n.refusal, "the mesh is not up yet") == 0, "and it carries the condition that caused it");
    CHECK(strcmp(iotdata_node_control_result_name(IOTDATA_NODE_CONTROL_RESULT_REFUSED), "refused") == 0, "which an operator reads as refused, not failed");

    /* A ROLLBACK WITH NOWHERE TO GO IS A REFUSAL TOO. No host build has a fallback slot, so
       firmware_rollback_possible() is false here -- and this used to land in stat_unknown, which is
       how `node rollback` came to read as a firmware built without the verb. */
    const uint32_t was_refused = n.stat_refused, was_unknown = n.stat_unknown;
    iotdata_kvr_init(&b2, kv2, sizeof(kv2));
    const uint8_t roll[2] = { IOTDATA_NODE_SUBJECT_NODE, IOTDATA_NODE_ACTION_NODE_ROLLBACK };
    iotdata_kvr_add(&b2, IOTDATA_NODE_CONTROL_CONTROL, roll, (uint8_t)sizeof(roll));
    iotdata_node_process_control(&n, kv2, b2.len, 7000u, &reboot);
    CHECK(n.stat_refused == was_refused + 1, "rollback with no fallback slot is refused");
    CHECK(n.stat_unknown == was_unknown, "and not called unimplemented");

    /* always listening: nothing is ever scheduled or advertised (a DOWN frame is transmitted
       before it is held, so such a node hears the immediate copy), yet the receiver counts as
       open at every instant */
    iotdata_node_init(&n, 0x0537, NULL, 0u);
    iotdata_node_attach(&n, &both);
    CHECK(!iotdata_node_window_advance(&n, 10u * 60u * 1000u), "an always-on node never becomes due");
    CHECK(!iotdata_node_window_pending(&n), "and so never advertises");
    CHECK(iotdata_node_window_active(&n, 123456u), "but is always active");

    /* a sleeping node still schedules and advertises normally */
    const iotdata_node_params_t sleeper = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = false, .receive_every_s = 60u, .receive_window_s = 30u };
    iotdata_node_init(&n, 0x0538, NULL, 0u);
    iotdata_node_attach(&n, &sleeper);
    CHECK(!iotdata_node_window_advance(&n, 1000u), "not due yet");
    CHECK(iotdata_node_window_advance(&n, 60000u), "due after the cadence");
    CHECK(iotdata_node_window_pending(&n), "and pending an advertisement");
    CHECK(!iotdata_node_window_active(&n, 1000u), "not active until the window opens");
    iotdata_node_window_begin(&n, 1000u);
    CHECK(iotdata_node_window_active(&n, 2000u), "active inside the window");
    CHECK(!iotdata_node_window_active(&n, 1000u + 30000u), "and closed after it");

    /* STATED IN SECONDS, COMPUTED IN MILLISECONDS: the _s forms are what the wire and the operator
       see, the _ms forms are what the schedule runs on, and the conversion lives in one named place
       so that no caller is left multiplying. */
    CHECK(iotdata_node_window_s(&n) == 30u && iotdata_node_window_ms(&n) == 30000u, "the window, said one way and used the other");
    CHECK(iotdata_node_window_every_s(&n) == 60u && iotdata_node_window_every_ms(&n) == 60000u, "and the cadence with it");
    {
        /* The settings cap does not reach the fallback: an application can put any uint32 of seconds
           there, and a uint32 of milliseconds runs out at 49.7 days. So the multiply saturates --
           a near-century cadence must not wrap round into a frantic one. */
        const iotdata_node_params_t absurd = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = false, .receive_every_s = 0xFFFFFFFFu, .receive_window_s = 30u };
        iotdata_node_attach(&n, &absurd);
        CHECK(iotdata_node_window_every_ms(&n) == 0xFFFFFFFFu, "an absurd cadence saturates");
        CHECK(!iotdata_node_window_advance(&n, 3600u * 1000u), "and is still not due an hour later");
        const iotdata_node_params_t unreachable = { .caps = &caps, .status = st_cb, .tx = tx_cb, .receive_always = false, .receive_every_s = 0u, .receive_window_s = 30u };
        iotdata_node_attach(&n, &unreachable);
        CHECK(iotdata_node_window_every_ms(&n) == 0u, "while 0 stays 0, and still means never");
    }

    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
