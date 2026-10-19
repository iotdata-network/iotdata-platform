#ifndef D_MODULE_MQTT_LINUX_H
#define D_MODULE_MQTT_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_mqtt_linux.h - an MQTT client on libmosquitto. The LINUX half of a seam whose esp32 half
// is esp-mqtt, and it lives here rather than beside one application because both halves are wanted
// by more than one of them.
//
// ONE CLIENT, FILE-SCOPE STATE, which is d_interface_e22900t22.h's shape rather than the datastore's:
// a node has one broker the way it has one radio, so there is no handle to thread through every call
// site -- one mqtt_state_t, whose counters a status publisher reads through mqtt_stats(). It is a
// header with definitions in it, so exactly one translation unit may include it.
//
// SUBSCRIBERS ARE A TABLE, AND THE PATTERN IS THE ROUTE
//
// There was one global callback here, so a second mqtt_subscribe() silently replaced the first; then
// there were two, the second ("aux") carrying a topic prefix of its own and the dispatcher trying it
// first. That prefix was a second copy of the subscription's shape -- "iotdata/ddup/" beside
// "iotdata/ddup/#" -- and a second copy is a thing that goes out of step.
//
// So a subscriber is now a row: the pattern it subscribed with, its handler, its context. The
// pattern is what the dispatcher matches, through mosquitto_topic_matches_sub(), which is the
// broker's own wildcard semantics taken from the library that delivered the message rather than a
// rule reimplemented here. Every matching row gets the message, so two subscribers may overlap on
// purpose; nothing does today, and routing by pattern means the exclusivity the aux path needed
// (a ddup presence must never reach the control parser, which would try to read it as a command)
// now falls out of the topics themselves.
//
// THE TABLE IS REPLAYED AT EVERY CONNECT, which is the part that was not merely tidier. The client
// is created with clean_session=true, so a reconnect starts a session the broker has no
// subscriptions for -- and the old code could not have replayed them, because it never kept the
// topic it subscribed to. A broker restart therefore left this side publishing happily and deaf:
// no management requests, no peer presence, until someone restarted the process. Subscribing
// before the first connect is the same case, so mqtt_subscribe() records the row either way and
// the connect callback is what tells the broker.
//
// The table is written at startup, by the thread that calls the _begin() functions, and read on
// mosquitto's network thread. Nothing subscribes or unsubscribes once traffic is flowing, which is
// the only reason that needs no lock -- as was true of the globals it replaces.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <mosquitto.h>

// ------------------------------------------------------------------------------------------------------------------------

#ifndef MQTT_CONNECT_TIMEOUT
#define MQTT_CONNECT_TIMEOUT 60
#endif
#ifndef MQTT_PUBLISH_QOS
#define MQTT_PUBLISH_QOS 0
#endif
#ifndef MQTT_PUBLISH_RETAIN
#define MQTT_PUBLISH_RETAIN false
#endif
#ifndef MQTT_TOPIC_MAX
#define MQTT_TOPIC_MAX 128
#endif
#ifndef MQTT_SUBSCRIBERS_MAX
#define MQTT_SUBSCRIBERS_MAX 4
#endif

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const char *server;
    const char *client;
    bool synchronous;
    bool tls_insecure;
    unsigned int reconnect_delay;
    unsigned int reconnect_delay_max;
    bool debug;
} mqtt_config_t;

typedef void (*mqtt_handler_t)(void *ctx, const char *topic, const void *payload, int length);

typedef struct {
    char pattern[MQTT_TOPIC_MAX];
    int qos;
    mqtt_handler_t handler;
    void *ctx;
} mqtt_subscriber_t;

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint32_t connects;
    uint32_t disconnects;
    uint32_t reconnects;
    time_t last_connect_time;
    uint32_t publishes;
    uint64_t publish_bytes;
    uint32_t publish_errors;
} mqtt_stats_t;

typedef struct {
    struct mosquitto *mosq;
    bool connected;
    bool synchronous;
    unsigned int reconnect_delay_base;
    unsigned int reconnect_delay_max_base;
    unsigned int reconnect_delay_current;
    time_t reconnect_next_attempt;
    mqtt_subscriber_t subscribers[MQTT_SUBSCRIBERS_MAX];
    int subscribers_count;
    mqtt_stats_t stats;
} mqtt_state_t;

// ------------------------------------------------------------------------------------------------------------------------

static char _mqtt_will_topic[MQTT_TOPIC_MAX] = "";
static mqtt_state_t _mqtt_state;

// ------------------------------------------------------------------------------------------------------------------------

static bool _mqtt_parse_server_string(const char *string, char *host, const int length, int *port, bool *ssl) {
    host[0] = '\0';
    *port = 1883;
    *ssl = false;
    if (strncmp(string, "mqtt://", 7) == 0)
        strncpy(host, string + 7, (size_t)length - 1);
    else if (strncmp(string, "mqtts://", 8) == 0) {
        strncpy(host, string + 8, (size_t)length - 1);
        *ssl = true;
        *port = 8883;
    } else
        strncpy(host, string, (size_t)length - 1);
    char *port_str = strchr(host, ':');
    if (port_str) {
        *port_str = '\0'; // Terminate host string at colon
        *port = atoi(port_str + 1);
    }
    return true;
}

static bool _mqtt_resolve_ipv4(const char *const host, char *const addr, const size_t size) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
        return false;
    struct sockaddr_in sin; /* copied, not cast: sockaddr is less aligned than sockaddr_in */
    memcpy(&sin, res->ai_addr, sizeof(sin));
    freeaddrinfo(res);
    return inet_ntop(AF_INET, &sin.sin_addr, addr, (socklen_t)size) != NULL;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

/* THE WILL IS REGISTERED AT CONNECT, so this has to be called BEFORE mqtt_begin(). Held here and
   applied there, rather than taking a second connect path. */
void mqtt_set_will(const char *const topic) {
    snprintf(_mqtt_will_topic, sizeof(_mqtt_will_topic), "%s", topic ? topic : "");
}

// ------------------------------------------------------------------------------------------------------------------------

bool mqtt_send(const char *topic, const char *message, const int length) {
    mqtt_state_t *const st = &_mqtt_state;
    if (!st->mosq || !st->connected) {
        st->stats.publish_errors++;
        return false;
    }
    const int result = mosquitto_publish(st->mosq, NULL, topic, length, message, MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN);
    if (result == MOSQ_ERR_SUCCESS) {
        st->stats.publishes++;
        st->stats.publish_bytes += (uint64_t)length;
        return true;
    } else {
        st->stats.publish_errors++;
        PRINTF_ERROR("mqtt: publish error: %s\n", mosquitto_strerror(result));
        return false;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * RETAINED publish, and a NULL payload is how a retained topic is CLEARED -- an empty retained
 * message is the broker's own way of saying "this is no longer true", and it is what a last will
 * publishes on our behalf if we die without saying it.
 */
bool mqtt_send_retained(const char *const topic, const char *const message, const int length) {
    mqtt_state_t *const st = &_mqtt_state;
    if (!st->mosq || !st->connected) {
        st->stats.publish_errors++;
        return false;
    }
    const int result = mosquitto_publish(st->mosq, NULL, topic, length, (length > 0) ? message : NULL, MQTT_PUBLISH_QOS, true);
    if (result == MOSQ_ERR_SUCCESS) {
        st->stats.publishes++;
        st->stats.publish_bytes += (uint64_t)((length > 0) ? length : 0);
        return true;
    } else {
        st->stats.publish_errors++;
        PRINTF_ERROR("mqtt: publish (retained) error: %s\n", mosquitto_strerror(result));
        return false;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

static int _mqtt_message_dispatch(const char *const topic, const void *const payload, const int length) {
    mqtt_state_t *const st = &_mqtt_state;
    int reached = 0;
    for (int i = 0; i < st->subscribers_count; i++) {
        bool matches = false;
        if (mosquitto_topic_matches_sub(st->subscribers[i].pattern, topic, &matches) == MOSQ_ERR_SUCCESS && matches) {
            st->subscribers[i].handler(st->subscribers[i].ctx, topic, payload, length);
            reached++;
        }
    }
    return reached;
}

// ------------------------------------------------------------------------------------------------------------------------

static void _mqtt_message_callback_wrapper(struct mosquitto *m, void *o __attribute__((unused)), const struct mosquitto_message *message) {
    mqtt_state_t *const st = &_mqtt_state;
    if (m != st->mosq)
        return;
    if (_mqtt_message_dispatch((const char *)message->topic, message->payload, message->payloadlen) == 0)
        PRINTF_DEBUG("mqtt: message on '%s' (%d bytes) matched no subscriber\n", message->topic, message->payloadlen);
}

// ------------------------------------------------------------------------------------------------------------------------

/* The SUBSCRIBE packet itself, shared by mqtt_subscribe() and by the connect replay. */
static bool _mqtt_subscribe_send(const mqtt_subscriber_t *const sub) {
    mqtt_state_t *const st = &_mqtt_state;
    const int result = mosquitto_subscribe(st->mosq, NULL, sub->pattern, sub->qos);
    if (result == MOSQ_ERR_SUCCESS) {
        PRINTF_INFO("mqtt: subscribed to topic '%s' (qos=%d)\n", sub->pattern, sub->qos);
        return true;
    } else {
        PRINTF_ERROR("mqtt: subscribe error on '%s': %s\n", sub->pattern, mosquitto_strerror(result));
        return false;
    }
}

bool mqtt_subscribe(const char *const pattern, const int qos, const mqtt_handler_t handler, void *const ctx) {
    mqtt_state_t *const st = &_mqtt_state;
    if (!pattern || pattern[0] == '\0' || !handler)
        return false;
    mqtt_subscriber_t *sub = NULL;
    for (int i = 0; i < st->subscribers_count && !sub; i++)
        if (strcmp(st->subscribers[i].pattern, pattern) == 0)
            sub = &st->subscribers[i];
    if (!sub) {
        if (st->subscribers_count == MQTT_SUBSCRIBERS_MAX) {
            PRINTF_ERROR("mqtt: subscriber table full (%d), '%s' not subscribed\n", MQTT_SUBSCRIBERS_MAX, pattern);
            return false;
        }
        sub = &st->subscribers[st->subscribers_count++];
    }
    *sub = (mqtt_subscriber_t){
        .qos = qos,
        .handler = handler,
        .ctx = ctx,
    };
    snprintf(sub->pattern, sizeof(sub->pattern), "%s", pattern);
    if (st->mosq && st->connected)
        return _mqtt_subscribe_send(sub);
    return true;
}

bool mqtt_unsubscribe(const char *const pattern) {
    mqtt_state_t *const st = &_mqtt_state;
    for (int i = 0; i < st->subscribers_count; i++)
        if (strcmp(st->subscribers[i].pattern, pattern) == 0) {
            st->subscribers[i] = st->subscribers[--st->subscribers_count];
            memset(&st->subscribers[st->subscribers_count], 0, sizeof(st->subscribers[0]));
            break;
        }
    if (!st->mosq)
        return false;
    const int result = mosquitto_unsubscribe(st->mosq, NULL, pattern);
    if (result == MOSQ_ERR_SUCCESS) {
        PRINTF_INFO("mqtt: unsubscribed from topic '%s'\n", pattern);
        return true;
    } else {
        PRINTF_ERROR("mqtt: unsubscribe error: %s\n", mosquitto_strerror(result));
        return false;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

bool mqtt_is_connected(void) {
    const mqtt_state_t *const st = &_mqtt_state;
    return st->mosq && st->connected;
}

// ------------------------------------------------------------------------------------------------------------------------

static void _mqtt_connect_callback(struct mosquitto *m, void *o __attribute__((unused)), int r) {
    mqtt_state_t *const st = &_mqtt_state;
    if (m != st->mosq)
        return;
    if (r == 0) {
        st->connected = true;
        st->stats.connects++;
        st->stats.last_connect_time = hw_clock();
        st->reconnect_delay_current = st->reconnect_delay_base;
        st->reconnect_next_attempt = 0;
        PRINTF_INFO("mqtt: connected\n");
        for (int i = 0; i < st->subscribers_count; i++)
            (void)_mqtt_subscribe_send(&st->subscribers[i]);
    } else
        PRINTF_ERROR("mqtt: connect failed: %s\n", mosquitto_connack_string(r));
}

static void _mqtt_disconnect_callback(struct mosquitto *m, void *o __attribute__((unused)), int r) {
    mqtt_state_t *const st = &_mqtt_state;
    if (m != st->mosq)
        return;
    st->connected = false;
    st->stats.disconnects++;
    if (r == 0)
        PRINTF_INFO("mqtt: disconnected (clean)\n");
    else
        PRINTF_ERROR("mqtt: disconnected unexpectedly: rc=%d\n", r);
}

// ------------------------------------------------------------------------------------------------------------------------

static void _mqtt_reconnect_attempt(void) {
    mqtt_state_t *const st = &_mqtt_state;
    const time_t now = hw_clock();
    if (now >= st->reconnect_next_attempt) {
        if (st->reconnect_delay_current == 0)
            st->reconnect_delay_current = st->reconnect_delay_base ? st->reconnect_delay_base : 1;
        PRINTF_INFO("mqtt: attempting reconnect\n");
        const int r = mosquitto_reconnect(st->mosq);
        if (r == MOSQ_ERR_SUCCESS) {
            st->stats.reconnects++;
            st->reconnect_delay_current = st->reconnect_delay_base;
            st->reconnect_next_attempt = 0;
        } else {
            PRINTF_ERROR("mqtt: reconnect failed: %s (next attempt in %us)\n", mosquitto_strerror(r), st->reconnect_delay_current);
            st->reconnect_next_attempt = now + st->reconnect_delay_current;
            st->reconnect_delay_current *= 2;
            if (st->reconnect_delay_max_base > 0 && st->reconnect_delay_current > st->reconnect_delay_max_base)
                st->reconnect_delay_current = st->reconnect_delay_max_base;
        }
    }
}

void mqtt_loop(const int timeout_ms) {
    mqtt_state_t *const st = &_mqtt_state;
    if (!st->mosq)
        return;
    const int r = mosquitto_loop(st->mosq, timeout_ms, 1);
    if (r != MOSQ_ERR_SUCCESS) {
        if (st->connected) {
            st->connected = false;
            st->stats.disconnects++;
            PRINTF_ERROR("mqtt: loop error: %s\n", mosquitto_strerror(r));
        }
        _mqtt_reconnect_attempt();
    }
}

// ------------------------------------------------------------------------------------------------------------------------

const mqtt_stats_t *mqtt_stats(void) {
    const mqtt_state_t *const st = &_mqtt_state;
    return &st->stats;
}

// ------------------------------------------------------------------------------------------------------------------------

bool mqtt_begin(const mqtt_config_t *cfg) {
    mqtt_state_t *const st = &_mqtt_state;

    *st = (mqtt_state_t){ 0 };

    char host[256];
    int port;
    bool ssl;
    if (!_mqtt_parse_server_string(cfg->server, host, sizeof(host), &port, &ssl)) {
        PRINTF_ERROR("mqtt: error parsing details in '%s'\n", cfg->server);
        return false;
    }
    /* not for TLS: the certificate is checked against the name */
    char addr[INET_ADDRSTRLEN] = "";
    const bool by_addr = !ssl && _mqtt_resolve_ipv4(host, addr, sizeof(addr)) && strcmp(addr, host) != 0;
    PRINTF_INFO("mqtt: connecting (host='%s'%s%s, port=%d, ssl=%s, client='%s')\n", host, by_addr ? " -> " : "", addr, port, ssl ? "true" : "false", cfg->client);
    char client_id[24];
    snprintf(client_id, sizeof(client_id), "%s-%06X", cfg->client ? cfg->client : "mqtt-linux", (unsigned int)(hw_clock() ^ getpid()) & 0xFFFFFF);
    mosquitto_lib_init();
    if (!(st->mosq = mosquitto_new(client_id, true, NULL))) {
        PRINTF_ERROR("mqtt: error creating client instance\n");
        return false;
    }
    if (ssl) {
        mosquitto_tls_set(st->mosq, NULL, NULL, NULL, NULL, NULL);
        if (cfg->tls_insecure) {
            mosquitto_tls_insecure_set(st->mosq, true);
            PRINTF_INFO("mqtt: WARNING tls certificate validation disabled\n");
        }
    }
    if (_mqtt_will_topic[0] != '\0')
        (void)mosquitto_will_set(st->mosq, _mqtt_will_topic, 0, NULL, MQTT_PUBLISH_QOS, true);
    mosquitto_connect_callback_set(st->mosq, _mqtt_connect_callback);
    mosquitto_disconnect_callback_set(st->mosq, _mqtt_disconnect_callback);
    mosquitto_message_callback_set(st->mosq, _mqtt_message_callback_wrapper);
    if (cfg->reconnect_delay > 0)
        mosquitto_reconnect_delay_set(st->mosq, cfg->reconnect_delay, cfg->reconnect_delay_max, true);
    st->reconnect_delay_base = cfg->reconnect_delay > 0 ? cfg->reconnect_delay : 1;
    st->reconnect_delay_max_base = cfg->reconnect_delay_max > 0 ? cfg->reconnect_delay_max : st->reconnect_delay_base;
    st->reconnect_delay_current = st->reconnect_delay_base;
    st->reconnect_next_attempt = 0;
    int result;
    if ((result = mosquitto_connect(st->mosq, by_addr ? addr : host, port, MQTT_CONNECT_TIMEOUT)) != MOSQ_ERR_SUCCESS) {
        PRINTF_ERROR("mqtt: error connecting to broker: %s\n", mosquitto_strerror(result));
        mosquitto_destroy(st->mosq);
        st->mosq = NULL;
        return false;
    }
    st->synchronous = cfg->synchronous;
    if (!st->synchronous && (result = mosquitto_loop_start(st->mosq)) != MOSQ_ERR_SUCCESS) {
        PRINTF_ERROR("mqtt: error starting loop: %s\n", mosquitto_strerror(result));
        mosquitto_disconnect(st->mosq);
        mosquitto_destroy(st->mosq);
        st->mosq = NULL;
        return false;
    }
    if (st->synchronous)
        for (int i = 0; i < 50 && !st->connected; i++)
            mosquitto_loop(st->mosq, 100, 1);

    return true;
}

// ------------------------------------------------------------------------------------------------------------------------

void mqtt_end(void) {
    mqtt_state_t *const st = &_mqtt_state;
    st->connected = false;
    st->subscribers_count = 0;
    if (st->mosq) {
        if (!st->synchronous)
            mosquitto_loop_stop(st->mosq, true);
        mosquitto_disconnect(st->mosq);
        mosquitto_destroy(st->mosq);
        st->mosq = NULL;
    }
    mosquitto_lib_cleanup();
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_MQTT_LINUX_H */
