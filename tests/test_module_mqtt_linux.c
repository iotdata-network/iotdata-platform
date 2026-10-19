//
// test_module_mqtt_linux.c - the subscriber table in d_module_mqtt_linux.h, with no broker.
//
// THE ROUTING IS THE ONLY LOGIC IN THAT MODULE THAT IS NOT MOSQUITTO'S. Everything else is a thin
// wrapper over a library call and needs a broker to mean anything; the table does not -- it is a
// list of patterns, a handler each, and mosquitto_topic_matches_sub(), which is pure string work
// that links without a connection. So this suite drives the dispatcher directly.
//
// WHAT IT IS GUARDING. The module replaced one global callback plus a second ("aux") channel
// carrying its own topic prefix, and the three things that went wrong there are all checked below:
// a second subscriber silently replacing the first, a message reaching a handler that does not own
// it, and a subscription made before the connect never being sent. The last one is why
// mqtt_subscribe() records a row and returns true with no client at all -- the connect callback is
// what tells the broker -- and that is exactly the state this suite runs in.
//

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The log hooks first, as an application does it: the module reports a full table through
   PRINTF_ERROR, and this suite asserts that it does rather than printing it. */
static int g_errors = 0;
static void test_log_discard(const char *const format, ...) {
    (void)format;
}
__attribute__((format(printf, 1, 2))) static void test_log_error(const char *const format, ...) {
    (void)format;
    g_errors++;
}
#define PRINTF_INFO  test_log_discard
#define PRINTF_WARN  test_log_discard
#define PRINTF_DEBUG test_log_discard
#define PRINTF_ERROR test_log_error

/* hw_clock() comes from the platform shim, as it does in the gateway */
#include "d_platform_linux.h"
#include "d_common.h"

/* SMALLER THAN THE DEFAULT, so the full-table path is reachable at all -- the same reason the
   content suite runs with a table smaller than its store. */
#define MQTT_SUBSCRIBERS_MAX 3
#include "d_module_mqtt_linux.h"

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

/* ------------------------------------------------------------------------------------------- */

typedef struct {
    int calls;
    char topic[MQTT_TOPIC_MAX];
    char payload[64];
    int length;
} sink_t;

static sink_t sink_a, sink_b, sink_c, sink_d;

static void sink_handler(void *const ctx, const char *const topic, const void *const payload, const int length) {
    sink_t *const s = (sink_t *)ctx;
    s->calls++;
    s->length = length;
    (void)snprintf(s->topic, sizeof(s->topic), "%s", topic);
    s->payload[0] = '\0';
    if (payload != NULL && length > 0)
        (void)snprintf(s->payload, sizeof(s->payload), "%.*s", length, (const char *)payload);
}

/* The module keeps ONE table, so every test starts from an empty one -- otherwise a test would be
   routing through whatever the one before it left registered. */
static void rig(void) {
    memset(_mqtt_state.subscribers, 0, sizeof(_mqtt_state.subscribers));
    _mqtt_state.subscribers_count = 0;
    memset(&sink_a, 0, sizeof(sink_a));
    memset(&sink_b, 0, sizeof(sink_b));
    memset(&sink_c, 0, sizeof(sink_c));
    memset(&sink_d, 0, sizeof(sink_d));
    g_errors = 0;
}

static int deliver(const char *const topic, const char *const payload) {
    return _mqtt_message_dispatch(topic, payload, (payload != NULL) ? (int)strlen(payload) : 0);
}

/* ------------------------------------------------------------------------------------------- */

/* NO CLIENT AT ALL, which is the state a subscribe made before mqtt_begin() runs in. It must be
   recorded rather than refused: the connect callback replays the table, and a row that was never
   added is a channel that goes deaf with no error anywhere. */
static void test_subscribe_without_a_connection(void) {
    rig();
    CHECK(_mqtt_state.mosq == NULL, "no client: this suite never connects");
    CHECK(mqtt_subscribe("iotdata/manage/req", 0, sink_handler, &sink_a), "a subscribe with no client is recorded, not refused");
    CHECK(_mqtt_state.subscribers_count == 1, "and it is one row");
    CHECK(g_errors == 0, "with nothing reported");
}

static void test_arguments_are_checked(void) {
    rig();
    CHECK(!mqtt_subscribe(NULL, 0, sink_handler, &sink_a), "a null pattern is refused");
    CHECK(!mqtt_subscribe("", 0, sink_handler, &sink_a), "so is an empty one -- it would match nothing and hide the mistake");
    CHECK(!mqtt_subscribe("iotdata/#", 0, NULL, &sink_a), "and a row with no handler");
    CHECK(_mqtt_state.subscribers_count == 0, "none of them took a slot");
}

/* THE CASE THE PREFIX USED TO ANSWER. A ddup presence must not reach the control handler, which
   would try to read it as a command -- and now that is a consequence of the patterns themselves
   rather than of a prefix compared before the fallback. */
static void test_routing_is_by_pattern(void) {
    rig();
    (void)mqtt_subscribe("iotdata/manage/req", 0, sink_handler, &sink_a);
    (void)mqtt_subscribe("iotdata/ddup/#", 0, sink_handler, &sink_b);

    CHECK(deliver("iotdata/ddup/0A1", "{\"ip\":\"10.0.0.1\"}") == 1, "a presence reaches one handler");
    CHECK(sink_b.calls == 1 && sink_a.calls == 0, "and it is the one that subscribed to it");
    CHECK(strcmp(sink_b.topic, "iotdata/ddup/0A1") == 0, "the topic arrives whole, not just the matched part");
    CHECK(strcmp(sink_b.payload, "{\"ip\":\"10.0.0.1\"}") == 0, "with its payload");

    CHECK(deliver("iotdata/manage/req", "{\"cmd\":\"status\"}") == 1, "a request reaches one handler");
    CHECK(sink_a.calls == 1 && sink_b.calls == 1, "and it is the other one");

    CHECK(deliver("iotdata/gateway/001", "{}") == 0, "a topic nobody subscribed to reaches nobody");
    CHECK(sink_a.calls == 1 && sink_b.calls == 1, "and leaves both handlers alone");
}

/* The broker's own wildcard rules, because the matcher IS the broker's. '+' is one level, '#' is
   the rest including none of it. */
static void test_wildcards_are_the_brokers(void) {
    rig();
    (void)mqtt_subscribe("iotdata/+/req", 0, sink_handler, &sink_a);
    (void)mqtt_subscribe("iotdata/ddup/#", 0, sink_handler, &sink_b);
    CHECK(deliver("iotdata/manage/req", "x") == 1 && sink_a.calls == 1, "+ matches one level");
    CHECK(deliver("iotdata/a/b/req", "x") == 0, "and only one");
    CHECK(deliver("iotdata/ddup", "x") == 1 && sink_b.calls == 1, "# matches the parent itself");
    CHECK(deliver("iotdata/ddup/a/b", "x") == 1 && sink_b.calls == 2, "and any depth below it");
}

/* Two subscribers may overlap ON PURPOSE -- a wildcard tap beside a specific handler -- and both
   get the message. This is the behaviour the single global callback could not have. */
static void test_overlapping_subscribers_both_run(void) {
    rig();
    (void)mqtt_subscribe("iotdata/#", 0, sink_handler, &sink_a);
    (void)mqtt_subscribe("iotdata/ddup/#", 0, sink_handler, &sink_b);
    CHECK(deliver("iotdata/ddup/0A1", "x") == 2, "both matching rows are delivered to");
    CHECK(sink_a.calls == 1 && sink_b.calls == 1, "each exactly once");
}

/* THE ORIGINAL BUG, now a defined outcome: subscribing the same pattern twice replaces its
   handler, rather than quietly routing to the old one or to both. */
static void test_the_same_pattern_twice_replaces(void) {
    rig();
    (void)mqtt_subscribe("iotdata/manage/req", 0, sink_handler, &sink_a);
    CHECK(mqtt_subscribe("iotdata/manage/req", 1, sink_handler, &sink_b), "the same pattern again is accepted");
    CHECK(_mqtt_state.subscribers_count == 1, "and does not take a second slot");
    CHECK(deliver("iotdata/manage/req", "x") == 1, "one delivery, not two");
    CHECK(sink_b.calls == 1 && sink_a.calls == 0, "to the handler that replaced the first");
    CHECK(_mqtt_state.subscribers[0].qos == 1, "the qos is the new one too, so the connect replay sends that");
}

static void test_a_full_table_is_refused_and_says_so(void) {
    rig();
    CHECK(mqtt_subscribe("a/#", 0, sink_handler, &sink_a), "1 of 3");
    CHECK(mqtt_subscribe("b/#", 0, sink_handler, &sink_b), "2 of 3");
    CHECK(mqtt_subscribe("c/#", 0, sink_handler, &sink_c), "3 of 3");
    CHECK(g_errors == 0, "a table filled to its limit is not an error");
    CHECK(!mqtt_subscribe("d/#", 0, sink_handler, &sink_d), "the fourth is refused");
    CHECK(g_errors == 1, "and reported -- a dimensioning mistake must not be silent");
    CHECK(_mqtt_state.subscribers_count == 3, "the table is unchanged");
    CHECK(deliver("d/1", "x") == 0 && sink_d.calls == 0, "and the refused handler never runs");
}

/* mqtt_unsubscribe() returns false with no client -- it cannot tell a broker anything -- but the
   row goes either way, and the rows after it still route. The last one fills the hole, so this is
   also the check that the swap did not lose an entry. */
static void test_unsubscribe_drops_the_row(void) {
    rig();
    (void)mqtt_subscribe("a/#", 0, sink_handler, &sink_a);
    (void)mqtt_subscribe("b/#", 0, sink_handler, &sink_b);
    (void)mqtt_subscribe("c/#", 0, sink_handler, &sink_c);
    (void)mqtt_unsubscribe("b/#");
    CHECK(_mqtt_state.subscribers_count == 2, "the row is gone");
    CHECK(deliver("b/1", "x") == 0 && sink_b.calls == 0, "and routes nowhere");
    CHECK(deliver("a/1", "x") == 1 && sink_a.calls == 1, "the row before it still routes");
    CHECK(deliver("c/1", "x") == 1 && sink_c.calls == 1, "and so does the one that filled the hole");
    (void)mqtt_unsubscribe("nothing/#");
    CHECK(_mqtt_state.subscribers_count == 2, "unsubscribing what was never subscribed changes nothing");
}

/* AN EMPTY PAYLOAD IS A MESSAGE, not an absence: a cleared retained topic is how a peer says it is
   offline, and the handler has to be given the chance to see length 0. */
static void test_an_empty_payload_is_delivered(void) {
    rig();
    (void)mqtt_subscribe("iotdata/ddup/#", 0, sink_handler, &sink_a);
    CHECK(_mqtt_message_dispatch("iotdata/ddup/0A1", NULL, 0) == 1, "a cleared retained topic is delivered");
    CHECK(sink_a.calls == 1 && sink_a.length == 0, "with length 0");
}

/* THE BROKER IS REACHED BY ITS IPv4 ADDRESS when it has one: the fleet's listens on nothing else, and
   a lookup for either family can come back with IPv6 only. No network here -- localhost is in the
   hosts table, and a literal is not looked up at all. */
static void test_the_broker_is_found_by_its_ipv4_address(void) {
    char a[INET_ADDRSTRLEN];
    CHECK(_mqtt_resolve_ipv4("localhost", a, sizeof(a)) && strcmp(a, "127.0.0.1") == 0, "a name gives its IPv4 address, though ::1 may be listed first");
    CHECK(_mqtt_resolve_ipv4("192.0.2.7", a, sizeof(a)) && strcmp(a, "192.0.2.7") == 0, "an IPv4 literal is itself");
    CHECK(!_mqtt_resolve_ipv4("::1", a, sizeof(a)), "an IPv6 literal has none, and the caller connects by name as before");
    CHECK(!_mqtt_resolve_ipv4("localhost", a, 4), "nor does an address that will not fit");
}

/* The client and its subscribers end together: the rows point at state the application tears down
   alongside the client, so a later mqtt_begin() must not replay them. Last in the suite, because it
   is the one test that ends the module. */
static void test_end_forgets_the_subscribers(void) {
    rig();
    (void)mqtt_subscribe("iotdata/#", 0, sink_handler, &sink_a);
    mqtt_end();
    CHECK(_mqtt_state.subscribers_count == 0, "mqtt_end() empties the table");
    CHECK(deliver("iotdata/ddup/0A1", "x") == 0 && sink_a.calls == 0, "and nothing routes afterwards");
}

/* ------------------------------------------------------------------------------------------- */

int main(void) {
    printf("test_module_mqtt_linux: the subscriber table\n");
    test_subscribe_without_a_connection();
    test_arguments_are_checked();
    test_routing_is_by_pattern();
    test_wildcards_are_the_brokers();
    test_overlapping_subscribers_both_run();
    test_the_same_pattern_twice_replaces();
    test_a_full_table_is_refused_and_says_so();
    test_unsubscribe_drops_the_row();
    test_an_empty_payload_is_delivered();
    test_the_broker_is_found_by_its_ipv4_address();
    test_end_forgets_the_subscribers();
    if (fails > 0) {
        printf("\n%d FAILED\n", fails);
        return 1;
    }
    printf("\nall ok\n");
    return 0;
}
