
// ------------------------------------------------------------------------------------------------------------------------
// I2C
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

#define I2C_PORT_NUM                  I2C_NUM_0
#define I2C_FREQ_DEFAULT              400000
#define I2C_FREQ_SLOW                 100000
#define I2C_TIMEOUT_MS                100
#define I2C_MAX_TRANSFER_SIZE         64
#define I2C_MAX_TRANSFER_SIZE_DEFAULT 32

// ------------------------------------------------------------------------------------------------------------------------

const i2c_port_t s_i2c_port = I2C_PORT_NUM;
gpio_num_t s_i2c_sda, s_i2c_scl;
int s_i2c_max_transfer_size;

i2c_master_bus_handle_t s_i2c_bus = NULL;
i2c_master_dev_handle_t s_i2c_dev = NULL;

// ------------------------------------------------------------------------------------------------------------------------

static inline void _hw_i2c_pins_enable(void) {
    // sda/scl enabled by i2c_new_master_bus()
}

static inline void _hw_i2c_pins_disable(void) {
    // sda/scl released by i2c_del_master_bus() anyway
    hw_gpio_cfg_disable_two(s_i2c_sda, s_i2c_scl);
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_i2c_start(const gpio_num_t sda, const gpio_num_t scl, const uint32_t freq_hz, const uint8_t dev_addr, const int max_transfer_size) {
    assert(!s_i2c_bus && !s_i2c_dev);

    s_i2c_sda = sda;
    s_i2c_scl = scl;
    s_i2c_max_transfer_size = max_transfer_size > 0 ? max_transfer_size : I2C_MAX_TRANSFER_SIZE_DEFAULT;

    esp_err_t ret = ESP_OK;

    _hw_i2c_pins_enable();

    ESP_ERROR_CHECK(i2c_new_master_bus(
        &(const i2c_master_bus_config_t){
            .i2c_port = s_i2c_port,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = false, // not needed internal pullups when we have external
        },
        &s_i2c_bus));

    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus,
                                                &(const i2c_device_config_t){
                                                    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                                    .device_address = dev_addr,
                                                    .scl_speed_hz = freq_hz,
                                                },
                                                &s_i2c_dev),
                      hw_i2c_start_failed, __func__, "i2c_master_bus_add_device (0x%02" PRIX8 ")", dev_addr);

    ESP_LOGD("hw_i2c", "started: sda=%d, scl=%d, freq=%" PRIu32 " addr=0x%02" PRIX8, sda, scl, freq_hz, dev_addr);

    return ESP_OK;

hw_i2c_start_failed:
    ESP_ERROR_CHECK(i2c_del_master_bus(s_i2c_bus));
    s_i2c_bus = NULL;
    _hw_i2c_pins_disable();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_i2c_stop(void) {
    assert(s_i2c_dev && s_i2c_bus);

    ESP_ERROR_CHECK(i2c_master_bus_rm_device(s_i2c_dev));
    s_i2c_dev = NULL;
    ESP_ERROR_CHECK(i2c_del_master_bus(s_i2c_bus));
    s_i2c_bus = NULL;
    _hw_i2c_pins_disable();

    ESP_LOGD("hw_i2c", "stopped: pins released");
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_write_reg(const uint8_t reg, const uint8_t *const data, const size_t len) {
    ESP_RETURN_ON_FALSE(len <= I2C_MAX_TRANSFER_SIZE, ESP_ERR_INVALID_SIZE, __func__, "invalid length %d (> %d)", (int)len, I2C_MAX_TRANSFER_SIZE);
    uint8_t buf[1 + I2C_MAX_TRANSFER_SIZE];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_i2c_dev, buf, 1 + len, I2C_TIMEOUT_MS);
}
// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_read_reg(const uint8_t reg, uint8_t *const data, const size_t len) {
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_write_byte(const uint8_t reg, const uint8_t val) {
    return i2c_master_transmit(s_i2c_dev, (uint8_t[2]){ reg, val }, 2, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_read_byte(const uint8_t reg, uint8_t *const val) {
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

#define I2C_SCAN_TIMEOUT_MS 20
#define I2C_SCAN_ADDR_FIRST 0x08 /* below is reserved: general call, CBUS, 10-bit prefixes */
#define I2C_SCAN_ADDR_LAST  0x77

// ------------------------------------------------------------------------------------------------------------------------

int hw_i2c_bus_scan(uint8_t *const out, const int out_max) {
    ESP_LOGW("hw_i2c", "scan: sda=%d (%d) scl=%d (%d) (both should be 1)", s_i2c_sda, hw_gpio_get(s_i2c_sda) ? 1 : 0, s_i2c_scl, hw_gpio_get(s_i2c_scl) ? 1 : 0);
    int found = 0;
    char list[8 * 6 + 4] = "";
    size_t at = 0;
    for (uint8_t addr = I2C_SCAN_ADDR_FIRST; addr <= I2C_SCAN_ADDR_LAST; addr++) {
        if ((addr & 0x0F) == 0)
            (void)esp_task_wdt_reset();
        if (i2c_master_probe(s_i2c_bus, addr, I2C_SCAN_TIMEOUT_MS) == ESP_OK) {
            if (out != NULL && found < out_max)
                out[found] = addr;
            if (at + 6 < sizeof(list))
                at += (size_t)snprintf(list + at, sizeof(list) - at, "%s0x%02X", at ? " " : "", addr);
            found++;
        }
    }
    ESP_LOGW("hw_i2c", "scan: %d device(s)%s", found, found ? ": " : "", list);
    return found;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t hw_i2c_bus_start(const gpio_num_t sda, const gpio_num_t scl, __attribute__((unused)) const uint32_t freq_hz) {
    assert(!s_i2c_bus && !s_i2c_dev);

    s_i2c_sda = sda;
    s_i2c_scl = scl;
    s_i2c_max_transfer_size = I2C_MAX_TRANSFER_SIZE_DEFAULT;

    _hw_i2c_pins_enable();

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(
                            &(const i2c_master_bus_config_t){
                                .i2c_port = s_i2c_port,
                                .sda_io_num = sda,
                                .scl_io_num = scl,
                                .clk_source = I2C_CLK_SRC_DEFAULT,
                                .glitch_ignore_cnt = 7,
                                .flags.enable_internal_pullup = false,
                            },
                            &s_i2c_bus),
                        __func__, "i2c_new_master_bus");
    ESP_LOGD("hw_i2c", "bus started: sda=%d, scl=%d", sda, scl);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_i2c_bus_stop(void) {
    if (!s_i2c_bus)
        return;
    assert(!s_i2c_dev);

    ESP_ERROR_CHECK(i2c_del_master_bus(s_i2c_bus));
    s_i2c_bus = NULL;
    _hw_i2c_pins_disable();
    ESP_LOGD("hw_i2c", "bus stopped: pins released");
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

static inline bool hw_i2c_bus_active(void) {
    return s_i2c_bus != NULL;
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_probe(const uint8_t addr) {
    return i2c_master_probe(s_i2c_bus, addr, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_add(const uint8_t addr, i2c_master_dev_handle_t *const out) {
    return i2c_master_bus_add_device(s_i2c_bus,
                                     &(const i2c_device_config_t){
                                         .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                         .device_address = addr,
                                         .scl_speed_hz = I2C_FREQ_SLOW,
                                     },
                                     out);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_del(const i2c_master_dev_handle_t dev) {
    return i2c_master_bus_rm_device(dev);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_reg_write_byte(const i2c_master_dev_handle_t dev, const uint8_t reg, const uint8_t val) {
    return i2c_master_transmit(dev, (uint8_t[2]){ reg, val }, 2, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_reg_read(const i2c_master_dev_handle_t dev, const uint8_t reg, uint8_t *const data, const size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_write(const i2c_master_dev_handle_t dev, const uint8_t *const data, const size_t len) {
    return i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------

static inline esp_err_t hw_i2c_dev_write_read(const i2c_master_dev_handle_t dev, const uint8_t *const wr, const size_t wr_len, uint8_t *const data, const size_t len) {
    return i2c_master_transmit_receive(dev, wr, wr_len, data, len, I2C_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------------------------------
