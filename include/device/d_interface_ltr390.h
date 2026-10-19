
// ------------------------------------------------------------------------------------------------------------------------
// LTR390 - Ambient Light (ALS) and UV Index (UVS)
// https://optoelectronics.liteon.com/upload/download/DS86-2015-0004/LTR-390UV_Final_%20DS_V1%201.pdf
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_ltr390 = "device-ltr390";

// ------------------------------------------------------------------------------------------------------------------------

// Resolution 16-bit (conversion time 25ms), Measurement Rate 25ms: a sample as soon as it converts
#define LTR390_MEAS_RATE_VAL_CONFIG 0b01000000
// GAIN: 3x (good balance for outdoor)
#define LTR390_GAIN_VAL_CONFIG      0x01 // 3x

#define LTR390_I2C_ADDR_DEFAULT     0x53
#ifdef USE_LTR390_I2C_ADDR
#define LTR390_I2C_ADDR USE_LTR390_I2C_ADDR
#endif
#ifndef LTR390_I2C_ADDR
#define LTR390_I2C_ADDR LTR390_I2C_ADDR_DEFAULT
#endif

#define LTR390_LUX_MIN (0.0f)
#define LTR390_LUX_MAX (200000.0f)
#define LTR390_UVI_MIN (0.0f)
#define LTR390_UVI_MAX (25.0f)

#ifndef LTR390_DARK_LUX
#define LTR390_DARK_LUX (10.0f)
#endif

const reading_strategy_t STRAT_LTR390_LUX_DEFAULT = {
    .num_total = 5,
    .num_discard = 2,
    .min_accepted = 1,
    .range_min = LTR390_LUX_MIN,
    .range_max = LTR390_LUX_MAX,
    .outlier_pct = 15.0f,
    .outlier_abs = 5.0f, // lux — floor for dawn/dusk near-zero
};
const reading_strategy_t STRAT_LTR390_UVI_DEFAULT = {
    .num_total = 5,
    .num_discard = 2,
    .min_accepted = 1,
    .range_min = LTR390_UVI_MIN,
    .range_max = LTR390_UVI_MAX,
    .outlier_pct = 15.0f,
    .outlier_abs = 0.5f, // UVI — floor for low-UV conditions
};

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    float lux;
    float uvi;
    reading_result_t lux_quality;
    reading_result_t uvi_quality;
} ltr390_reading_t;

typedef struct {
    uint8_t i2c_addr;
    reading_strategy_t strategy_lux, strategy_uvi;
    float dark_lux; /* lux below which UV is not measured, and reads 0 -- see LTR390_DARK_LUX; 0 = always */
} ltr390_config_t;

const ltr390_config_t ltr390_config_default = {
    .i2c_addr = LTR390_I2C_ADDR,
    .strategy_lux = STRAT_LTR390_LUX_DEFAULT,
    .strategy_uvi = STRAT_LTR390_UVI_DEFAULT,
    .dark_lux = LTR390_DARK_LUX,
};

// ------------------------------------------------------------------------------------------------------------------------

#define _LTR390_PART_ID_VAL           0xB2
#define _LTR390_I2C_MAX_PAYLOAD       8

#define _LTR390_MODE_SLEEP            0x00
#define _LTR390_SLEEP_DELAY_MS        5
#define _LTR390_READY_DELAY_MS        10
#define _LTR390_WAIT_DELAY_MS         (_LTR390_READY_DELAY_MS * 30)

// ------------------------------------------------------------------------------------------------------------------------

#define _LTR390_REG_MAIN_CTRL         0x00
#define _LTR390_REG_MEAS_RATE         0x04
#define _LTR390_REG_GAIN              0x05
#define _LTR390_REG_PART_ID           0x06
#define _LTR390_REG_STATUS            0x07
#define _LTR390_REG_ALS_DATA          0x0D // 3 bytes (20-bit)
#define _LTR390_REG_UVS_DATA          0x10 // 3 bytes (20-bit)

// MAIN_CTRL bits
#define _LTR390_CTRL_ENABLE           0x02
#define _LTR390_CTRL_UVS_MODE         0x08 // 0=ALS, 1=UVS

// MEAS_RATE: resolution (bits 6:4) and rate (bits 2:0)
// Resolution 18-bit (conversion time 100ms), Measurement Rate 100ms
#define _LTR390_MEAS_RATE_VAL_DEFAULT 0x22

// GAIN: 3x (good balance for outdoor)
#define _LTR390_GAIN_VAL_DEFAULT      0x01 // 3x

// STATUS bit 3 = data ready
#define _LTR390_STATUS_DRDY           0x08

// // 18-bit, gain=3x, int=100ms (default)
// #define _LTR390_LUX_FACTOR  (0.6f / (3.0f * 1.0f))
// 16-bit, gain=3x, int=50ms (configured)
#define _LTR390_LUX_FACTOR            (0.6f / (3.0f * 0.5f))

// // 18-bit (default)
// #define _LTR390_UVI_SENSITIVITY 2300.0f
// 16-bit (configured)
#define _LTR390_UVI_SENSITIVITY       1150.0f

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    ltr390_config_t config;
    uint8_t part_id;
} _ltr390_rtc_t;

#define _LTR390_RTC_MAGIC 0xDEE1E390
_RTC_DATA_STRUCT _ltr390_rtc_t _ltr390_rtc;
#define _LTR390_RTC_INIT()             _RTC_DATA_INIT(&_ltr390_rtc, _LTR390_RTC_MAGIC)
#define _LTR390_RTC_VALID()            _RTC_DATA_VALID(&_ltr390_rtc, _LTR390_RTC_MAGIC)

#define _LTR390_CONFIG(entry)          ((_ltr390_rtc.config).entry)

// ------------------------------------------------------------------------------------------------------------------------

#define _LTR390_PART_ID_VALID(part_id) ((part_id) == _LTR390_PART_ID_VAL)

// ------------------------------------------------------------------------------------------------------------------------

bool ltr390_is_ready(void) {
    uint8_t status;
    if (hw_i2c_read_byte(_LTR390_REG_STATUS, &status) != ESP_OK)
        return false;
    return (status & _LTR390_STATUS_DRDY) != 0;
}

D_WAIT_READY_FUNC(ltr390_wait_ready, ltr390_is_ready, _LTR390_READY_DELAY_MS, false, __tag_device_ltr390)

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _ltr390_set_mode(const bool uvs_mode) {

    ESP_RETURN_ON_ERROR(hw_i2c_write_byte(_LTR390_REG_MAIN_CTRL, _LTR390_CTRL_ENABLE | (uvs_mode ? _LTR390_CTRL_UVS_MODE : 0)), __tag_device_ltr390, "set_mode: i2c write");

    return ESP_OK;
}

esp_err_t _ltr390_read_raw(const bool uvs_mode, uint32_t *const raw_out) {

    ESP_RETURN_ON_FALSE(ltr390_wait_ready(_LTR390_WAIT_DELAY_MS), DEV_ERR_TIMEOUT, __tag_device_ltr390, "read_raw: wait ready");
    uint8_t buf[3];
    ESP_RETURN_ON_ERROR(hw_i2c_read_reg(uvs_mode ? _LTR390_REG_UVS_DATA : _LTR390_REG_ALS_DATA, buf, sizeof(buf)), __tag_device_ltr390, "read_raw: i2c read (data)");
    *raw_out = ((uint32_t)buf[2] << 16 | (uint32_t)buf[1] << 8 | buf[0]) & 0x0FFFFF;

    return ESP_OK;
}

esp_err_t _ltr390_trigger_and_read_raw(const bool uvs_mode, uint32_t *const raw_out) {

    ESP_RETURN_ON_ERROR(_ltr390_set_mode(uvs_mode), __tag_device_ltr390, "trigger_and_read_raw: i2c write (mode)");
    ESP_RETURN_ON_ERROR(_ltr390_read_raw(uvs_mode, raw_out), __tag_device_ltr390, "trigger_and_read_raw: read_raw");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_sleep(void) {

    ESP_RETURN_ON_ERROR(hw_i2c_write_byte(_LTR390_REG_MAIN_CTRL, _LTR390_MODE_SLEEP), __tag_device_ltr390, "sleep: i2c write");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

/* PART_ID is [7:4] part number, [3:0] revision, so the family is the high nibble and an
   unexpected low nibble is a different silicon revision rather than a different part. */
static const char *_ltr390_part_name(const uint8_t id) {
    return ((id >> 4) == (_LTR390_PART_ID_VAL >> 4)) ? "LTR390" : "unknown";
}

void ltr390_diagnose(void) {
    const uint8_t expect = _LTR390_RTC_VALID() ? _LTR390_CONFIG(i2c_addr) : (uint8_t)LTR390_I2C_ADDR;
    if (hw_i2c_bus_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT) != ESP_OK) {
        ESP_LOGE(__tag_device_ltr390, "diag: cannot open the i2c bus on gpio%d/%d", PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL);
        return;
    }
    uint8_t addr[8];
    const int n = hw_i2c_bus_scan(addr, (int)(sizeof(addr) / sizeof(addr[0])));
    if (n <= 0) {
        ESP_LOGE(__tag_device_ltr390, "diag: module is unpowered, unconnected or faulty (check VCC at 3V3, and SDA/SCL to gpio%d/%d)", PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL);
    } else {
        for (int i = 0; i < n && i < (int)(sizeof(addr) / sizeof(addr[0])); i++) {
            i2c_master_dev_handle_t dev;
            if (hw_i2c_dev_add(addr[i], &dev) == ESP_OK) {
                uint8_t id = 0;
                if (hw_i2c_dev_reg_read(dev, _LTR390_REG_PART_ID, &id, 1) != ESP_OK)
                    ESP_LOGW(__tag_device_ltr390, "diag: 0x%02" PRIX8 " answers, but register 0x%02X would not read", addr[i], _LTR390_REG_PART_ID);
                else if ((id >> 4) == (_LTR390_PART_ID_VAL >> 4) && addr[i] != expect)
                    ESP_LOGW(__tag_device_ltr390, "diag: 0x%02" PRIX8 " part-id=0x%02" PRIX8 " %s <-- THE SENSOR IS HERE, not at 0x%02" PRIX8 ". Set .i2c_addr = 0x%02" PRIX8 " in this board's ltr390_config_t", addr[i], id,
                             _ltr390_part_name(id), expect, addr[i]);
                else
                    ESP_LOGW(__tag_device_ltr390, "diag: 0x%02" PRIX8 " part-id=0x%02" PRIX8 " %s", addr[i], id, _ltr390_part_name(id));
                (void)hw_i2c_dev_del(dev);
            }
        }
    }
    hw_i2c_bus_stop();
}

// ------------------------------------------------------------------------------------------------------------------------

bool ltr390_detect(const ltr390_config_t *const config) {

    const uint8_t addr = (config != NULL && config->i2c_addr != 0u) ? config->i2c_addr : (uint8_t)LTR390_I2C_ADDR;

    if (hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, addr, _LTR390_I2C_MAX_PAYLOAD) != ESP_OK) {
        ESP_LOGD(__tag_device_ltr390, "detect: i2c start failed");
        return false;
    }
    uint8_t id = 0;
    const bool read = (hw_i2c_read_byte(_LTR390_REG_PART_ID, &id) == ESP_OK);
    hw_i2c_stop();

    const bool found = read && ((id >> 4) == (_LTR390_PART_ID_VAL >> 4));
    if (found)
        ESP_LOGI(__tag_device_ltr390, "detect: present at 0x%02" PRIX8 " (part-id=0x%02" PRIX8 ", rev %u)", addr, id, (unsigned)(id & 0x0Fu));
    else if (read)
        ESP_LOGD(__tag_device_ltr390, "detect: 0x%02" PRIX8 " answers but part-id=0x%02" PRIX8 " is %s", addr, id, _ltr390_part_name(id));
    else
        ESP_LOGD(__tag_device_ltr390, "detect: nothing at 0x%02" PRIX8, addr);
    return found;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_setup(const ltr390_config_t *const config) {

    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_I2C_SDA) && GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_I2C_SDA));
    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_I2C_SCL));

    _LTR390_RTC_INIT();
    _ltr390_rtc.config = *config;
    if (_ltr390_rtc.config.i2c_addr == 0u)
        _ltr390_rtc.config.i2c_addr = LTR390_I2C_ADDR;

    char sb1[STRATEGY_STR_MAX], sb2[STRATEGY_STR_MAX];
    ESP_LOGD(__tag_device_ltr390, "setup: strategy_lux=%s, strategy_uvi=%s", reading_strategy_to_str(&_LTR390_CONFIG(strategy_lux), sb1, sizeof(sb1)), reading_strategy_to_str(&_LTR390_CONFIG(strategy_uvi), sb2, sizeof(sb2)));

    esp_err_t ret;

    ESP_RETURN_ON_ERROR(hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, _LTR390_CONFIG(i2c_addr), _LTR390_I2C_MAX_PAYLOAD), __tag_device_ltr390, "start: i2c start");

    ESP_GOTO_ON_ERROR(hw_i2c_read_byte(_LTR390_REG_PART_ID, &_ltr390_rtc.part_id), ltr390_setup_failed, __tag_device_ltr390, "start: i2c read (part-id)");
    ESP_LOGD(__tag_device_ltr390, "product: part-id=0x%02" PRIX8, _ltr390_rtc.part_id);
    ESP_GOTO_ON_FALSE(_LTR390_PART_ID_VALID(_ltr390_rtc.part_id), DEV_ERR_PRODUCT_ID, ltr390_setup_failed, __tag_device_ltr390, "start: part-id invalid: 0x%02" PRIX8 " (expected 0x%02" PRIX8 ")", _ltr390_rtc.part_id, _LTR390_PART_ID_VAL);

    hw_i2c_stop();

    ESP_LOGI(__tag_device_ltr390, "setup: part-id=0x%02" PRIX8, _ltr390_rtc.part_id);

    return ESP_OK;

ltr390_setup_failed:
    (void)ltr390_sleep();
    hw_i2c_stop();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_start(void) {

    ESP_RETURN_ON_FALSE(_LTR390_RTC_VALID(), DEV_ERR_RTC, __tag_device_ltr390, "start: rtc invalid");
    ESP_RETURN_ON_FALSE(_LTR390_PART_ID_VALID(_ltr390_rtc.part_id), DEV_ERR_PRODUCT_ID, __tag_device_ltr390, "start: part-id invalid: 0x%02" PRIX8 " (expected 0x%02" PRIX8 ")", _ltr390_rtc.part_id, _LTR390_PART_ID_VAL);

    esp_err_t ret;

    ESP_RETURN_ON_ERROR(hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, _LTR390_CONFIG(i2c_addr), _LTR390_I2C_MAX_PAYLOAD), __tag_device_ltr390, "start: i2c start");

    if (LTR390_MEAS_RATE_VAL_CONFIG != _LTR390_MEAS_RATE_VAL_DEFAULT)
        ESP_GOTO_ON_ERROR(hw_i2c_write_byte(_LTR390_REG_MEAS_RATE, LTR390_MEAS_RATE_VAL_CONFIG), ltr390_start_failed, __tag_device_ltr390, "start: i2c write (meas rate)");
    if (LTR390_GAIN_VAL_CONFIG != _LTR390_GAIN_VAL_DEFAULT)
        ESP_GOTO_ON_ERROR(hw_i2c_write_byte(_LTR390_REG_GAIN, LTR390_GAIN_VAL_CONFIG), ltr390_start_failed, __tag_device_ltr390, "start: i2c write (gain)");

    ESP_LOGI(__tag_device_ltr390, "started: part-id=0x%02" PRIX8, _ltr390_rtc.part_id);

    return ESP_OK;

ltr390_start_failed:
    (void)ltr390_sleep();
    hw_i2c_stop();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_read(ltr390_reading_t *const out, const int opts, const reading_strategy_t *const strategy) {
    (void)opts;

    const reading_strategy_t *st_lux = strategy ? strategy : &_LTR390_CONFIG(strategy_lux);
    const reading_strategy_t *st_uvi = strategy ? strategy : &_LTR390_CONFIG(strategy_uvi);

    float lux_arr[READINGS_MAX], uvi_arr[READINGS_MAX];
    int lux_count = 0, uvi_count = 0;

    uint32_t raw;

    if (_ltr390_set_mode(false) != ESP_OK)
        ESP_LOGW(__tag_device_ltr390, "ALS mode set failed, skipping lux readings");
    else
        for (int i = 0; i < MIN_INT(st_lux->num_total, READINGS_MAX); i++) {
            if (_ltr390_read_raw(false, &raw) == ESP_OK)
                lux_arr[lux_count++] = (float)raw * _LTR390_LUX_FACTOR;
            else
                ESP_LOGW(__tag_device_ltr390, "reading %d (lux) failed, skipping", i);
        }
    if (lux_count > 0)
        readings_process(lux_arr, lux_count, st_lux, &out->lux_quality);

    const bool dark = _LTR390_CONFIG(dark_lux) > 0.0f && lux_count > 0 && out->lux_quality.valid && out->lux_quality.value < _LTR390_CONFIG(dark_lux);
    if (dark)
        out->uvi_quality = (reading_result_t){ .value = 0.0f, .valid = true };
    else if (_ltr390_set_mode(true) != ESP_OK)
        ESP_LOGW(__tag_device_ltr390, "UVS mode set failed, skipping uvi readings");
    else
        for (int i = 0; i < MIN_INT(st_uvi->num_total, READINGS_MAX); i++) {
            if (_ltr390_read_raw(true, &raw) == ESP_OK)
                uvi_arr[uvi_count++] = (float)raw / _LTR390_UVI_SENSITIVITY;
            else
                ESP_LOGW(__tag_device_ltr390, "reading %d (uvi) failed, skipping", i);
        }

    ESP_RETURN_ON_FALSE(lux_count > 0 || uvi_count > 0, DEV_ERR_BAD_READING, __tag_device_ltr390, "ltr390_read");

    out->lux = (lux_count > 0) ? out->lux_quality.value : nanf("");

    if (dark)
        out->uvi = 0.0f;
    else if (uvi_count <= 0)
        out->uvi = nanf("");
    else {
        readings_process(uvi_arr, uvi_count, st_uvi, &out->uvi_quality);
        out->uvi = out->uvi_quality.value;
    }

    ESP_LOGI(__tag_device_ltr390, "lux=%.1f uvi=%.2f%s", (double)out->lux, (double)out->uvi, dark ? " (dark: uv not measured)" : "");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_stop(void) {

    if (ltr390_sleep() == ESP_OK)
        if (_LTR390_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_LTR390_SLEEP_DELAY_MS);
    hw_i2c_stop();

    ESP_LOGI(__tag_device_ltr390, "stopped");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t ltr390_test(device_test_result_t *const result, const uint32_t duration_ms) {

    result->passed = false;
    const uint32_t sleeping_ms = 1 * 1000;
    const uint32_t start_ms = hw_time_ms();

    esp_err_t rc;
    ltr390_reading_t reading;

    const ltr390_config_t config = _LTR390_RTC_VALID() ? _ltr390_rtc.config : ltr390_config_default;

    if ((rc = ltr390_setup(&config)) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "setup failed: %s", esp_err_to_name(rc));
        return rc;
    }

    if ((rc = ltr390_start()) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "start failed: %s", esp_err_to_name(rc));
        return rc;
    }

    while (rc == ESP_OK && (hw_time_ms() - start_ms) < duration_ms) {
        ESP_LOGD(__tag_device_ltr390, "%s: ltr390_read", __func__);
        if ((rc = ltr390_read(&reading, 0, NULL)) != ESP_OK)
            break;
        if (!reading.lux_quality.valid || !reading.uvi_quality.valid) {
            snprintf(result->detail, sizeof(result->detail), "quality fail lux:%d uvi:%d", reading.lux_quality.valid, reading.uvi_quality.valid);
            rc = DEV_ERR_BAD_READING;
        } else if ((hw_time_ms() - start_ms) < duration_ms)
            hw_delay_ms_yieldable((uint32_t)sleeping_ms);
    }

    esp_err_t rc2;
    if ((rc2 = ltr390_stop()) != ESP_OK && rc == ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "stop failed: %s", esp_err_to_name(rc));
        return rc2;
    }

    if (rc != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "read failed: %s", esp_err_to_name(rc));
        return rc;
    }

    result->passed = true;
    snprintf(result->detail, sizeof(result->detail), "lux=%.1f uvi=%.2f", (double)reading.lux, (double)reading.uvi);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
