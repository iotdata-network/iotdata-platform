
// ------------------------------------------------------------------------------------------------------------------------
// GNSS - ATGM336H / AT6558 (UART+PPS)
// https://wiki.wirenboard.com/wiki/images/5/53/AT6558-protocol-en.pdf
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_at6558 = "device-at6558";

#ifndef GNSS_UART_PORT
#define GNSS_UART_PORT UART_PORT_NUM
#endif

// ------------------------------------------------------------------------------------------------------------------------
//
// PINS. A board that multiplexes ONE uart across several devices -- as the SDS does, driving the
// ultrasonic, the GNSS and the radio in turn down the same pair -- names them once as
// PIN_DEVICE_UART_*, and these fall back to it so nothing there changes.
//
// A board that gives the GNSS its OWN pins defines PIN_GNSS_UART_* instead. That is not the same
// as needing a second uart controller: hw_uart_start(GNSS_UART_PORT, ) takes its pins and calls uart_set_pin(), and
// every driver here releases the port when it stops, so one controller can be re-pointed between
// devices as long as they are never wanted at the same moment. A sense-and-send cycle reads its
// sensors and only then powers the radio, so they are not.
//
// NOTE THE DIRECTION: _TX is the ESP's transmit, wired to the module's RX.
//
// ------------------------------------------------------------------------------------------------------------------------

#ifndef PIN_GNSS_UART_TX
#define PIN_GNSS_UART_TX PIN_DEVICE_UART_TX
#endif
#ifndef PIN_GNSS_UART_RX
#define PIN_GNSS_UART_RX PIN_DEVICE_UART_RX
#endif

/* PPS is optional and the driver already treats GPIO_NUM_NC as "not wired", so a board without one
   need say nothing at all -- which is most of them, since GNSS_WAIT_FOR_PPS is off by default. */
/* PARKING THE RADIO IS A SHARED-BUS MEASURE, and only that.
 *
 * On a board that drives the GNSS and the radio down ONE uart -- as the SDS does -- the radio has
 * to be told to stop driving the line before this driver takes it, and M0/M1 is how you say that
 * to an E22.
 *
 * On a board where they have separate controllers it is not merely unnecessary, it is DESTRUCTIVE:
 * LORA_MODE_DEEP_SLEEP is M1M0=11, the E22's configuration mode, and nothing here ever puts it
 * back while the GNSS is running. The radio goes deaf, and mute -- writes still succeed at the
 * uart because the module reads them as commands -- for as long as the receiver is up. On a node
 * that listens continuously, that is forever.
 *
 * So it is conditional on the two actually sharing a port, which is a question the ports can
 * answer for themselves. */
/* A RUNTIME COMPARISON, NOT #if. UART_NUM_0 and UART_NUM_1 are enum MEMBERS, not macros, so the
   preprocessor has never heard of either: `#if (GNSS_UART_PORT == E22_UART_PORT)` reads both as 0,
   finds them equal, and parks the radio on every board including the ones that must not. Written
   as C the comparison is real, and the compiler folds the dead branch away just the same. */
static inline bool _gnss_shares_radio_uart(void) {
    return (int)GNSS_UART_PORT == (int)E22_UART_PORT;
}

static inline void _gnss_park_radio(void) {
    if (_gnss_shares_radio_uart())
        _hack_lora_mode_deep_sleep_start();
}

static inline void _gnss_unpark_radio(void) {
    if (_gnss_shares_radio_uart())
        _hack_lora_mode_deep_sleep_end();
}

#ifndef PIN_GNSS_PPS
#ifdef PIN_DEVICE_GNSS_PPS
#define PIN_GNSS_PPS PIN_DEVICE_GNSS_PPS
#else
#define PIN_GNSS_PPS GPIO_NUM_NC
#endif
#endif

// ------------------------------------------------------------------------------------------------------------------------

// When GNSS_WAIT_FOR_PPS is defined and the PPS pin is not GPIO_NUM_NC, gnss_read()
// will yield/sleep until PPS goes high (indicating a fix) before parsing NMEA. This
// avoids burning CPU on no-fix sentences during cold start. Undefine to always parse.
#undef GNSS_WAIT_FOR_PPS

#define GNSS_FIX_TIMEOUT_MS_DEFAULT  (45 * 1000)
#define GNSS_NMEA_TIMEOUT_MS_DEFAULT (5 * 1000)
#define GNSS_MAX_FIX_RETRIES_DEFAULT 0
#define GNSS_MIN_SATELLITES_DEFAULT  0
#define GNSS_HDOP_MAX_DEFAULT        0.0f
#define GNSS_MIN_QUALITY_DEFAULT     0

typedef struct {
    uint32_t fix_timeout_ms;  // max time waiting for PPS/fix (currently hardcoded 90000)
    uint32_t nmea_timeout_ms; // max time parsing NMEA after PPS (currently 5000)
    uint8_t max_fix_retries;  // how many times to retry if fix fails (currently 0)
    uint8_t min_satellites;   // minimum sats to accept a fix; 0 = accept any
    uint8_t min_quality;      // minimum GGA fix quality; 0 = accept any non-zero
    float hdop_max;           // reject a fix worse than this HDOP; 0 = accept any
} gnss_strategy_t;

/* GGA fix quality (field 6). 6 is NOT a fix: it is the receiver's own estimate carried forward
   with nothing behind it, and it arrives looking exactly like a real one -- same fields, plausible
   position, satellite count and all. A node that only wants to know roughly where it is may take
   it; anything whose VALUE is the position being right should set min_quality to 1. */
#define GNSS_QUALITY_INVALID   0
#define GNSS_QUALITY_GPS       1
#define GNSS_QUALITY_DGPS      2
#define GNSS_QUALITY_PPS       3
#define GNSS_QUALITY_RTK_FIXED 4
#define GNSS_QUALITY_RTK_FLOAT 5
#define GNSS_QUALITY_ESTIMATED 6

const gnss_strategy_t STRAT_GNSS_DEFAULT = {
    .fix_timeout_ms = GNSS_FIX_TIMEOUT_MS_DEFAULT,
    .nmea_timeout_ms = GNSS_NMEA_TIMEOUT_MS_DEFAULT,
    .max_fix_retries = GNSS_MAX_FIX_RETRIES_DEFAULT, // XXX unused, yet
    .min_satellites = GNSS_MIN_SATELLITES_DEFAULT,   // 0 = disabled
    .min_quality = GNSS_MIN_QUALITY_DEFAULT,         // 0 = disabled
    .hdop_max = GNSS_HDOP_MAX_DEFAULT,               // 0 = disabled
};

// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    GNSS_READ_POSN = BIT(0), // request GGA (lat/lon/alt/sats/fix)
    GNSS_READ_TIME = BIT(1)  // request RMC (utc_time)
} gnss_read_flags_t;

typedef struct {
    double latitude;
    double longitude;
    float altitude; // m
    float hdop;     // horizontal dilution of precision
    int64_t utc_time;
    uint8_t satellites;
    uint8_t fix_quality; // 0=none, 1=GPS, 2=DGPS
    int nmea_count;
} gnss_reading_t;

typedef struct {
    gnss_strategy_t strategy;
} gnss_config_t;

const gnss_config_t gnss_config_default = {
    .strategy = STRAT_GNSS_DEFAULT,
};

// ------------------------------------------------------------------------------------------------------------------------

#define _GNSS_LAT_MIN        (-90.0)
#define _GNSS_LAT_MAX        (90.0)
#define _GNSS_LON_MIN        (-180.0)
#define _GNSS_LON_MAX        (180.0)

#define _GNSS_START_DELAY_MS 250
#define _GNSS_SLEEP_DELAY_MS 1

#define _GNSS_PPS_POLL_MS    100

// ------------------------------------------------------------------------------------------------------------------------

void _gnss_pins_enable(void) {
    if (PIN_GNSS_PPS != GPIO_NUM_NC)
        hw_gpio_cfg_enable_input(PIN_GNSS_PPS, false);
}

void _gnss_pins_disable(void) {
    if (PIN_GNSS_PPS != GPIO_NUM_NC)
        hw_gpio_cfg_disable(PIN_GNSS_PPS);
}

// ------------------------------------------------------------------------------------------------------------------------
//  NMEA parsing (standard NMEA 0183)
// ------------------------------------------------------------------------------------------------------------------------

#include "d_interface_nmea0183.h"

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    gnss_config_t config;
    nmea_txt_product_t product;
} _gnss_rtc_t;

#define _GNSS_RTC_MAGIC 0xDEAE6558
_RTC_DATA_STRUCT _gnss_rtc_t _gnss_rtc;
#define _GNSS_RTC_INIT()                _RTC_DATA_INIT(&_gnss_rtc, _GNSS_RTC_MAGIC)
#define _GNSS_RTC_VALID()               _RTC_DATA_VALID(&_gnss_rtc, _GNSS_RTC_MAGIC)

#define _GNSS_CONFIG(entry)             ((_gnss_rtc.config).entry)

// ------------------------------------------------------------------------------------------------------------------------

#define _GNSS_PRODUCT_ID_VALID(product) (true)

// ------------------------------------------------------------------------------------------------------------------------
//  PCAS command interface
// ------------------------------------------------------------------------------------------------------------------------

#define _PCAS_SEND_DELAY_MS             50
#define _PCAS_PCAS06_TIMEOUT_MS         2000

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _pcas_send(const char *const body) {

    // Build "$<body>*XX\r\n" with standard NMEA checksum (XOR of bytes between $ and *)
    uint8_t cksum = 0;
    for (const char *p = body; *p; p++)
        cksum ^= (uint8_t)*p;
    char cmd[64];
    const int len = snprintf(cmd, sizeof(cmd), "$%s*%02" PRIX8 "\r\n", body, cksum);
    ESP_RETURN_ON_FALSE(len > 0 && len <= (int)sizeof(cmd), ESP_ERR_INVALID_SIZE, __tag_device_at6558, "pcas send: invalid length %d (< 0 || > %d)", len, (int)sizeof(cmd));

    ESP_RETURN_ON_FALSE(hw_uart_write(GNSS_UART_PORT, (const uint8_t *)cmd, (size_t)len) == len, ESP_FAIL, __tag_device_at6558, "pcas send: uart write (len=%d)", len);
    hw_delay_ms_yieldable(_PCAS_SEND_DELAY_MS); // allow module to process

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _gnss_read_product(void) {

    _gnss_rtc.product = (nmea_txt_product_t){ 0 };

    const int txt_fields = 5;

    for (int i = 0; i <= txt_fields; i++) {
        char buf[sizeof("PCAS06,X") + 1];
        snprintf(buf, sizeof(buf), "PCAS06,%d", i);
        ESP_RETURN_ON_ERROR(_pcas_send(buf), __tag_device_at6558, "read_product: PCAS06");
    }

    int found = 0;
    _gnss_rx_init();
    const int64_t start_ms = hw_ticks_ms();
    while ((hw_ticks_ms() - start_ms) < _PCAS_PCAS06_TIMEOUT_MS && found < txt_fields) {
        char line[_NMEA_SENTENCE_LENGTH_MAX];
        if (_gnss_read_sentence(line, _NMEA_SENTENCE_LENGTH_MAX, _PCAS_PCAS06_TIMEOUT_MS, start_ms) && nmea_verify_checksum(line) && NMEA_SENTENCE(line, "TXT"))
            found += nmea_parse_txt(line, &_gnss_rtc.product) ? 1 : 0;
    }

    ESP_RETURN_ON_FALSE(found > 0, DEV_ERR_PRODUCT_ID, __tag_device_at6558, "read_product: not found");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

bool _gnss_dispatch(const char *const line, gnss_reading_t *const out, bool *const got_gga, bool *const got_rmc, const gnss_strategy_t *const st) {

    if (!nmea_verify_checksum(line)) {
        ESP_LOGD(__tag_device_at6558, "read: bad checksum: %.20s...", line);
        return false;
    }

    if (NMEA_SENTENCE(line, "GGA")) {
        if (!*got_gga) {
            gnss_reading_t tmp = *out;
            if (nmea_parse_gga(line, &tmp) == ESP_OK) {
                if (st->min_satellites > 0 && tmp.satellites < st->min_satellites)
                    ESP_LOGD(__tag_device_at6558, "read: GGA rejected: sats=%d (min=%d)", tmp.satellites, st->min_satellites);
                else if (st->hdop_max > 0.0f && tmp.hdop > st->hdop_max)
                    ESP_LOGD(__tag_device_at6558, "read: GGA rejected: hdop=%.1f (max=%.1f)", (double)tmp.hdop, (double)st->hdop_max);
                else {
                    *out = tmp;
                    *got_gga = true;
                    ESP_LOGD(__tag_device_at6558, "read: GGA: fix=%d sats=%d hdop=%.1f lat=%.6f lon=%.6f alt=%.1f", out->fix_quality, out->satellites, (double)out->hdop, (double)out->latitude, (double)out->longitude, (double)out->altitude);
                    return true;
                }
            }
        }

    } else if (NMEA_SENTENCE(line, "RMC")) {
        if (!*got_rmc) {
            if (nmea_parse_rmc(line, out) == ESP_OK) {
                if (true) {
                    *got_rmc = true;
                    ESP_LOGD(__tag_device_at6558, "read: RMC: time=%" PRIi64, out->utc_time);
                    return true;
                }
            }
        }
    }

    return false;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _gnss_validate(const gnss_reading_t *const out, const gnss_strategy_t *const st, const bool want_posn, const bool want_time, const bool got_gga, const bool got_rmc) {

    if (want_posn) {
        ESP_RETURN_ON_FALSE(got_gga, DEV_ERR_NO_FIX, __tag_device_at6558, "read: GGA timeout");
        ESP_RETURN_ON_FALSE(out->latitude >= _GNSS_LAT_MIN && out->latitude <= _GNSS_LAT_MAX && out->longitude >= _GNSS_LON_MIN && out->longitude <= _GNSS_LON_MAX, DEV_ERR_BAD_READING, __tag_device_at6558,
                            "read: GGA position out of range: lat=%.6f lon=%.6f", (double)out->latitude, (double)out->longitude);
        /* Each 0 means "do not care", so a caller that has not thought about it gets what it always got. */
        ESP_RETURN_ON_FALSE(st->min_quality == 0 || out->fix_quality >= st->min_quality, DEV_ERR_BAD_READING, __tag_device_at6558, "read: fix quality %u below %u (6 is an ESTIMATE, not a fix)", (unsigned)out->fix_quality,
                            (unsigned)st->min_quality);
        ESP_RETURN_ON_FALSE(st->min_satellites == 0 || out->satellites >= st->min_satellites, DEV_ERR_BAD_READING, __tag_device_at6558, "read: %u satellites, wanted %u", (unsigned)out->satellites, (unsigned)st->min_satellites);
        /* An HDOP of 0 means the field was absent, which is not the same as perfect: with a
           ceiling set, a fix that will not say how good it is cannot be shown to meet it. */
        ESP_RETURN_ON_FALSE(st->hdop_max <= 0.0f || (out->hdop > 0.0f && out->hdop <= st->hdop_max), DEV_ERR_BAD_READING, __tag_device_at6558, "read: hdop %.1f outside 0 < h <= %.1f", (double)out->hdop, (double)st->hdop_max);
    }

    if (want_time) {
        ESP_RETURN_ON_FALSE(got_rmc, DEV_ERR_TIMEOUT, __tag_device_at6558, "read: RMC timeout");
    }

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

#ifdef GNSS_WAIT_FOR_PPS
bool gnss_is_ready(void) {
    if (PIN_GNSS_PPS == GPIO_NUM_NC)
        return true; // no PPS pin — assume ready
    return hw_gpio_get(PIN_GNSS_PPS);
}
esp_err_t _gnss_wait_pps(const uint32_t fix_timeout_ms) {
    if (PIN_GNSS_PPS == GPIO_NUM_NC)
        return ESP_OK;
    ESP_LOGD(__tag_device_at6558, "read: PPS wait");
    const int64_t start = hw_ticks_ms();
    while ((hw_ticks_ms() - start) < fix_timeout_ms) {
        if (hw_gpio_get(PIN_GNSS_PPS)) {
            ESP_LOGD(__tag_device_at6558, "read: PPS after %dms", (int)(hw_ticks_ms() - start));
            ESP_RETURN_ON_ERROR(hw_uart_flush(GNSS_UART_PORT), __tag_device_at6558, "read: uart flush");
            return ESP_OK;
        }
        hw_delay_ms_yieldable(_GNSS_PPS_POLL_MS);
    }
    ESP_LOGE(__tag_device_at6558, "read: PPS timeout");
    return DEV_ERR_NO_FIX;
}
#endif

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_sleep(const bool deep) {

    // PCAS12,n: put module into standby/sleep mode with automatic wakeup after n seconds
    if (deep)
        ESP_RETURN_ON_ERROR(_pcas_send("PCAS12,120"), __tag_device_at6558, "sleep: PCAS12 standby/sleep");

    return ESP_OK;
}

esp_err_t gnss_quiet(void) {

    // Silence all NMEA output so the module doesn't chatter on the shared UART when idle
#define GNSS_PCAS_MESSAGE_ZERO "0,0,0,0,0,0,0,0"
    ESP_RETURN_ON_ERROR(_pcas_send("PCAS03,0,0,0,0,0,0,0,0"), __tag_device_at6558, "configure: PCAS03 msg");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_configure(void) {

    ESP_RETURN_ON_ERROR(gnss_quiet(), __tag_device_at6558, "configure: gnss_quiet");

    // Enable all available constellations (GPS + BDS + GLONASS)
#define GNSS_PCAS_SYSTEM_GPS_BDS_GLO 7
    ESP_RETURN_ON_ERROR(_pcas_send("PCAS04,7"), __tag_device_at6558, "configure: PCAS04 system");

    // Set navigation mode to stationary
#define GNSS_PCAS_NAV_MODE_STATIONARY 4
    ESP_RETURN_ON_ERROR(_pcas_send("PCAS11,4"), __tag_device_at6558, "configure: PCAS11 nav");

    return ESP_OK;
}

esp_err_t gnss_initialise(void) {

    // Set update rate to 1Hz (1000ms)
#define GNSS_PCAS_UPDATE_RATE_MS 1000
    ESP_RETURN_ON_ERROR(_pcas_send("PCAS02,1000"), __tag_device_at6558, "initialise: PCAS02 rate");

    // Enable only GGA and RMC, disable GLL/GSA/GSV/VTG/ZDA/TXT
    // Format: PCAS03,GGA,GLL,GSA,GSV,RMC,VTG,ZDA,TXT
#define GNSS_PCAS_MESSAGE_SET "1,0,0,0,1,0,0,0"
    ESP_RETURN_ON_ERROR(_pcas_send("PCAS03,1,0,0,0,1,0,0,0"), __tag_device_at6558, "initialise: PCAS03 msg");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_setup(const gnss_config_t *const config) {

    esp_err_t ret;

    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_GNSS_UART_TX));
    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_GNSS_UART_RX));
    ESP_ERROR_CHECK_BOOLEAN(PIN_GNSS_PPS == GPIO_NUM_NC || GPIO_IS_VALID_INPUT_GPIO(PIN_GNSS_PPS));

    _GNSS_RTC_INIT();
    _gnss_rtc.config = *config;

    ESP_LOGD(__tag_device_at6558, "setup: strategy=(fix_timeout_ms=%" PRIu32 "/nmea_timeout_ms=%" PRIu32 "/max_fix_retries=%d/min_satellites=%d/hdop_max=%.1f)", _GNSS_CONFIG(strategy).fix_timeout_ms, _GNSS_CONFIG(strategy).nmea_timeout_ms,
             _GNSS_CONFIG(strategy).max_fix_retries, _GNSS_CONFIG(strategy).min_satellites, (double)_GNSS_CONFIG(strategy).hdop_max);

    // _hack_lora_mode_deep_sleep_start();
    // _gnss_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(GNSS_UART_PORT, PIN_GNSS_UART_TX, PIN_GNSS_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_DEFAULT, UART_TX_BUF_SIZE_MIN), __tag_device_at6558, "setup: uart start");

    hw_delay_ms_yieldable(_GNSS_START_DELAY_MS); // allow module to boot
    ESP_GOTO_ON_ERROR(hw_uart_flush(GNSS_UART_PORT), gnss_setup_failed, __tag_device_at6558, "setup: uart flush");

    (void)_gnss_read_product();
    char info[128];
    ESP_LOGD(__tag_device_at6558, "product: %s", nmea_format_txt_product(&_gnss_rtc.product, info, sizeof(info)));
    ESP_GOTO_ON_FALSE(_GNSS_PRODUCT_ID_VALID(_gnss_rtc.product), DEV_ERR_PRODUCT_ID, gnss_setup_failed, __tag_device_at6558, "setup: product invalid");

    if ((ret = gnss_configure()) != ESP_OK)
        ESP_LOGW(__tag_device_at6558, "setup: configured failed (%s), will retry at start", esp_err_to_name(ret));

    ESP_RETURN_ON_ERROR(gnss_quiet(), __tag_device_at6558, "setup: gnss_quiet");
    ESP_RETURN_ON_ERROR(gnss_sleep(false), __tag_device_at6558, "setup: gnss_sleep");

    hw_uart_stop(GNSS_UART_PORT);

    char info2[128];
    ESP_LOGI(__tag_device_at6558, "setup: %s", nmea_format_txt_product(&_gnss_rtc.product, info2, sizeof(info2)));

    return ESP_OK;

gnss_setup_failed:
    (void)gnss_sleep(true);
    hw_uart_stop(GNSS_UART_PORT);
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_start(void) {

    ESP_RETURN_ON_FALSE(_GNSS_RTC_VALID(), DEV_ERR_RTC, __tag_device_at6558, "start: rtc invalid");
    ESP_RETURN_ON_FALSE(_GNSS_PRODUCT_ID_VALID(_gnss_rtc.product), DEV_ERR_PRODUCT_ID, __tag_device_at6558, "start: product invalid");

    esp_err_t ret;

    _gnss_park_radio();
    _gnss_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(GNSS_UART_PORT, PIN_GNSS_UART_TX, PIN_GNSS_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_DEFAULT, UART_TX_BUF_SIZE_MIN), __tag_device_at6558, "start: uart start");

    hw_delay_ms_yieldable(_GNSS_START_DELAY_MS);
    ESP_GOTO_ON_ERROR(hw_uart_flush(GNSS_UART_PORT), gnss_start_failed, __tag_device_at6558, "start: uart flush");

    if ((ret = gnss_configure()) != ESP_OK || (ret = gnss_initialise()) != ESP_OK)
        ESP_LOGW(__tag_device_at6558, "start: configure/intialise failed (%s), using defaults", esp_err_to_name(ret));

    ESP_LOGI(__tag_device_at6558, "started");

    return ESP_OK;

gnss_start_failed:
    (void)gnss_sleep(true);
    hw_uart_stop(GNSS_UART_PORT);
    _gnss_pins_disable();
    _gnss_unpark_radio();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_read(gnss_reading_t *const out, const gnss_read_flags_t flags, const gnss_strategy_t *const strategy) {

    const gnss_strategy_t *st = strategy ? strategy : &_GNSS_CONFIG(strategy);

    *out = (gnss_reading_t){ 0 };

#ifdef GNSS_WAIT_FOR_PPS
    ESP_RETURN_ON_ERROR(_gnss_wait_pps(st->fix_timeout_ms), __tag_device_at6558, "read: PPS");
    const uint32_t read_timeout_ms = st->nmea_timeout_ms;
#else
    const uint32_t read_timeout_ms = st->fix_timeout_ms;
#endif

    const bool want_posn = (flags & GNSS_READ_POSN) != 0;
    const bool want_time = (flags & GNSS_READ_TIME) != 0;
    bool got_gga = !want_posn;
    bool got_rmc = !want_time;

    const int64_t start = hw_ticks_ms();

    _gnss_rx_init();
    int failed_count = 0;
    while ((hw_ticks_ms() - start) < read_timeout_ms) {
        char line[_NMEA_SENTENCE_LENGTH_MAX];
        if (_gnss_read_sentence(line, _NMEA_SENTENCE_LENGTH_MAX, st->nmea_timeout_ms, hw_ticks_ms()) > 0) {
            ESP_LOGD(__tag_device_at6558, "gnss_read: sentence=%s", line);
            out->nmea_count++;
            if (_gnss_dispatch(line, out, &got_gga, &got_rmc, st))
                if ((got_gga && got_rmc))
                    break;
        } else
            failed_count++;
        hw_delay_ms_yieldable(50);
    }
    if (failed_count > 0)
        ESP_LOGD(__tag_device_at6558, "gnss_read: %d failures", failed_count);

    ESP_RETURN_ON_ERROR(_gnss_validate(out, st, want_posn, want_time, got_gga, got_rmc), __tag_device_at6558, "read: validate");

    ESP_LOGI(__tag_device_at6558, "read: nmea=%d fix=%d lat=%.6f lon=%.6f alt=%.1fm hdop=%.1f sats=%d time=%" PRIi64, out->nmea_count, out->fix_quality, (double)out->latitude, (double)out->longitude, (double)out->altitude,
             (double)out->hdop, out->satellites, out->utc_time);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

/* ATTACH / DETACH -- the uart, and ONLY the uart.
 *
 * gnss_start() and gnss_stop() also wake and sleep the RECEIVER, which is what a board that
 * power-gates its GNSS wants: nothing should be drawing current between cycles. But a module left
 * running keeps its almanac, its ephemeris and its fix, and re-acquiring after a soft sleep costs
 * a cold start every cycle -- which on a node that samples every minute is most of the cycle.
 *
 * So a board that leaves the module powered and merely needs to SHARE one uart controller between
 * the GNSS and the radio hands the port back and forth with these instead. The module never stops
 * tracking; only our listening does, and sentences missed while the radio has the port are
 * sentences we did not want.
 *
 * The lora parking is kept: the E22 is told to stop driving before we take the line, which costs
 * nothing on a board where the two are on separate pins and is essential on one where they are not.
 */

/* How good the fix was, for a receiver that has to decide whether to believe the position.
 *
 * METADATA, NOT A GATE. Rejecting a poor fix at the node throws away the only evidence the node
 * was ever there; carrying its quality lets the decision be made later by something with the whole
 * picture. Reject what is not a fix at all -- see min_quality above -- and carry what is merely a
 * poor one. A survey point at HDOP 2 and one at HDOP 10 are both real; only one is worth metres.
 *
 *   +0  quality(u8)      GGA field 6 as reported: 1 GPS, 2 DGPS, 6 ESTIMATED
 *   +1  satellites(u8)   GGA field 7
 *   +2  hdop_x10(u8)     HDOP x10; 0 = not reported, 255 = 25.5 or worse (meaningless either way)
 *   +3  altitude(i16)    metres, big-endian
 *
 * The TLV TYPE is deliberately not chosen here: 0x20 upwards is application space, so the number
 * this travels under belongs to whichever application allocates it, not to the driver.
 */

// #define GNSS_QUALITY_TLV_SIZE 5

// int gnss_quality_tlv_pack(const gnss_reading_t *const r, uint8_t *const buf, const size_t cap) {
//     if (r == NULL || buf == NULL || cap < (size_t)GNSS_QUALITY_TLV_SIZE)
//         return 0;
//     const int hdop_x10 = (r->hdop > 0.0f) ? (int)(r->hdop * 10.0f + 0.5f) : 0;
//     const int alt_m = (int)(r->altitude + (r->altitude < 0.0f ? -0.5f : 0.5f));
//     const int16_t alt = (int16_t)(alt_m < -32768 ? -32768 : (alt_m > 32767 ? 32767 : alt_m));
//     buf[0] = r->fix_quality;
//     buf[1] = r->satellites;
//     buf[2] = (uint8_t)(hdop_x10 < 0 ? 0 : (hdop_x10 > 255 ? 255 : hdop_x10));
//     buf[3] = (uint8_t)((uint16_t)alt >> 8);
//     buf[4] = (uint8_t)((uint16_t)alt & 0xFFu);
//     return GNSS_QUALITY_TLV_SIZE;
// }

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_attach(void) {
    _gnss_park_radio();
    _gnss_pins_enable();
    ESP_RETURN_ON_ERROR(hw_uart_start(GNSS_UART_PORT, PIN_GNSS_UART_TX, PIN_GNSS_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_DEFAULT, UART_TX_BUF_SIZE_MIN), __tag_device_at6558, "attach: uart start");
    hw_delay_ms_yieldable(_GNSS_START_DELAY_MS);
    return hw_uart_flush(GNSS_UART_PORT);
}

void gnss_detach(void) {
    hw_uart_stop(GNSS_UART_PORT);
    _gnss_pins_disable();
    _gnss_unpark_radio();
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_stop(void) {

    if (gnss_sleep(true) == ESP_OK)
        if (_GNSS_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_GNSS_SLEEP_DELAY_MS);
    hw_uart_stop(GNSS_UART_PORT);
    _gnss_pins_disable();
    _gnss_unpark_radio();

    ESP_LOGI(__tag_device_at6558, "stopped");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t gnss_test(device_test_result_t *const result, const uint32_t duration_ms) {

    result->passed = false;
    const uint32_t sleeping_ms = 1 * 1000;
    const int64_t start_ms = hw_ticks_ms();

    esp_err_t rc;
    gnss_reading_t reading;

    if ((rc = gnss_setup(&gnss_config_default)) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "setup failed: %s", esp_err_to_name(rc));
        return rc;
    }

    if ((rc = gnss_start()) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "start failed: %s", esp_err_to_name(rc));
        return rc;
    }

    int nmea_count = 0;
    int failures = 5;
    while (rc == ESP_OK && (hw_ticks_ms() - start_ms) < duration_ms) {
        ESP_LOGD(__tag_device_at6558, "%s: gnss_read", __func__);
        gnss_strategy_t strategy = STRAT_GNSS_DEFAULT;
        strategy.fix_timeout_ms = (uint32_t)duration_ms - (uint32_t)(hw_ticks_ms() - start_ms);
        if ((rc = gnss_read(&reading, GNSS_READ_POSN | GNSS_READ_TIME, &strategy)) != ESP_OK && --failures > 0)
            rc = ESP_OK;
        else
            nmea_count += reading.nmea_count;
        if ((hw_ticks_ms() - start_ms) < duration_ms)
            hw_delay_ms_yieldable((uint32_t)sleeping_ms);
    }
    if (!nmea_count)
        rc = DEV_ERR_NOT_READY;

    esp_err_t rc2;
    if ((rc2 = gnss_stop()) != ESP_OK && rc == ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "stop failed: %s", esp_err_to_name(rc));
        return rc2;
    }

    if (rc != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "read failed: %s", esp_err_to_name(rc));
        return rc;
    }

    result->passed = true;
    snprintf(result->detail, sizeof(result->detail), "nmea=%d", nmea_count);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
