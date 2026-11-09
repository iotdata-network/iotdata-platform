// test_interface_cart.c - the switched-rail companion supervisor.
//
// The whole module is a clock and a pin, so a host test can drive both exactly: a fake GPIO pair
// and a time that only moves when the test says so. What is worth pinning is not that the state
// machine transitions -- it is the handful of decisions that are easy to get subtly wrong and
// impossible to observe in the field: that liveness is a CHANGE and never a level, that a cart
// which goes quiet and speaks again is not cut, that every path ends with the power off.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- the device seam, faked ------------------------------------------------------------------ */

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)

static bool g_pin_out;  /* what the module drives: the load switch enable */
static bool g_pin_in;   /* what the cart drives: the cartbeat             */
static int g_out_edges; /* how many times the power actually changed      */

static void hw_gpio_cfg_enable_output(const gpio_num_t pin) {
    (void)pin;
}
static void hw_gpio_cfg_enable_input(const gpio_num_t pin, const bool pullup) {
    (void)pin;
    (void)pullup;
}
static bool hw_gpio_get(const gpio_num_t pin) {
    (void)pin;
    return g_pin_in;
}
static void hw_gpio_set(const gpio_num_t pin, const bool level) {
    (void)pin;
    if (level != g_pin_out)
        g_out_edges++;
    g_pin_out = level;
}

__attribute__((format(printf, 2, 3))) static void test_log(const char *tag, const char *fmt, ...) {
    (void)tag;
    (void)fmt;
}
#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log(tag, __VA_ARGS__)

#include "device/d_interface_cart.h"

/* --- harness ---------------------------------------------------------------------------------- */

static int fails = 0;
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            printf("  FAIL: %s (line %d)\n", m, __LINE__); \
            fails++; \
        } \
    } while (0)

static uint32_t g_now;

/* Advance time in loop-sized steps, beating the pin every `beat_ms` if asked. Stepping rather than
   jumping is the point: the module is specified to be sampled, so the test samples it. */
static cart_event_t run_ms(cart_t *const c, const uint32_t span_ms, const uint32_t beat_ms) {
    cart_event_t last = CART_EVENT_NONE;
    const uint32_t step = 100u; /* the relay's loop cadence */
    uint32_t beat_at = beat_ms;
    for (uint32_t t = 0; t < span_ms; t += step) {
        g_now += step;
        if (beat_ms > 0u && t + step >= beat_at) {
            g_pin_in = !g_pin_in; /* a CHANGE -- the module must not care which way */
            beat_at += beat_ms;
        }
        const cart_event_t e = cart_tick(c, g_now);
        if (e != CART_EVENT_NONE)
            last = e;
    }
    return last;
}

static cart_config_t small_config(void) {
    cart_config_t cfg = CART_CONFIG_DEFAULTS(1, 2);
    cfg.interval_ms = 10000u; /* a window every 10s, so a test can watch several */
    cfg.boot_ms = 3000u;
    cfg.silent_ms = 2000u;
    cfg.settle_ms = 1000u;
    cfg.limit_ms = 20000u;
    cfg.retry_ms = 10000u;
    cfg.retry_max = 3u;
    return cfg;
}

int main(void) {
    printf("the cart: a companion board on a switched rail\n\n");
    cart_config_t cfg = small_config();
    static cart_t c;

    printf("OFF is the state it starts in, and the power is off before anything else happens\n");
    g_now = 1000u;
    g_pin_in = false;
    g_pin_out = true; /* as if a pad were left high: init must correct it */
    g_out_edges = 0;
    cart_init(&c, &cfg, g_now);
    CHECK(!g_pin_out, "init drove the rail down");
    CHECK(c.state == CART_OFF, "and starts closed");
    CHECK(!cart_is_open(&c), "which is what is_open says");

    printf("\nthe first window is one interval away, not immediate\n");
    CHECK(run_ms(&c, cfg.interval_ms - 2000u, 0) == CART_EVENT_NONE, "nothing yet");
    CHECK(!g_pin_out, "still off");
    CHECK(run_ms(&c, 3000u, 0) == CART_EVENT_OPENED, "then it opens on its own");
    CHECK(g_pin_out, "and the rail is live");

    printf("\nA LEVEL IS NOT A CARTBEAT: a pin stuck high reads as dead, not as working\n");
    g_pin_in = true; /* and never changes again -- the wedged-cart case */
    CHECK(run_ms(&c, cfg.boot_ms + 500u, 0) == CART_EVENT_NO_BOOT, "the boot deadline still fires");
    CHECK(!g_pin_out, "and the power went off");
    CHECK(c.st_no_boot == 1, "counted as a failure to boot");
    CHECK(c.fails == 1, "and against the back-off");

    printf("\na cart that beats is a cart that lives\n");
    g_pin_in = false;
    CHECK(run_ms(&c, cart_wait_ms(&c) + 500u, 0) == CART_EVENT_OPENED, "next window (after the back-off)");
    CHECK(run_ms(&c, 1000u, 500u) == CART_EVENT_LIVE, "it spoke");
    CHECK(c.state == CART_RUNNING, "so it is running");

    printf("\nAND IT STAYS UP WHILE IT KEEPS BEATING -- the cartbeat IS the hold-open\n");
    /* well past the boot deadline and several times the silence bound: only the beat holds it */
    CHECK(run_ms(&c, 8000u, 500u) == CART_EVENT_NONE, "no event while it works");
    CHECK(g_pin_out && c.state == CART_RUNNING, "still powered, still running");

    printf("\nstopping is how it says it is finished, and the settle is the grace\n");
    CHECK(run_ms(&c, cfg.silent_ms - 500u, 0) == CART_EVENT_NONE, "quiet, but not yet quiet enough");
    CHECK(c.state == CART_RUNNING, "still running");
    CHECK(run_ms(&c, 1000u, 0) == CART_EVENT_NONE, "now settling");
    CHECK(c.state == CART_SETTLING, "and it says so");
    CHECK(g_pin_out, "power is STILL on during the settle");
    CHECK(run_ms(&c, cfg.settle_ms + 500u, 0) == CART_EVENT_CLOSED, "then the cut");
    CHECK(!g_pin_out, "and the rail is down");
    CHECK(c.fails == 0, "a good window clears the back-off");
    CHECK(c.st_closed == 1, "counted as a normal close");

    printf("\nA BEAT ARRIVING INSIDE THE SETTLE IS LIVENESS, so the settle is abandoned\n");
    /* a cart that goes quiet for longer than silent_ms and then beats again was never finished: a
       stalled scheduler or a long busy moment looks exactly like this. Cutting on the first silence
       would cut a working cart, which is why a late change puts it back to RUNNING. */
    CHECK(run_ms(&c, cfg.interval_ms + 500u, 0) == CART_EVENT_OPENED, "a fresh window");
    CHECK(run_ms(&c, 1000u, 500u) == CART_EVENT_LIVE, "booted");
    CHECK(run_ms(&c, cfg.silent_ms + 300u, 0) == CART_EVENT_NONE, "goes quiet: shutdown has begun");
    CHECK(c.state == CART_SETTLING, "settling");
    g_pin_in = !g_pin_in; /* a late beat */
    CHECK(cart_tick(&c, g_now) == CART_EVENT_NONE, "the pulse is not an event");
    CHECK(c.state == CART_RUNNING, "but it IS liveness: the settle is abandoned");
    CHECK(g_pin_out, "and nothing was cut");
    CHECK(run_ms(&c, cfg.silent_ms + cfg.settle_ms + 500u, 0) == CART_EVENT_CLOSED, "then it really does finish");
    CHECK(!g_pin_out, "off");

    printf("\nthe BACKSTOP cuts a cart that is alive and will not stop\n");
    CHECK(run_ms(&c, cfg.interval_ms + 500u, 0) == CART_EVENT_OPENED, "a window");
    CHECK(run_ms(&c, 1000u, 500u) == CART_EVENT_LIVE, "booted");
    /* beating the whole way: nothing but the backstop can end this */
    CHECK(run_ms(&c, cfg.limit_ms, 500u) == CART_EVENT_OVERRAN, "the limit fired against a living cart");
    CHECK(!g_pin_out, "cut anyway -- the battery is the thing being protected");
    CHECK(c.st_overran == 1, "counted apart from a failure to boot: a different problem");

    printf("\nback-off stretches for a cart that never comes back, and is capped\n");
    {
        static cart_t d;
        cart_init(&d, &cfg, 1000u);
        g_pin_in = false;
        const uint32_t plain = cart_wait_ms(&d);
        for (int i = 0; i < 6; i++)
            d.fails = (uint8_t)(i + 1), (void)0;
        CHECK(cart_wait_ms(&d) == cfg.interval_ms + (uint32_t)cfg.retry_max * cfg.retry_ms, "capped at retry_max, not unbounded");
        d.fails = 1;
        CHECK(cart_wait_ms(&d) > plain, "and one failure already stretches it");
        d.fails = 0;
        CHECK(cart_wait_ms(&d) == plain, "while success returns to the plain interval at once");
    }

    printf("\nopen and close on demand, and every path ends with the rail down\n");
    {
        static cart_t d;
        cart_init(&d, &cfg, 1000u);
        g_now = 1000u;
        g_pin_in = false;
        CHECK(cart_open(&d, g_now), "opened by hand");
        CHECK(g_pin_out, "powered");
        CHECK(!cart_open(&d, g_now), "and opening an open one does nothing");
        cart_close(&d, g_now, "test");
        CHECK(!g_pin_out && d.state == CART_OFF, "closed by hand");
        cart_close(&d, g_now, "test"); /* must be harmless */
        CHECK(!g_pin_out, "closing a closed one is harmless");
    }

    printf("\na zero interval is ON DEMAND ONLY: the clock never opens it, a command still can\n");
    {
        /* The inverse reading is the trap this pins down. The OFF test is `elapsed >= wait`, and
           with interval 0 the wait is 0, so without an explicit guard a zero interval would mean
           "open on every tick" -- the exact opposite of what it is configured to mean. */
        cart_config_t zero = small_config();
        zero.interval_ms = 0u;
        static cart_t d;
        g_now = 1000u;
        g_pin_in = false;
        g_pin_out = false;
        g_out_edges = 0;
        cart_init(&d, &zero, g_now);
        CHECK(run_ms(&d, 60000u, 0u) == CART_EVENT_NONE, "an hour of ticks, no beats: nothing happened");
        CHECK(d.state == CART_OFF && !g_pin_out, "still closed, still cold");
        CHECK(g_out_edges == 0, "and the pin was never driven, not even briefly");

        CHECK(cart_open(&d, g_now), "but a command opens it");
        CHECK(g_pin_out, "and the rail comes up");
        /* No beats, so the boot test must fail and cut it -- and the failure must not then be
           retried, because a retry wait is the interval plus a penalty, and there is no interval. */
        CHECK(run_ms(&d, 10000u, 0u) == CART_EVENT_NO_BOOT, "a silent cart still fails its boot");
        CHECK(!g_pin_out && d.state == CART_OFF, "which cuts the rail");
        CHECK(d.fails > 0, "and is counted a failure");
        const int edges = g_out_edges;
        CHECK(run_ms(&d, 120000u, 0u) == CART_EVENT_NONE, "yet no retry ever reopens it");
        CHECK(g_out_edges == edges, "the pin stayed exactly where the cut left it");
    }

    printf("\nand the rail is never left on by any ending above\n");
    CHECK(!g_pin_out, "off at the end of every case");

    printf("\n%s\n", fails == 0 ? "all ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
