// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// test_node_control.c - host tests for iotdata_node_control.h.
//
// The property under test throughout is that ONE node driven TWO ways behaves the same: a console
// and an MQTT request must reach the same command, the same arguments and the same bytes. A
// vocabulary that diverges per medium is the failure this header exists to prevent, and it is not
// a failure any single-medium test can see.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iotdata_variant.h"
#include "iotdata.c"
#include "iotdata_node.h"
#include "iotdata_node_status.h"
#include "iotdata_node_control.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

static const iotdata_node_control_command_t *argv_find(iotdata_node_control_args_t *const a, int *const used, int argc, ...) {
    char *v[8];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 8; i++)
        v[i] = va_arg(ap, char *);
    va_end(ap);
    return iotdata_node_control_from_argv(argc, v, a, used);
}

static void test_vocabulary(void) {
    printf("the vocabulary: one set of words, whoever is asking\n");
    CHECK(iotdata_node_control_find("vers") != NULL, "a four-letter primary resolves");
    CHECK(iotdata_node_control_find("mesh-peers-update") != NULL, "and a mesh command");
    CHECK(iotdata_node_control_find("no-such-command") == NULL, "an unknown name does not");
    CHECK(iotdata_node_control_find(NULL) == NULL, "nor NULL");

    /* every row must name a key the key table knows, or a manager cannot decode what it built */
    for (uint8_t i = 0;; i++) {
        const iotdata_node_control_command_t *const c = iotdata_node_control_at(i);
        if (c == NULL)
            break;
        if (c->arg == IOTDATA_NODE_CONTROL_ARG_REPORTS)
            continue; /* not one key */
        if (iotdata_node_tlv_key_name(IOTDATA_NODE_TLV_CONTROL, c->key) == NULL) {
            printf("  FAIL: %s emits key 0x%02X, which is not in the key table\n", c->name, c->key);
            fails++;
        }
    }
}

static void test_app_commands(void) {
    printf("a device adds its own, and cannot take a word that is taken\n");
    static const iotdata_node_control_command_t app[] = {
        /* a TSA's own, on a proprietary SUBJECT -- which the old derived scheme could not reach */
        { "calibrate", IOTDATA_NODE_CONTROL_CONTROL, 0x80, 0x10, IOTDATA_NODE_CONTROL_ARG_NONE, 0 },
        { "boot", IOTDATA_NODE_CONTROL_CONTROL, 0x80, 0x11, IOTDATA_NODE_CONTROL_ARG_NONE, 0 }, /* redefining a built-in */
    };
    iotdata_node_control_init(app, (uint8_t)(sizeof(app) / sizeof(app[0])));

    const iotdata_node_control_command_t *const c = iotdata_node_control_find("calibrate");
    CHECK(c != NULL && c->subject == 0x80 && c->action == 0x10, "the device's own command is found");
    /* built-ins are searched FIRST: an application must not leave an operator holding a word that
       means something else on this node than on every other one */
    const iotdata_node_control_command_t *const b = iotdata_node_control_find("boot");
    CHECK(b != NULL && b->subject == IOTDATA_NODE_SUBJECT_NODE && b->action == IOTDATA_NODE_ACTION_NODE_REBOOT, "and cannot shadow `boot`");

    /* the walk covers both, so help and completion see the device's commands too */
    bool saw_app = false, saw_builtin = false;
    for (uint8_t i = 0;; i++) {
        const iotdata_node_control_command_t *const w = iotdata_node_control_at(i);
        if (w == NULL)
            break;
        if (strcmp(w->name, "calibrate") == 0)
            saw_app = true;
        if (strcmp(w->name, "vers") == 0)
            saw_builtin = true;
    }
    CHECK(saw_app && saw_builtin, "the walk yields built-ins and the device's own");
    iotdata_node_control_init(NULL, 0);
    CHECK(iotdata_node_control_find("calibrate") == NULL, "and they go when the device deregisters");
}

static void test_console(void) {
    printf("a console: argv joined, longest match first\n");
    iotdata_node_control_args_t a;
    int used = 0;

    const iotdata_node_control_command_t *c = argv_find(&a, &used, 3, "diag", "bbox", "enable");
    CHECK(c != NULL && strcmp(c->name, "diag-bbox-enable") == 0, "`diag bbox enable` is diag-bbox-enable");
    CHECK(used == 3, "three words were the name");

    /* the SAME command typed the other way -- neither spelling is a second vocabulary */
    c = argv_find(&a, &used, 1, "diag-bbox-enable");
    CHECK(c != NULL && strcmp(c->name, "diag-bbox-enable") == 0, "and so is `diag-bbox-enable`");

    /* the longest match matters: the trailing words here are ARGUMENTS, and a greedy join would
       go looking for a command called mesh-peers-update-0x537-remove */
    c = argv_find(&a, &used, 5, "mesh", "peers", "update", "0x537", "remove");
    CHECK(c != NULL && strcmp(c->name, "mesh-peers-update") == 0, "the name stops where the arguments start");
    CHECK(used == 3, "three words of name");
    CHECK(a.station == 0x537, "the station was read");
    CHECK(a.action == IOTDATA_NODE_CONTROL_MESH_PEER_NONE, "and `remove` is the action");

    c = argv_find(&a, &used, 5, "node", "filters", "update", "0xABC", "block");
    CHECK(c != NULL && a.action == IOTDATA_NODE_CONTROL_NODE_FILTERS_BLOCK, "`block` is an action");
    c = argv_find(&a, &used, 3, "node", "filters", "clear");
    CHECK(c != NULL && a.scope_filter == IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_ALL, "no scope word = every entry");
    c = argv_find(&a, &used, 4, "node", "filters", "clear", "manual");
    CHECK(c != NULL && a.scope_filter == IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_MANUAL, "and a scope word is read");
    c = argv_find(&a, &used, 2, "stat", "mesh");
    CHECK(c != NULL && a.scope_status == IOTDATA_NODE_STATUS_SCOPE_MESH, "`stat mesh` scopes the status");

    /* the trap in longest-match: `mesh peers` IS a command (the request), so a mistyped or legacy
       removal must not quietly resolve to it with the two words dropped on the floor */
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "remove", "0x537") == NULL, "words that cannot belong to the name reject it");
    CHECK(argv_find(&a, &used, 3, "diag", "enable", "please") == NULL, "an argument to a command that takes none");
    CHECK(argv_find(&a, &used, 4, "mesh", "filters", "clear", "sideways") == NULL, "a scope that is not a scope");
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "update", "everyone") == NULL, "an update with no station");
    CHECK(argv_find(&a, &used, 5, "mesh", "peers", "update", "0x537", "sideways") == NULL, "an action that is not an action");
    CHECK(argv_find(&a, &used, 4, "mesh", "peers", "update", "0x537") != NULL, "but the station alone is enough");

    /* THE SIGNATURE STREAM IS MANDATORY WHERE A PERSON TYPES IT and forgiving everywhere below --
       §3 makes it "mandatory to provide, optional to use", and this is the half that provides. A
       manifest with no stream behind it is not supposed to exist, so an offer typed without one
       does not resolve at all rather than quietly carrying 0. */
    CHECK(argv_find(&a, &used, 3, "content", "manifest", "1a2b3c") == NULL, "an offer with no signature stream is not a command");
    c = argv_find(&a, &used, 4, "content", "manifest", "1a2b3c", "1a2b3d");
    CHECK(c != NULL && a.content == 0x1a2b3cu && a.content_sig == 0x1a2b3du, "naming one resolves, id then stream");
    CHECK(c != NULL && a.content_expiry_m == IOTDATA_CONTENT_EXPIRY_INDEFINITE, "and an unsaid expiry OFFERS rather than revokes");
    /* `self` is the deliberate zero: a signature stream's own manifest names no stream, because
       hashing the hashes would recurse without end. Saying so is allowed; forgetting is not. */
    c = argv_find(&a, &used, 4, "content", "manifest", "1a2b3c", "self");
    CHECK(c != NULL && a.content_sig == IOTDATA_CONTENT_SIG_NONE, "`self` says it IS one");
    CHECK(argv_find(&a, &used, 4, "content", "manifest", "1a2b3c", "sideways") == NULL, "and a word that is neither is refused");
    c = argv_find(&a, &used, 7, "content", "manifest", "1a2b3c", "1a2b3d", "img", "1440", "11");
    CHECK(c != NULL && a.content_expiry_m == 1440u && a.content_encoding == 0x11u && strcmp(a.content_name, "img") == 0, "then name, expiry and encoding in order");
    /* the audience is optional, because an advertisement is normally for everybody */
    c = argv_find(&a, &used, 3, "content", "advertise", "1a2b3c");
    CHECK(c != NULL && a.station == 0u, "an advertisement with no audience leaves the field unsaid");
    c = argv_find(&a, &used, 4, "content", "advertise", "1a2b3c", "0x57d");
    CHECK(c != NULL && a.station == 0x57Du, "and a named one is read");

    /* A STATION IS THE WHOLE WORD OR IT IS NOT A STATION. `57d` is how every log line and every
       MQTT topic in this system spells 0x57D, and it used to parse as decimal 57 and send the
       command to a different node in silence. Decimal stays the default -- changing that would
       redirect existing scripts -- so what is refused is the ambiguous spelling. */
    CHECK(argv_find(&a, &used, 4, "node", "filters", "update", "57d") == NULL, "a bare hex station is refused, not read as 57");
    c = argv_find(&a, &used, 4, "node", "filters", "update", "0x57d");
    CHECK(c != NULL && a.station == 0x57Du, "with the 0x it is the station it looks like");
    c = argv_find(&a, &used, 4, "node", "filters", "update", "1405");
    CHECK(c != NULL && a.station == 0x57Du, "and decimal still means decimal: the same node");
    /* and a leading zero is not octal: `057` was station 47 under strtol's base 0 */
    c = argv_find(&a, &used, 4, "node", "filters", "update", "057");
    CHECK(c != NULL && a.station == 57u, "a leading zero is decimal, not octal");
    CHECK(argv_find(&a, &used, 4, "node", "filters", "update", "12abc") == NULL, "and trailing rubbish is refused rather than ignored");

    CHECK(argv_find(&a, &used, 1, "nonesuch") == NULL, "an unknown verb resolves to nothing");
    CHECK(argv_find(&a, &used, 2, "mesh", "nonesuch") == NULL, "and so does an unknown subcommand");
    /* absent target is BROADCAST, as it is over MQTT: "this command, to whoever hears it" */
    c = argv_find(&a, &used, 1, "vers");
    CHECK(c != NULL && a.target == IOTDATA_STATION_BROADCAST, "no target named means broadcast");
}

/* EVERY COMMAND IN THE TABLE MUST BUILD, into a buffer sized by IOTDATA_NODE_CONTROL_BUILD_MAX.
   That is the guard the two stale hand-counted bounds needed: a number living in an application
   cannot notice that this table grew, and both times it did not -- `* update <station> <action>`
   overflowed by two bytes, and MANIFEST by twenty. Walking the vocabulary cannot go stale. */
static void test_every_command_builds(void) {
    printf("the whole vocabulary packs into the bound it publishes\n");
    iotdata_node_control_args_t a = { 0 };
    a.target = IOTDATA_STATION_BROADCAST;
    a.station = 0x057Du;
    a.content = 0x001003u;
    a.content_sig = 0x001004u;
    a.content_expiry_m = 1440u;
    a.content_encoding = 0x11u;
    a.scope_filter = IOTDATA_NODE_CONTROL_NODE_FILTERS_SCOPE_ALL;
    /* THE WORST CASE IS THE WIDEST NAME, because that is what the bound is sized for: a name one
       byte over is the whole of what went wrong. */
    memset(a.content_name, 'z', sizeof(a.content_name) - 1u);
    a.content_name[sizeof(a.content_name) - 1u] = '\0';

    int n = 0;
    for (uint8_t i = 0;; i++) {
        const iotdata_node_control_command_t *const c = iotdata_node_control_at(i);
        if (c == NULL)
            break;
        uint8_t buf[IOTDATA_NODE_CONTROL_BUILD_MAX];
        iotdata_kvr_t kv;
        iotdata_kvr_init(&kv, buf, sizeof(buf));
        const bool ok = iotdata_node_control_build(&kv, c, &a);
        if (!ok || kv.overflow) {
            printf("  FAIL: '%s' does not build into %u bytes (len=%u overflow=%d)\n", c->name, (unsigned)IOTDATA_NODE_CONTROL_BUILD_MAX, (unsigned)kv.len, (int)kv.overflow);
            fails++;
        }
        n++;
    }
    CHECK(n > 20, "the whole table was walked, not an empty one");
    /* AND THE BOUND IS NOT SILLY: a buffer one byte short of it must actually fail for the widest
       command, or the number is bigger than it needs to be and proves nothing. */
    const iotdata_node_control_command_t *const widest = iotdata_node_control_find("content-manifest");
    CHECK(widest != NULL, "the widest command is in the table");
    if (widest != NULL) {
        uint8_t tight[IOTDATA_NODE_CONTROL_BUILD_MAX - 1u];
        iotdata_kvr_t kv;
        iotdata_kvr_init(&kv, tight, sizeof(tight));
        CHECK(!iotdata_node_control_build(&kv, widest, &a) || kv.overflow, "one byte short of the bound is not enough");
    }
}

/* THE point of the header: two media, one result. */
static void test_media_agree(void) {
    printf("two media, identical bytes\n");
#if !defined(IOTDATA_NO_JSON)
    static const struct {
        const char *json;
        int argc;
        const char *argv[8]; /* `content manifest <id> <name> <expiry> <enc> <sig>` is the longest */
    } cases[] = {
        { "{\"cmd\":\"mesh-peers-update\",\"station\":\"0x537\",\"action\":\"remove\"}", 5, { "mesh", "peers", "update", "0x537", "remove" } },
        { "{\"cmd\":\"node-filters-update\",\"station\":\"0xABC\",\"action\":\"block\"}", 5, { "node", "filters", "update", "0xABC", "block" } },
        { "{\"cmd\":\"node-filters-clear\",\"scope\":\"manual\"}", 4, { "node", "filters", "clear", "manual" } },
        { "{\"cmd\":\"diag-bbox-enable\"}", 3, { "diag", "bbox", "enable" } },
        { "{\"cmd\":\"stat\",\"scope\":\"mesh\"}", 2, { "stat", "mesh" } },
        { "{\"cmd\":\"vers\"}", 1, { "vers" } },
        /* THE OFFERS HALF, which is the one place the two media take the same fields in different
           SHAPES: a console reads four optional words positionally, a request names them. Identical
           bytes is the only thing that keeps those two readings honest. */
        { "{\"cmd\":\"content-manifest\",\"content\":\"1a2b3c\",\"name\":\"img\",\"expiry\":1440,\"encoding\":\"11\",\"sig\":\"1a2b3d\"}", 7, { "content", "manifest", "1a2b3c", "1a2b3d", "img", "1440", "11" } },
        /* and with everything optional left out, so the DEFAULTS have to agree too -- an omitted
           expiry must mean indefinite through both, never the zero that revokes */
        /* `self` is how a signature stream's OWN manifest says it names none -- the field cannot
           be omitted, but it can deliberately be zero. */
        { "{\"cmd\":\"content-manifest\",\"content\":\"1a2b3c\",\"sig\":\"0\"}", 4, { "content", "manifest", "1a2b3c", "self" } },
        { "{\"cmd\":\"content-manifest\",\"content\":\"1a2b3c\",\"name\":\"img\",\"expiry\":\"indefinite\",\"sig\":\"1a2b3d\"}", 6, { "content", "manifest", "1a2b3c", "1a2b3d", "img", "indefinite" } },
        { "{\"cmd\":\"content-advertise\",\"content\":\"1a2b3c\",\"station\":\"0x57d\"}", 4, { "content", "advertise", "1a2b3c", "0x57d" } },
        /* no audience named: everybody, which is what an advertisement normally is */
        { "{\"cmd\":\"content-advertise\",\"content\":\"1a2b3c\"}", 3, { "content", "advertise", "1a2b3c" } },
        { "{\"cmd\":\"content-revoke\",\"content\":\"1a2b3c\"}", 3, { "content", "revoke", "1a2b3c" } },
        { "{\"cmd\":\"content-manifests\"}", 2, { "content", "manifests" } },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *const root = cJSON_Parse(cases[i].json);
        iotdata_node_control_args_t ja, ca;
        const iotdata_node_control_command_t *const jc = iotdata_node_control_from_json(root, &ja);
        char *v[8];
        for (int k = 0; k < cases[i].argc; k++)
            v[k] = (char *)(uintptr_t)cases[i].argv[k];
        const iotdata_node_control_command_t *const cc = iotdata_node_control_from_argv(cases[i].argc, v, &ca, NULL);
        if (jc == NULL || cc == NULL || jc != cc) {
            printf("  FAIL: %s resolved differently by the two media\n", cases[i].json);
            fails++;
            cJSON_Delete(root);
            continue;
        }
        uint8_t jb[64], cb[64];
        iotdata_kvr_t jk, ck;
        iotdata_kvr_init(&jk, jb, sizeof(jb));
        iotdata_kvr_init(&ck, cb, sizeof(cb));
        (void)iotdata_node_control_build(&jk, jc, &ja);
        (void)iotdata_node_control_build(&ck, cc, &ca);
        if (jk.len != ck.len || memcmp(jb, cb, jk.len) != 0) {
            printf("  FAIL: %s built different bytes from the console form\n", cases[i].json);
            fails++;
        }
        cJSON_Delete(root);
    }
#else
    printf("  (skipped: built without JSON, so there is only one medium here)\n");
#endif
}

static void test_report(void) {
    printf("the report: what a node says it accepts\n");
    static const uint8_t own[] = { IOTDATA_NODE_SUBJECT_MESH, 0x7F }; /* one (subject, action) pair */
    uint8_t buf[128];
    iotdata_kvr_t kv;

    iotdata_kvr_init(&kv, buf, sizeof(buf));
    const iotdata_node_control_report_t plain = { .actions = NULL, .actions_count = 0 };
    CHECK(iotdata_node_control_pack(&kv, &plain) > 0, "packed");
    /* the seven every node answers */
    uint8_t seen = 0;
    size_t cur = 0;
    uint8_t key, vlen;
    const uint8_t *val;
    while (iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen))
        seen++;
    /* nine requestable types -- every one that carries something of its own, DISCRIMINATOR
       excluded because a modifier has no state to state -- plus reboot and reset */
    CHECK(seen == 11, "nine requests, plus the node's two actions");

    /* The tables have no request keys of their own any more: asking for one is STATUS_REQUEST
       with a scope bit, so the inventory must carry STATUS_REQUEST and nothing table-shaped. */
    iotdata_kvr_init(&kv, buf, sizeof(buf));
    const iotdata_node_control_report_t meshy = { .actions = own, .actions_count = (uint8_t)(sizeof(own) / 2u) };
    CHECK(iotdata_node_control_pack(&kv, &meshy) > 0, "packed");
    bool status = false, mine = false, reboot = false;
    cur = 0;
    while (iotdata_kvr_next(buf, kv.len, &cur, &key, &val, &vlen)) {
        if (key == IOTDATA_NODE_CONTROL_REQUEST && vlen == 1 && val[0] == IOTDATA_NODE_TLV_STATUS)
            status = true;
        if (key == IOTDATA_NODE_CONTROL_CONTROL && vlen == 2 && val[0] == IOTDATA_NODE_SUBJECT_NODE && val[1] == IOTDATA_NODE_ACTION_NODE_REBOOT)
            reboot = true;
        if (key == IOTDATA_NODE_CONTROL_CONTROL && vlen == 2 && val[0] == IOTDATA_NODE_SUBJECT_MESH)
            mine = true;
    }
    CHECK(status, "a request for STATUS is advertised, and covers the tables");
    CHECK(reboot, "and the node's own actions");
    CHECK(mine, "and the device's own");

    /* a buffer too small must fail rather than advertise half a list -- a truncated report is a
       node claiming less than it can do, which is a worse lie than claiming more */
    uint8_t small[4];
    iotdata_kvr_init(&kv, small, sizeof(small));
    CHECK(iotdata_node_control_pack(&kv, &meshy) == -1, "a short buffer is an error");
}

int main(void) {
    printf("iotdata_node_control: one node, driven two ways\n\n");
    test_vocabulary();
    test_app_commands();
    test_console();
    test_every_command_builds();
    test_media_agree();
    test_report();
    printf(fails ? "\nFAILED (%d)\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
