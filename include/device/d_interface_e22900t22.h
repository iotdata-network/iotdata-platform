
// ------------------------------------------------------------------------------------------------------------------------
// LoRa - E22-900T22 (UART, transparent mode)
// https://github.com/matthewgream/e22900t22/blob/main/specs/E22-900T22D_UserManual_EN_v1.3.pdf
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_e22900t22 = "device-e22900t22";

#ifndef E22_UART_PORT
#define E22_UART_PORT UART_PORT_NUM
#endif

// ------------------------------------------------------------------------------------------------------------------------

typedef enum {
    LORA_MODULE_DIP = 0,
    LORA_MODULE_USB = 1,
} lora_module_t;

typedef enum {
    LORA_MODE_NORMAL,
    LORA_MODE_WAKE_ON_RECEIVE,
    LORA_MODE_CONFIG,
    LORA_MODE_DEEP_SLEEP
} lora_mode_t;

#define LORA_E22_ADDRESS_DEFAULT            0x0008
#define LORA_E22_NETWORK_DEFAULT            0x00
#define LORA_CHANNEL_DEFAULT                0x0F
#define LORA_TRANSMIT_POWER_DEFAULT         22
#define LORA_PACKET_RATE_DEFAULT            2400
#define LORA_PACKET_SIZE_DEFAULT            240
#define LORA_LISTEN_BEFORE_TRANSMIT_DEFAULT true
#define LORA_CRYPT_DEFAULT                  0x0000
#define LORA_RSSI_PACKET_DEFAULT            true
#define LORA_RSSI_CHANNEL_DEFAULT           false
#define LORA_DEBUG_DEFAULT                  false

#define LORA_E22_ADDRESS_MONITOR            0xFFFF

typedef struct {
    lora_module_t module;
    uint16_t e22_address;
    uint8_t e22_network;
    uint8_t channel;
    uint8_t transmit_power;
    uint16_t air_data_rate;
    uint8_t packet_size;
    bool listen_before_transmit;
    uint16_t crypt;
    bool rssi_packet;
    bool rssi_channel;
    bool debug;
} lora_config_t;

const lora_config_t lora_config_default = {
    .module = LORA_MODULE_DIP,
    .e22_address = LORA_E22_ADDRESS_DEFAULT,
    .e22_network = LORA_E22_NETWORK_DEFAULT,
    .channel = LORA_CHANNEL_DEFAULT,
    .transmit_power = LORA_TRANSMIT_POWER_DEFAULT,
    .air_data_rate = LORA_PACKET_RATE_DEFAULT,
    .packet_size = LORA_PACKET_SIZE_DEFAULT,
    .listen_before_transmit = LORA_LISTEN_BEFORE_TRANSMIT_DEFAULT,
    .crypt = LORA_CRYPT_DEFAULT,
    .rssi_packet = LORA_RSSI_PACKET_DEFAULT,
    .rssi_channel = LORA_RSSI_CHANNEL_DEFAULT,
    .debug = LORA_DEBUG_DEFAULT,
};

static inline uint8_t _lora_e22_air_data_rate(uint16_t rate) {
    if (rate == 300)
        return 0x00;
    else if (rate == 1200)
        return 0x01;
    else if (rate == 2400)
        return 0x02;
    else if (rate == 4800)
        return 0x03;
    else if (rate == 9600)
        return 0x04;
    else if (rate == 19200)
        return 0x05;
    else if (rate == 38400)
        return 0x06;
    else if (rate == 62500)
        return 0x07;
    else
        return 0x02;
}
#define _LORA_E22_CONFIG_AIR_DATA_RATE(n)  ((uint8_t)(_lora_e22_air_data_rate(n) & 0x07))
#define _LORA_E22_CONFIG_TRANSMIT_POWER(n) ((uint8_t)(((n) == 10) ? 0x03 : ((n) == 13 ? 0x02 : ((n) == 17 ? 0x01 : 0x00))))
#define _LORA_E22_CONFIG_PACKET_SIZE(n)    ((uint8_t)((((n) == 32) ? 0x03 : ((n) == 64 ? 0x02 : ((n) == 128 ? 0x01 : 0x00))) << 6))
#define _LORA_E22_CONFIG_LBT(n)            ((uint8_t)(((n) ? 0x01 : 0x00) << 4))

#define LORA_PACKET_SIZE_MAX               240

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_READY_DELAY_MS               10
/*
 * USB flow control, such as it is.
 *
 * On a DIP module AUX tells us when the module will accept a command, and every command waits on
 * it. A USB module exposes no AUX, so there is nothing to wait FOR -- and issuing a command before
 * the dongle is ready gets no response at all, not an error. The only available substitute is time.
 */
#define _LORA_USB_SETTLE_MS                50
#define _LORA_SETUP_DELAY_MS               100
#define _LORA_SETTLE_DELAY_MS              500
/*
 * Response budget for a command. Generous on purpose: a config WRITE commits to the module's own
 * NVM before it answers, and the reference implementation effectively allowed far more than its
 * nominal 1000ms -- its serial_read waited that long for the FIRST byte and then permitted 100ms
 * per byte after, so a 12-byte response had ~2.1s. hw_uart_read is a total budget (matching
 * uart_read_bytes on the chip), so the number has to cover the whole exchange.
 */
#define _LORA_CMD_TIMEOUT_MS               3000
/* A config WRITE commits the register block to the module's own NVM before it answers, and only
   then sends the echo. Observed at ~3s on a USB dongle, i.e. right on the general command budget,
   so it gets its own. */
#define _LORA_CMD_TIMEOUT_SAVE_MS          8000
/*
 * How long to wait for AUX to say the module has drained -- see _lora_transmit_wait_ms(). These
 * bound a value derived from the configured air rate rather than replacing it.
 */
#define _LORA_TRANSMIT_WAIT_MIN_MS         1000  /* the fast rates make the arithmetic tiny */
#define _LORA_TRANSMIT_WAIT_MAX_MS         20000 /* beyond this the module is not merely busy */
#define _LORA_SLEEP_DELAY_MS               5
#define _LORA_TRANSMIT_DELAY_MS            30000
#define _LORA_SAVE_DELAY_MS                100

// ------------------------------------------------------------------------------------------------------------------------

// E22 register layout (read from offset 0x00, 9 bytes)
// [0] ADDH  [1] ADDL  [2] NETID  [3] REG0  [4] REG1  [5] CH  [6] REG3
// [7] CRYPT_H  [8] CRYPT_L
#define _LORA_E22_REG_SIZE_CONFIG_READ     9
#define _LORA_E22_REG_SIZE_CONFIG_WRITE    9
#define _LORA_E22_REG_SIZE_PRODUCT_READ    7
#define _LORA_E22_CMD_SIZE_HEADER          3

#define _LORA_E22_REG1_OFFSET              4 /* ADDH ADDL NETID REG0 [REG1] CH REG3 ... */

// ------------------------------------------------------------------------------------------------------------------------

/*
 * The module reports RSSI as 256 + dBm, which only fits a byte for -1 dBm and weaker. A signal at
 * 0 dBm or stronger -- what a 22 dBm module a few centimetres away actually delivers -- WRAPS it
 * to 0x00, and the naive decode then reports -256 dBm: the weakest possible link, for the
 * strongest possible signal.
 *
 * That is not just a wrong number in a log. relay_mesh.h ranks candidate parents by cost and then
 * by RSSI, so a wrapped neighbour is scored worst of all, can never win a tie on cost, and can
 * never clear the changeover hysteresis. The closest relay on the bench was the one the mesh was
 * most reluctant to use.
 *
 * Nothing below the receiver's sensitivity can be a measurement, so anything under the floor is
 * read as the saturation it is and reported as "at least 0 dBm" -- which is both true and sorts
 * correctly against every real reading.
 */
/*
 * What a reading MEANS at this boundary, so nothing above it has to know about module bytes:
 *
 *   <= 0     dBm, always. 0 is a saturated reading -- "at least this strong".
 *   NONE     no reading was available (a fragment, or a read that never reached the RSSI byte).
 *
 * The sentinel exists because 0 became a legitimate value the moment saturation was handled, and
 * a caller using "0 means nothing arrived" would then silently discard the strongest signals it
 * ever sees. Every genuine reading is negative or zero, so a positive value can carry the absence.
 */
#define LORA_RSSI_NONE                     1      /* not a dBm value: no reading was available */
#define _LORA_E22_RSSI_FLOOR_DBM           (-148) /* SX126x sensitivity: the weakest genuine reading */
#define _LORA_E22_RSSI_SATURATED_DBM       0      /* wrapped: at least this strong, and we cannot say more */

static inline int _e22_rssi_dbm(const uint8_t raw) {
    const int dbm = -(256 - (int)raw);
    return (dbm < _LORA_E22_RSSI_FLOOR_DBM) ? _LORA_E22_RSSI_SATURATED_DBM : dbm;
}

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    lora_config_t config;
    uint8_t product[_LORA_E22_REG_SIZE_PRODUCT_READ];
} _lora_rtc_t;

#define _LORA_RTC_MAGIC 0xDEE210AA
_RTC_DATA_STRUCT _lora_rtc_t _lora_rtc;
#define _LORA_RTC_INIT()                _RTC_DATA_INIT(&_lora_rtc, _LORA_RTC_MAGIC)
#define _LORA_RTC_VALID()               _RTC_DATA_VALID(&_lora_rtc, _LORA_RTC_MAGIC)

#define _LORA_CONFIG(entry)             ((_lora_rtc.config).entry)

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_PRODUCT_ID_VALID(product) (true)

// ------------------------------------------------------------------------------------------------------------------------

#define _LORA_IS_USB()                  (_LORA_CONFIG(module) == LORA_MODULE_USB)

static inline uint32_t _lora_transmit_wait_ms(void) {
    const uint16_t bps = _LORA_CONFIG(air_data_rate);
    if (bps == 0)
        return _LORA_TRANSMIT_WAIT_MIN_MS;
    const uint32_t air_ms = ((uint32_t)LORA_PACKET_SIZE_MAX * 8u * 1000u) / (uint32_t)bps;
    const uint32_t budget = air_ms * 2u;
    if (budget < _LORA_TRANSMIT_WAIT_MIN_MS)
        return _LORA_TRANSMIT_WAIT_MIN_MS;
    return budget > _LORA_TRANSMIT_WAIT_MAX_MS ? _LORA_TRANSMIT_WAIT_MAX_MS : budget;
}

void _lora_pins_enable(void) {
    if (!_LORA_IS_USB()) {
        gpio_deep_sleep_hold_dis();
        (void)gpio_hold_dis(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            (void)gpio_hold_dis(PIN_DEVICE_LORA_M0);
        hw_gpio_revoke_two(PIN_DEVICE_LORA_M0, PIN_DEVICE_LORA_M1);
        hw_gpio_cfg_enable_input(PIN_DEVICE_LORA_AUX, false);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M0);
        hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M1);
    }
}

void _lora_pins_disable(void) {
    if (!_LORA_IS_USB()) {
        hw_gpio_revoke_two(PIN_DEVICE_LORA_M0, PIN_DEVICE_LORA_M1);
        hw_gpio_cfg_disable(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
            hw_gpio_cfg_disable(PIN_DEVICE_LORA_M0);
        hw_gpio_cfg_disable(PIN_DEVICE_LORA_AUX);
    }
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * TRANSMIT BACK-PRESSURE, and the USB module has none of its own.
 *
 * On DIP, AUX is the module telling us its buffer has room, and lora_write() waits on it. USB
 * exposes no AUX at all, so this answered `true` unconditionally -- meaning lora_write()'s
 * "wait for ready" was a no-op and the host could hand the module a second frame while it was
 * still radiating the first. With listen-before-transmit on, how long that takes is not even
 * predictable: the module waits for a clear channel first.
 *
 * What that cost, observed live on a gateway: three phantom stations in two logs, each one the
 * gateway's OWN BEACON transmitted with its leading byte missing. `F0 01 11 2F 00 01 00 12 64`
 * went out as `01 11 2F 00 01 00 12 64`, which is a syntactically perfect header for a station
 * that does not exist, so every relay that heard it invented a neighbour and forwarded it. Each
 * one was a beacon whose mesh sequence was exactly ACK+1, sent ~220ms behind that ACK in the same
 * loop pass, where ack-to-ack spacing of ~390ms never failed. The module drops the leading byte
 * of a frame written while it is busy.
 *
 * Time is the only substitute for AUX here, so the guard is a minimum interval between writes.
 * It is deliberately a crude floor rather than a computed air time: with LBT the module's busy
 * period is not a function of frame length, so a formula would be a guess dressed up as physics.
 * 400ms is the spacing that was never observed to fail; tune it against a log, not against the
 * bit rate.
 */
#ifndef _LORA_TX_GUARD_USB_MS
#define _LORA_TX_GUARD_USB_MS 400
#endif

static uint32_t _lora_tx_guard_until_ms = 0;

bool lora_is_ready(void) {
    return _LORA_IS_USB() ? ((int32_t)(hw_time_ms() - _lora_tx_guard_until_ms) >= 0) : hw_gpio_get(PIN_DEVICE_LORA_AUX);
}

static inline void _lora_settle(void) {
    if (_LORA_IS_USB())
        hw_delay_ms_yieldable(_LORA_USB_SETTLE_MS);
}

D_WAIT_READY_FUNC(lora_wait_ready, lora_is_ready, _LORA_READY_DELAY_MS, true, __tag_device_e22900t22)

// ------------------------------------------------------------------------------------------------------------------------

static bool _lora_cmd_send(const uint8_t *const cmd, const size_t cmd_len) {
    _lora_settle();
    if (!lora_wait_ready(_lora_transmit_wait_ms()))
        return false;
    if (hw_uart_flush(E22_UART_PORT) != ESP_OK)
        return false;
    if (hw_uart_write(E22_UART_PORT, cmd, cmd_len) != (int)cmd_len)
        return false;
    _lora_settle();
    return true;
}

static int _lora_cmd_xfer_to(const uint8_t *const cmd, const size_t cmd_len, uint8_t *const res, const size_t res_len, const int timeout_ms) {
    if (!_lora_cmd_send(cmd, cmd_len))
        return -1;
    if (!lora_wait_ready(_lora_transmit_wait_ms()))
        return -1;
    return hw_uart_read(E22_UART_PORT, res, res_len, timeout_ms);
}

static int _lora_cmd_xfer(const uint8_t *const cmd, const size_t cmd_len, uint8_t *const res, const size_t res_len) {
    return _lora_cmd_xfer_to(cmd, cmd_len, res, res_len, _LORA_CMD_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_mode_set_dip(const lora_mode_t mode) {
    bool m0 = false, m1 = false;
    if (mode == LORA_MODE_WAKE_ON_RECEIVE)
        m0 = true, m1 = false;
    else if (mode == LORA_MODE_CONFIG)
        m0 = false, m1 = true;
    else if (mode == LORA_MODE_DEEP_SLEEP)
        m0 = true, m1 = true;
    // else if (mode == LORA_MODE_NORMAL)          m0 = false, m1 = false;
    hw_gpio_set(PIN_DEVICE_LORA_M1, m1);
    if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC)
        hw_gpio_set(PIN_DEVICE_LORA_M0, m0);
    return mode == LORA_MODE_DEEP_SLEEP || lora_wait_ready(_LORA_SETTLE_DELAY_MS) ? ESP_OK : DEV_ERR_TIMEOUT;
}

esp_err_t _lora_mode_set_usb(const lora_mode_t mode) {
    uint8_t mode_byte;
    if (mode == LORA_MODE_NORMAL)
        mode_byte = 0x00;
    else if (mode == LORA_MODE_CONFIG)
        mode_byte = 0x01;
    else if (mode == LORA_MODE_WAKE_ON_RECEIVE)
        mode_byte = 0x02;
    else
        mode_byte = 0x03; // LORA_MODE_DEEP_SLEEP
    const uint8_t cmd[] = { 0xC0, 0xC1, 0xC2, 0xC3, 0x02, mode_byte };
    if (mode == LORA_MODE_DEEP_SLEEP) {
        ESP_RETURN_ON_FALSE(_lora_cmd_send(cmd, sizeof(cmd)), ESP_FAIL, __tag_device_e22900t22, "mode set (usb): sleep command not written");
    } else {
        uint8_t res[sizeof(cmd)];
        const int want = (int)sizeof(cmd) - 1;
        const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, (size_t)want);
        if (n == 3 && res[0] == 0xFF && res[1] == 0xFF && res[2] == 0xFF) {
            /*
             * WARN, not DEBUG, and read the message carefully before believing it.
             *
             * FF FF FF is documented nowhere. The reference implementation calls it "already appears to
             * be in required mode, will accept" and returns success -- and that reading cost real
             * debugging time, because it is at least equally consistent with a plain NACK. Observed
             * with REG1 bit 2 (switch-config-serial) CLEAR: every mode command answers FF FF FF while
             * the module stays put, config reads keep working, and data writes come back as FF FF FF
             * too. Nothing looks broken; nothing works.
             *
             * It is NOT on its own evidence that bit 2 is clear -- a deep-sleep command answers this
             * way with bit 2 perfectly set, which is why that mode no longer reaches here at all.
             *
             * So it is accepted as success (the module may genuinely be in the mode we asked for) but
             * it is now audible. If you see it for NORMAL or CONFIG, read the REG1 value the setup
             * logs and check bit 2 rather than guessing.
             */
            ESP_LOGW(__tag_device_e22900t22, "mode set (usb): mode %d not confirmed (FF FF FF) -- accepted, but check the REG1 value if this repeats", (int)mode);
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(n >= want, ESP_FAIL, __tag_device_e22900t22, "mode set (usb): got %d bytes, expected %d", n, want);
    }
    return ESP_OK;
}

esp_err_t _lora_mode_set(const lora_mode_t mode) {
    return _LORA_IS_USB() ? _lora_mode_set_usb(mode) : _lora_mode_set_dip(mode);
}

void lora_hold(void) {
    if (!_LORA_IS_USB()) {
        hw_gpio_set(PIN_DEVICE_LORA_M1, true);
        hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M1);
        (void)gpio_hold_en(PIN_DEVICE_LORA_M1);
        if (PIN_DEVICE_LORA_M0 != GPIO_NUM_NC) {
            hw_gpio_set(PIN_DEVICE_LORA_M0, true);
            hw_gpio_cfg_enable_output(PIN_DEVICE_LORA_M0);
            (void)gpio_hold_en(PIN_DEVICE_LORA_M0);
        }
        gpio_deep_sleep_hold_en();
    }
}

void _hack_lora_mode_deep_sleep_start(void) {
    _lora_pins_enable();
    _lora_mode_set(LORA_MODE_DEEP_SLEEP);
}
void _hack_lora_mode_deep_sleep_end(void) {
    _lora_pins_disable();
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_read_config(uint8_t *const config_data) {

    const uint8_t cmd[] = { 0xC1, 0x00, _LORA_E22_REG_SIZE_CONFIG_READ };
    uint8_t resp[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_READ];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), resp, sizeof(resp));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(resp), ESP_FAIL, __tag_device_e22900t22, "config read: got %d bytes, expected %d", n, (int)sizeof(resp));
    ESP_RETURN_ON_FALSE(resp[0] == 0xC1 && resp[1] == 0x00 && resp[2] == _LORA_E22_REG_SIZE_CONFIG_READ, ESP_FAIL, __tag_device_e22900t22, "config read: bad header %02" PRIX8 " %02" PRIX8 " %02" PRIX8, resp[0], resp[1], resp[2]);
    memcpy(config_data, resp + _LORA_E22_CMD_SIZE_HEADER, _LORA_E22_REG_SIZE_CONFIG_READ);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_write_config(const uint8_t *const config_data) {

    uint8_t cmd[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_WRITE] = { 0xC0, 0x00, _LORA_E22_REG_SIZE_CONFIG_WRITE };
    memcpy(cmd + _LORA_E22_CMD_SIZE_HEADER, config_data, _LORA_E22_REG_SIZE_CONFIG_WRITE);
    ESP_LOGD(__tag_device_e22900t22, "config_write: len=%d", (int)sizeof(cmd));
    uint8_t res[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_CONFIG_WRITE];
    const int n = _lora_cmd_xfer_to(cmd, sizeof(cmd), res, sizeof(res), _LORA_CMD_TIMEOUT_SAVE_MS);
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "config write: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1, ESP_FAIL, __tag_device_e22900t22, "config write: bad response header 0x%02" PRIX8, res[0]);
    for (int i = 0; i < _LORA_E22_REG_SIZE_CONFIG_WRITE; i++)
        ESP_RETURN_ON_FALSE(res[_LORA_E22_CMD_SIZE_HEADER + i] == config_data[i], ESP_FAIL, __tag_device_e22900t22, "config write: verify fail at [%d]: wrote 0x%02" PRIX8 ", read 0x%02" PRIX8, i, config_data[i],
                            res[_LORA_E22_CMD_SIZE_HEADER + i]);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_cmd_read_product(uint8_t *const info_data) {

    const uint8_t cmd[] = { 0xC1, 0x80, _LORA_E22_REG_SIZE_PRODUCT_READ };
    uint8_t res[_LORA_E22_CMD_SIZE_HEADER + _LORA_E22_REG_SIZE_PRODUCT_READ];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, sizeof(res));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "product read: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1, ESP_FAIL, __tag_device_e22900t22, "product read: bad response header 0x%02" PRIX8, res[0]);
    memcpy(info_data, res + _LORA_E22_CMD_SIZE_HEADER, _LORA_E22_REG_SIZE_PRODUCT_READ);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_read_product(void) {

    ESP_RETURN_ON_ERROR(_lora_cmd_read_product(_lora_rtc.product), __tag_device_e22900t22, "read_product: cmd_read_product");
    ESP_LOGD(__tag_device_e22900t22, "product: name=%d, ver=%d, power=%ddBm, freq=%d, type=%d", (int)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3], _lora_rtc.product[4],
             _lora_rtc.product[5]);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _lora_read_and_update_config(void) {

    const uint8_t req_config[_LORA_E22_REG_SIZE_CONFIG_WRITE] = {
        (uint8_t)((_LORA_CONFIG(e22_address) >> 8)),                        // [ADDH = 0x00]
        (uint8_t)((_LORA_CONFIG(e22_address) & 0xFF)),                      // [ADDL = 0x08]
        _LORA_CONFIG(e22_network),                                          // [NETID = 0x00]
        0x60 | _LORA_E22_CONFIG_AIR_DATA_RATE(_LORA_CONFIG(air_data_rate)), // REG0: 0x60 = UART 9600/8N1, [air rate]
        (uint8_t)((_LORA_IS_USB() ? 0x04 : 0) | (_LORA_CONFIG(rssi_channel) ? 0x20 : 0) | _LORA_E22_CONFIG_PACKET_SIZE(_LORA_CONFIG(packet_size)) | _LORA_E22_CONFIG_TRANSMIT_POWER(_LORA_CONFIG(transmit_power))),
        _LORA_CONFIG(channel),                                                                                                 // [CH = 10 / 860.125 MHz]
        (uint8_t)((_LORA_CONFIG(rssi_packet) ? 0x80 : 0) | 0x03 | _LORA_E22_CONFIG_LBT(_LORA_CONFIG(listen_before_transmit))), // REG3: [RSSI packet], transparent, [LBT], WOR 2000ms
        (uint8_t)((_LORA_CONFIG(crypt) >> 8)),                                                                                 // [CRYPT_H = 0x00]
        (uint8_t)((_LORA_CONFIG(crypt) & 0xFF)),                                                                               // [CRYPT_L = 0x00]
    };

    uint8_t dev_config[_LORA_E22_REG_SIZE_CONFIG_READ];
    ESP_RETURN_ON_ERROR(_lora_cmd_read_config(dev_config), __tag_device_e22900t22, "setup: config read");
    char str[sizeof(dev_config) * 3]; // 'XX:...XX\0'
    ESP_LOGD(__tag_device_e22900t22, "CONFIG: %s", d_bytes_hex_str(str, sizeof(str), dev_config, sizeof(dev_config), ":"));

    const bool e22_config_differs = memcmp(dev_config, req_config, _LORA_E22_REG_SIZE_CONFIG_WRITE) != 0;
    if (e22_config_differs) {
        ESP_RETURN_ON_ERROR(_lora_cmd_write_config(req_config), __tag_device_e22900t22, "setup: config write");
        hw_delay_ms_yieldable(_LORA_SAVE_DELAY_MS);
        uint8_t now_config[_LORA_E22_REG_SIZE_CONFIG_READ];
        if (_lora_cmd_read_config(now_config) != ESP_OK)
            ESP_LOGW(__tag_device_e22900t22, "config verify: read back failed -- cannot confirm the write took");
        else if (memcmp(now_config, req_config, _LORA_E22_REG_SIZE_CONFIG_WRITE) != 0) {
            char want[sizeof(req_config) * 3], got[sizeof(now_config) * 3];
            ESP_LOGE(__tag_device_e22900t22, "config verify: read back failed -- wanted %s, module reports %s", d_bytes_hex_str(want, sizeof(want), req_config, (int)sizeof(req_config), ":"),
                     d_bytes_hex_str(got, sizeof(got), now_config, _LORA_E22_REG_SIZE_CONFIG_WRITE, ":"));
        }
    }

    if (_LORA_IS_USB()) {
        uint8_t reg_config[_LORA_E22_REG_SIZE_CONFIG_READ];
        if (_lora_cmd_read_config(reg_config) == ESP_OK && (reg_config[_LORA_E22_REG1_OFFSET] & 0x04) == 0)
            ESP_LOGE(__tag_device_e22900t22, "REG1 bit 2 (switch config by serial) is CLEAR (REG1=0x%02" PRIX8 ") -- mode commands will be PUT ON AIR instead of obeyed; use the module's mode button to reach config mode and rewrite it",
                     reg_config[_LORA_E22_REG1_OFFSET]);
    }

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_sleep(void) {

    ESP_RETURN_ON_ERROR(_lora_mode_set(LORA_MODE_DEEP_SLEEP), __tag_device_e22900t22, "sleep: mode deep sleep");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wjump-misses-init"
esp_err_t lora_setup(const lora_config_t *const config) {

    _LORA_RTC_INIT();
    _lora_rtc.config = *config;

    if (!_LORA_IS_USB()) {
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_UART_TX));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_UART_RX));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_LORA_AUX));
        ESP_ERROR_CHECK_BOOLEAN(PIN_DEVICE_LORA_M0 == GPIO_NUM_NC || GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_LORA_M0));
        ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_LORA_M1));
    }

    ESP_LOGI(__tag_device_e22900t22, "setup: address=0x%04" PRIX16 ", network=0x%02" PRIX8 ", channel=%d, transmit_power=%d, air_data_rate=%d, packet_size=%d, LBT=%s, crypt=0x%04" PRIX16, _LORA_CONFIG(e22_address),
             _LORA_CONFIG(e22_network), _LORA_CONFIG(channel), _LORA_CONFIG(transmit_power), _LORA_CONFIG(air_data_rate), _LORA_CONFIG(packet_size), _LORA_CONFIG(listen_before_transmit) ? "true" : "false", _LORA_CONFIG(crypt));

    esp_err_t ret;

    _lora_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(E22_UART_PORT, PIN_DEVICE_UART_TX, PIN_DEVICE_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_MIN, UART_TX_BUF_SIZE_DEFAULT), __tag_device_e22900t22, "setup: uart start");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_CONFIG), lora_setup_exit, __tag_device_e22900t22, "setup: config mode");

    (void)_lora_read_product();
    ESP_GOTO_ON_FALSE(_LORA_PRODUCT_ID_VALID(_lora_rtc.product), DEV_ERR_PRODUCT_ID, lora_setup_exit, __tag_device_e22900t22, "setup: product invalid");

    ESP_GOTO_ON_ERROR(_lora_read_and_update_config(), lora_setup_exit, __tag_device_e22900t22, "setup: config");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_DEEP_SLEEP), lora_setup_exit, __tag_device_e22900t22, "setup: mode deep sleep");
    hw_uart_stop(E22_UART_PORT);
    _lora_pins_disable();

    ESP_LOGI(__tag_device_e22900t22, "setup: name=0x%04" PRIX16 ", version=%d, power=%ddBm", (uint16_t)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3]);

    return ESP_OK;

lora_setup_exit:
    (void)lora_sleep();
    hw_uart_stop(E22_UART_PORT);
    _lora_pins_disable();
    return ret;
}
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_start(void) {

    ESP_RETURN_ON_FALSE(_LORA_RTC_VALID(), DEV_ERR_RTC, __tag_device_e22900t22, "start: rtc invalid");
    ESP_RETURN_ON_FALSE(_LORA_PRODUCT_ID_VALID(lora_rtc.product), DEV_ERR_PRODUCT_ID, __tag_device_e22900t22, "start: product invalid");

    esp_err_t ret;

    _lora_pins_enable();

    ESP_RETURN_ON_ERROR(hw_uart_start(E22_UART_PORT, PIN_DEVICE_UART_TX, PIN_DEVICE_UART_RX, UART_BAUD_DEFAULT, UART_RX_BUF_SIZE_MIN, UART_TX_BUF_SIZE_DEFAULT), __tag_device_e22900t22, "start: uart start");

    ESP_GOTO_ON_ERROR(_lora_mode_set(LORA_MODE_NORMAL), lora_start_failed, __tag_device_e22900t22, "start: mode normal");

    ESP_LOGI(__tag_device_e22900t22, "started");

    return ESP_OK;

lora_start_failed:
    (void)_lora_mode_set(LORA_MODE_NORMAL);
    hw_uart_stop(E22_UART_PORT);
    _lora_pins_disable();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_read_channel_rssi(int *const rssi_dbm) {

    const uint8_t cmd[] = { 0xC0, 0xC1, 0xC2, 0xC3, 0x00, 0x01 };
    uint8_t res[4];
    const int n = _lora_cmd_xfer(cmd, sizeof(cmd), res, sizeof(res));
    ESP_RETURN_ON_FALSE(n >= (int)sizeof(res), ESP_FAIL, __tag_device_e22900t22, "channel rssi: got %d bytes, expected %d", n, (int)sizeof(res));
    ESP_RETURN_ON_FALSE(res[0] == 0xC1 && res[1] == 0x00 && res[2] == 0x01, ESP_FAIL, __tag_device_e22900t22, "channel rssi: bad header %02" PRIX8 " %02" PRIX8 " %02" PRIX8, res[0], res[1], res[2]);

    *rssi_dbm = _e22_rssi_dbm(res[3]);
    ESP_LOGI(__tag_device_e22900t22, "channel rssi: %d dBm", *rssi_dbm);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

/*
 * The inter-byte idle that delimits a received frame -- and it has to be told about the link, not
 * about the baud rate.
 *
 * On a DIP module the UART is wired straight to the SoC: bytes arrive one byte-time apart (1.04ms
 * at 9600) and any real gap is a frame boundary, so 20ms is already generous.
 *
 * It was briefly raised to 50ms on the theory that a DIP module was splitting frames mid-packet,
 * after a relay invented stations out of beacons that arrived a byte short. That theory was
 * WRONG and the history is worth keeping, because the symptom looks identical either way: the
 * byte was never lost on this side at all. It was dropped by the USB module at the OTHER end,
 * which had been handed a frame while still radiating the previous one -- see
 * _LORA_TX_GUARD_USB_MS. The proof was _LORA_RX_FRAME_MIN below: a read-side split reports a runt
 * and logs its cause, and across two hours of the 50ms build not one fired while the phantoms kept
 * coming. So 20ms stood, on the stated condition that if a DIP module ever did split a frame the
 * fragment warning would say so and 50ms was the answer.
 *
 * 2026-09-13: IT SAID SO, and 20ms is now 50ms. Two relays, repeatedly, three bytes read and the
 * remaining six arriving later -- and the evidence the earlier investigation could not have had is
 * `buffered=0` in the forensic line beside it: the UART held NOTHING more at the instant the read
 * gave up. The rest of that packet had not been delivered yet, so it was not a byte missed on this
 * side and not a byte dropped at the far end. The DIP module does pace its delivery across more
 * than 20ms, at least sometimes.
 *
 * 50ms is measured rather than guessed: across 1188 receives on that link the shortest gap between
 * two frames was 180ms, with nothing below 100ms, so there is room to raise this without merging
 * two frames into one. Keep DIP below USB rather than collapsing them, though: that 180ms floor is
 * a property of one network's beacon and ack cadence at 2.4kbps, not a guarantee.
 *
 * Note also that FreeRTOS tick granularity works against the smaller number -- pdMS_TO_TICKS(20) is
 * two ticks at 100Hz, so the effective wait could be nearer 10ms than 20ms. At 50ms that error is
 * proportionally much smaller.
 *
 * Behind a USB-serial dongle the host schedules URBs, and a frame longer than one bulk transfer
 * arrives as two, tens of milliseconds apart, with nothing wrong. At 20ms that reads as a frame
 * boundary and a long frame is torn in half: observed on a 56-byte node VERSION report, split into
 * a 31-byte piece that decoded as far as its header and a 28-byte tail that looked like a fresh
 * packet from a nonsense station. The reference implementation allowed 100ms per byte after the
 * first, which is why it never showed this; that is the number to match.
 *
 * The cost of a longer gap is twofold. Two frames genuinely arriving back-to-back within it are
 * read as one; that merged frame then fails to decode -- and since a frame that does not decode no
 * longer claims a dedup slot, it is dropped and the relayed copy still gets through. And every
 * read waits the gap out after its last byte, so a received frame costs the gap in loop time --
 * which is the other reason not to raise this without evidence: it is paid on every frame, by a
 * loop that has to keep reading.
 */
#define _LORA_RX_GAP_DIP_MS 50
#define _LORA_RX_GAP_USB_MS 100

/* The shortest thing that could be a frame: an iotd/mesh header is 4 bytes (variant+station,
   sequence). A read shorter than that PLUS the appended RSSI byte did not catch a whole frame, it
   caught a fragment -- and a fragment's last byte is DATA, not RSSI. Treating it as RSSI is how a
   1-byte fragment becomes `len = 0`, which the caller cannot tell from "nothing arrived", while
   the rest of the packet is then read as a frame in its own right, one byte short at the front.
   Observed live: a gateway beacon `F0 01 0B DD 00 01 00 11 A2` split after its first byte was
   read back as `01 0B DD 00 01 00 11 A2` -- a syntactically perfect header for a station that
   does not exist, which nothing downstream can reject. */
#define _LORA_RX_FRAME_MIN  4
#define _LORA_RX_GAP_MS     (_LORA_IS_USB() ? _LORA_RX_GAP_USB_MS : _LORA_RX_GAP_DIP_MS)

/* Set when a fragment was read, because the REMAINDER of that packet is still coming and will be
   read next as a frame in its own right -- headerless, and so wearing whatever its payload happens
   to spell. That is not a hypothetical: it is where station 0115 came from on 2026-09-13, out of
   the gateway ACK `F0 01 15 F7 ...` split after one byte, forwarded into the mesh and ACKed by the
   gateway before anyone could tell it was not a station.
   Nothing downstream CAN reject it -- a mid-packet read is a syntactically perfect header -- but
   here we know, so here it is dropped. The frame was already lost when the split happened; what
   this prevents is the loss turning into a fictional neighbour. */
static bool _lora_rx_desynced = false;

/*
 * RX FORENSICS. Off in one line (set to 0) -- this is instrumentation for an open question, not
 * something the driver needs to work.
 *
 * It prints only on an ANOMALY, never on a healthy frame. EVERY line it produces -- including the
 * fragment and dropped-tail warnings below, which are not themselves conditional on this flag --
 * is tagged `rx-forensic`, so one grep over a long capture finds the whole story and nothing else:
 *
 *     grep rx-forensic relay0-*.log
 *
 * It prints the four things that tell the candidate explanations apart:
 *
 *   bytes=     cumulative bytes read since boot. A fault that recurs at multiples of a buffer
 *              size is a ring-index fault; nothing else produces that.
 *   since_tx=  ms since our own last transmission. The last phantom hunt ended at a TX/RX
 *              interaction, so this is the correlate with previous form.
 *   since_rx=  ms since the previous read returned, which says whether we were even listening.
 *   buffered=  bytes STILL held by the UART driver the instant the read stopped. Non-zero means
 *              more had already arrived and we gave up early -- the inter-byte gap is too short.
 *              Zero means the wire had genuinely gone quiet and the remainder came later, which
 *              is the module dwelling, not us being impatient. That one field is the question.
 */
#define LORA_RX_FORENSICS 1

#if LORA_RX_FORENSICS
static uint32_t _lora_rx_reads = 0, _lora_rx_bytes = 0, _lora_rx_last_ms = 0, _lora_tx_last_ms = 0;

/* `prev_rx_ms` is passed rather than read from the global because the global is advanced as soon
   as this read ends, and every caller below that point would otherwise measure the gap against
   itself. */
static void _lora_rx_forensic(const char *const why, const uint32_t started_ms, const uint32_t prev_rx_ms) {
    const uint32_t now = hw_time_ms();
    ESP_LOGW(__tag_device_e22900t22, "rx-forensic: %s | read=#%" PRIu32 " bytes=%" PRIu32 " since_rx=%" PRIu32 "ms since_tx=%" PRIu32 "ms took=%" PRIu32 "ms buffered=%u", why, _lora_rx_reads, _lora_rx_bytes,
             prev_rx_ms ? started_ms - prev_rx_ms : 0, _lora_tx_last_ms ? now - _lora_tx_last_ms : 0, now - started_ms, (unsigned)hw_uart_available(E22_UART_PORT));
}
#endif

static const char *_lora_rx_hex(char *const out, const size_t size, const uint8_t *const buf, const size_t len) {
    size_t at = 0;
    for (size_t i = 0; i < len && i < 8 && at + 3 < size; i++)
        at += (size_t)snprintf(out + at, size - at, "%s%02" PRIX8, at ? " " : "", buf[i]);
    out[at] = '\0';
    return out;
}

/* d_bytes_hex_log() takes an emit now rather than logging itself, so the tag and the level are
   ours to supply -- this is the one line that used to be inside it. */
static void _lora_hex_emit(const char *const line) {
    ESP_LOGD(__tag_device_e22900t22, "%s", line);
}

esp_err_t lora_read(uint8_t *const buf, const size_t max, int *const out_len, int *const out_rssi_dbm, const int first_byte_timeout_ms) {

    ESP_RETURN_ON_FALSE(buf != NULL && out_len != NULL && max > 0, ESP_ERR_INVALID_ARG, __tag_device_e22900t22, "read: bad args");

    *out_len = 0;
    if (out_rssi_dbm != NULL)
        *out_rssi_dbm = LORA_RSSI_NONE;

#if LORA_RX_FORENSICS
    const uint32_t forensic_started_ms = hw_time_ms(); /* BEFORE the first byte: `took` is the whole read */
#endif
    if (hw_uart_read(E22_UART_PORT, buf, 1, first_byte_timeout_ms) <= 0)
        return ESP_OK;
    size_t total = 1;
    while (total < max && hw_uart_read(E22_UART_PORT, buf + total, 1, _LORA_RX_GAP_MS) > 0)
        total++;
#if LORA_RX_FORENSICS
    _lora_rx_reads++;
    _lora_rx_bytes += (uint32_t)total;
    const uint32_t forensic_prev_rx_ms = _lora_rx_last_ms; /* the gap BEFORE this read */
    _lora_rx_last_ms = hw_time_ms();
    if (total >= max)
        _lora_rx_forensic("READ FILLED THE BUFFER -- the rest of this frame is still in the uart", forensic_started_ms, forensic_prev_rx_ms);
#endif
    if (total >= (size_t)_LORA_RX_FRAME_MIN + 1) {
        if (_lora_rx_desynced) {
            _lora_rx_desynced = false;
#if LORA_RX_FORENSICS
            _lora_rx_forensic("DROPPING THE TAIL", forensic_started_ms, forensic_prev_rx_ms);
#endif
            char hex[3 * 8 + 1];
            ESP_LOGW(__tag_device_e22900t22, "rx-forensic: DROPPED TAIL, %u bytes [%s] -- the remainder of the packet that split, not a frame", (unsigned)total, _lora_rx_hex(hex, sizeof(hex), buf, total));
            return ESP_OK; /* *out_len stays 0: nothing arrived that we can honestly hand upwards */
        }
        if (out_rssi_dbm != NULL)
            *out_rssi_dbm = _e22_rssi_dbm(buf[total - 1]);
#if LORA_RX_FORENSICS
        /* 0x00 and 0xFF are the two values the module never legitimately reports: -256 dBm is
           below any receiver, -1 dBm above any signal we could survive. Either means the byte we
           took for RSSI is not one -- and unlike a fragment, this is happening often enough to
           correlate against something. */
        if (_e22_rssi_dbm(buf[total - 1]) >= 0) /* every real reading is negative; 0 is the saturation flag */
            _lora_rx_forensic("RSSI WRAPPED (saturated: the sender is very close), reported as 0dBm", forensic_started_ms, forensic_prev_rx_ms);
#endif
        total -= 1;
    } else {
        _lora_rx_desynced = true;
#if LORA_RX_FORENSICS
        _lora_rx_forensic("FRAGMENT", forensic_started_ms, forensic_prev_rx_ms);
#endif
        /* A fragment. Hand back every byte read, with no RSSI claimed, so the caller counts it as
           the runt it is rather than being told nothing happened. The stream is already
           desynchronised at this point and these bytes cannot be un-read -- what this buys is
           that the NEXT frame's bogus header has a logged cause sitting immediately before it. */
        char hex[3 * 8 + 1];
        ESP_LOGW(__tag_device_e22900t22, "rx-forensic: FRAGMENT, %u bytes [%s] -- not a frame: a packet split across the %dms gap, so the next read would have started mid-packet", (unsigned)total, _lora_rx_hex(hex, sizeof(hex), buf, total),
                 _LORA_RX_GAP_MS);
    }
    *out_len = (int)total;

    if (*out_len > 0) {
        ESP_LOGD(__tag_device_e22900t22, "read: %d bytes, rssi=%d dBm", *out_len, out_rssi_dbm != NULL ? *out_rssi_dbm : 0);
        d_bytes_hex_log(_lora_hex_emit, buf, *out_len);
    }

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_write(const uint8_t *const data, const size_t len) {

    ESP_RETURN_ON_FALSE(len > 0 && len <= LORA_PACKET_SIZE_MAX, ESP_ERR_INVALID_SIZE, __tag_device_e22900t22, "write: invalid length %d (< 0 || > %d)", (int)len, (int)LORA_PACKET_SIZE_MAX);

    // Wait for AUX ready before transmit
    ESP_RETURN_ON_FALSE(lora_wait_ready(_lora_transmit_wait_ms()), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "write: wait ready (%" PRIu32 "ms at %ubps -- module still busy)", _lora_transmit_wait_ms(), (unsigned)_LORA_CONFIG(air_data_rate));
    ESP_RETURN_ON_FALSE(hw_uart_write(E22_UART_PORT, data, len) == (int)len, ESP_FAIL, __tag_device_e22900t22, "write: uart write (len=%d)", (int)len);
    // Block until the UART has physically clocked every byte out to the module
    ESP_RETURN_ON_ERROR(hw_uart_wait_tx_done(E22_UART_PORT, _LORA_CMD_TIMEOUT_MS), __tag_device_e22900t22, "write: tx drain");
    if (_LORA_IS_USB())
        _lora_tx_guard_until_ms = hw_time_ms() + _LORA_TX_GUARD_USB_MS;
#if LORA_RX_FORENSICS
    _lora_tx_last_ms = hw_time_ms();
#endif

    ESP_LOGD(__tag_device_e22900t22, "write: sent %d bytes", (int)len);
    d_bytes_hex_log(_lora_hex_emit, data, (int)len);

    return ESP_OK;
}

esp_err_t lora_write_complete(const uint8_t *const data, const size_t len, const bool wait_complete) {
    ESP_RETURN_ON_ERROR(lora_write(data, len), __tag_device_e22900t22, "write_complete: write");
    if (wait_complete)
        ESP_RETURN_ON_FALSE(lora_wait_ready(_LORA_TRANSMIT_DELAY_MS), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "write_complete: wait ready (%d)", _LORA_TRANSMIT_DELAY_MS);
    return ESP_OK;
}

esp_err_t lora_wait_complete(void) {
    ESP_RETURN_ON_FALSE(lora_wait_ready(_LORA_TRANSMIT_DELAY_MS), DEV_ERR_TIMEOUT, __tag_device_e22900t22, "wait_complete: wait ready (%d)", _LORA_TRANSMIT_DELAY_MS);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_stop(void) {

    if (lora_sleep() == ESP_OK)
        if (_LORA_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_LORA_SLEEP_DELAY_MS);
    hw_uart_stop(E22_UART_PORT);
    _lora_pins_disable();

    ESP_LOGI(__tag_device_e22900t22, "stopped");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t lora_test(device_test_result_t *const result, const uint32_t duration_ms) {

    result->passed = false;
    const uint32_t sleeping_ms = 1 * 1000;
    const uint32_t start_ms = hw_time_ms();
    const uint32_t s = esp_random();

    esp_err_t rc;

    if ((rc = lora_setup(&lora_config_default)) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "setup failed: %s", esp_err_to_name(rc));
        return rc;
    }

    if ((rc = lora_start()) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "start failed: %s", esp_err_to_name(rc));
        return rc;
    }

#define _TEST_IOTDATA_VARIANT    0x04 // 4 bits
#define _TEST_IOTDATA_STATION_ID 3210 // 12 bits
#define _TEST_IOTDATA_PRESENCE   0x00 // 8 bits

    int failures = 5, iterations = 0;
    uint16_t sequence = (uint16_t)(s & 0xFFFF);
    while (rc == ESP_OK && (hw_time_ms() - start_ms) < duration_ms) {
        if (iterations++ & 1) {
            // rssi
            int rssi_dbm;
            if ((rc = lora_read_channel_rssi(&rssi_dbm)) != ESP_OK && --failures > 0)
                rc = ESP_OK;
        } else {
            // data
            if (rc == ESP_OK && (hw_time_ms() - start_ms) < duration_ms) {
                const uint8_t packet[5] = {
                    (uint8_t)(((_TEST_IOTDATA_VARIANT << 4) & 0xF0) | ((_TEST_IOTDATA_STATION_ID >> 8) & 0x0F)),
                    (uint8_t)((_TEST_IOTDATA_STATION_ID >> 0) & 0xFF),
                    (uint8_t)((sequence >> 8) & 0xFF),
                    (uint8_t)((sequence >> 0) & 0xFF),
                    _TEST_IOTDATA_PRESENCE,
                };
                if ((rc = lora_write(packet, sizeof(packet))) != ESP_OK && --failures > 0)
                    rc = ESP_OK;
                else
                    ESP_LOGD(__tag_device_e22900t22, "transmit (%d bytes) [%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 ":%02" PRIX8 "]", (int)sizeof(packet), packet[0], packet[1], packet[2], packet[3], packet[4]);
                sequence++;
            }
        }
        if (rc == ESP_OK && (hw_time_ms() - start_ms) < duration_ms)
            hw_delay_ms_yieldable((uint32_t)sleeping_ms);
    }

    esp_err_t rc2;
    if ((rc2 = lora_stop()) != ESP_OK && rc == ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "stop failed: %s", esp_err_to_name(rc));
        return rc2;
    }

    if (rc != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "rssi read failed: %s", esp_err_to_name(rc));
        return rc;
    }

    result->passed = true;
    snprintf(result->detail, sizeof(result->detail), "E22 name=0x%04" PRIX16 ", ver=%d, pwr=%ddBm", (uint16_t)((_lora_rtc.product[0] << 8) | _lora_rtc.product[1]), _lora_rtc.product[2], _lora_rtc.product[3]);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
