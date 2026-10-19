
// ------------------------------------------------------------------------------------------------------------------------
// BME280 - Temperature / Pressure / Humidity (I2C, forced mode)
// https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf
// ------------------------------------------------------------------------------------------------------------------------

const char *__tag_device_bme280 = "device-bme280";

// ------------------------------------------------------------------------------------------------------------------------

#define BME280_DO_RESET_ON_STARTUP false
#define BME280_DO_CONF_CONFIG      0x00 // default

#define BME280_I2C_ADDR_DEFAULT    0x76
#ifdef USE_BME280_I2C_ADDR
#define BME280_I2C_ADDR USE_BME280_I2C_ADDR
#endif
#ifndef BME280_I2C_ADDR
#define BME280_I2C_ADDR BME280_I2C_ADDR_DEFAULT
#endif
#define BME280_CHIP_ID_VAL     0x60
#define BME280_CHIP_ID_BMP280  0x58
#define BME280_I2C_MAX_PAYLOAD 32

#define BME280_TEMP_MIN        (-50.0f)
#define BME280_TEMP_MAX        (80.0f)
#define BME280_PRES_MIN        (300.0f)
#define BME280_PRES_MAX        (1100.0f)
#define BME280_HUMI_MIN        (0.0f)
#define BME280_HUMI_MAX        (100.0f)

const reading_strategy_t STRAT_BME280_TEMP_DEFAULT = {
    .num_total = 5,
    .num_discard = 2, // forced mode settles by reading 2
    .min_accepted = 1,
    .range_min = BME280_TEMP_MIN,
    .range_max = BME280_TEMP_MAX,
    .outlier_pct = 5.0f,
    .outlier_abs = 1.5f, // °C floor for near-zero operation
};
const reading_strategy_t STRAT_BME280_PRES_DEFAULT = {
    .num_total = 5,
    .num_discard = 2,
    .min_accepted = 1,
    .range_min = BME280_PRES_MIN,
    .range_max = BME280_PRES_MAX,
    .outlier_pct = 0.0f, // percentage is meaningless at ~1000 hPa
    .outlier_abs = 5.0f, // hPa — catches gross failures, allows weather variation
};
const reading_strategy_t STRAT_BME280_HUMI_DEFAULT = {
    .num_total = 5,
    .num_discard = 2,
    .min_accepted = 1,
    .range_min = BME280_HUMI_MIN,
    .range_max = BME280_HUMI_MAX,
    .outlier_pct = 10.0f,
    .outlier_abs = 5.0f, // %RH — floor for low-humidity conditions
};

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    float temperature_c;
    float pressure_hpa;
    float humidity_pct;
    reading_result_t temp_quality;
    reading_result_t pres_quality;
    reading_result_t humi_quality;
} bme280_reading_t;

typedef struct {
    uint8_t i2c_addr;
    reading_strategy_t strategy_temp, strategy_pres, strategy_humi;
} bme280_config_t;

const bme280_config_t bme280_config_default = {
    .i2c_addr = BME280_I2C_ADDR,
    .strategy_temp = STRAT_BME280_TEMP_DEFAULT,
    .strategy_pres = STRAT_BME280_PRES_DEFAULT,
    .strategy_humi = STRAT_BME280_HUMI_DEFAULT,
};

// ------------------------------------------------------------------------------------------------------------------------

#define _BME280_SLEEP_DELAY_MS        5
#define _BME280_START_DELAY_MS        10
#define _BME280_READY_DELAY_MS        2
#define _BME280_WAIT_DELAY_MS         (_BME280_READY_DELAY_MS * 30)
#define _BME280_READ_DELAY_MS         2

#define _BME280_READ_CYCLE_COUNT      10

// ------------------------------------------------------------------------------------------------------------------------

#define _BME280_REG_CHIP_ID           0xD0
#define _BME280_REG_RESET             0xE0
#define _BME280_REG_CTRL_HUMI         0xF2
#define _BME280_REG_STATUS            0xF3
#define _BME280_REG_CTRL_MEAS         0xF4
#define _BME280_REG_CONFIG            0xF5
#define _BME280_REG_DATA              0xF7 // 8 bytes: press[2:0] temp[2:0] hum[1:0]
#define _BME280_REG_CALIB_T_P         0x88 // 26 bytes
#define _BME280_REG_CALIB_H1          0xA1 // 1 byte
#define _BME280_REG_CALIB_H2          0xE1 // 7 bytes

#define _BME280_STATUS_BIT_IM_UPDATE  0x01
#define _BME280_STATUS_BIT_MEAS_CLEAR 0x08
#define _BME280_RESET_VAL             0xB6
#define _BME280_MODE_FORCED           0x01
#define _BME280_MODE_SLEEP            0x00
#define _BME280_OSRS_1X               0x01

#define _BME280_CONF_DEFAULT          0x00 // no standby, no filter, no SPI3w (stays in sleep mode)
#define _BME280_CTRL_HUMI_DEFAULT     (_BME280_OSRS_1X)
#define _BME280_CTRL_MEAS_DEFAULT     ((_BME280_OSRS_1X << 5) | (_BME280_OSRS_1X << 2) | _BME280_MODE_FORCED)

// ------------------------------------------------------------------------------------------------------------------------

typedef struct {
    uint16_t dig_T1;
    int16_t dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4, dig_H5;
    int8_t dig_H6;
    int32_t t_fine; // shared state between T and P/H compensation
} _bme280_calib_t;

typedef struct {
    _RTC_DATA_STAMP_ENTRY;
    bme280_config_t config;
    uint8_t chip_id;
    _bme280_calib_t calib;
} _bme280_rtc_t;

#define _BME280_RTC_MAGIC 0xDEEBE280
_RTC_DATA_STRUCT _bme280_rtc_t _bme280_rtc;
#define _BME280_RTC_INIT()             _RTC_DATA_INIT(&_bme280_rtc, _BME280_RTC_MAGIC)
#define _BME280_RTC_VALID()            _RTC_DATA_VALID(&_bme280_rtc, _BME280_RTC_MAGIC)

#define _BME280_CONFIG(entry)          ((_bme280_rtc.config).entry)

// ------------------------------------------------------------------------------------------------------------------------

#define _BME280_CHIP_ID_VALID(chip_id) ((chip_id) == BME280_CHIP_ID_VAL || (chip_id) == BME280_CHIP_ID_BMP280)

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _bme280_read_calibration(_bme280_calib_t *const calib) {

    // Temperature and pressure calibration (0x88..0xA1)
    uint8_t buf1[26];
    ESP_RETURN_ON_ERROR(hw_i2c_read_reg(_BME280_REG_CALIB_T_P, buf1, sizeof(buf1)), __tag_device_bme280, "read calib: i2c read (T/P)");
    calib->dig_T1 = (uint16_t)(buf1[1] << 8 | buf1[0]);
    calib->dig_T2 = (int16_t)(buf1[3] << 8 | buf1[2]);
    calib->dig_T3 = (int16_t)(buf1[5] << 8 | buf1[4]);
    calib->dig_P1 = (uint16_t)(buf1[7] << 8 | buf1[6]);
    calib->dig_P2 = (int16_t)(buf1[9] << 8 | buf1[8]);
    calib->dig_P3 = (int16_t)(buf1[11] << 8 | buf1[10]);
    calib->dig_P4 = (int16_t)(buf1[13] << 8 | buf1[12]);
    calib->dig_P5 = (int16_t)(buf1[15] << 8 | buf1[14]);
    calib->dig_P6 = (int16_t)(buf1[17] << 8 | buf1[16]);
    calib->dig_P7 = (int16_t)(buf1[19] << 8 | buf1[18]);
    calib->dig_P8 = (int16_t)(buf1[21] << 8 | buf1[20]);
    calib->dig_P9 = (int16_t)(buf1[23] << 8 | buf1[22]);

    // Humidity calibration part 1 (0xA1)
    ESP_RETURN_ON_ERROR(hw_i2c_read_byte(_BME280_REG_CALIB_H1, &calib->dig_H1), __tag_device_bme280, "read calib: i2c read (H1)");
    // Humidity calibration part 2 (0xE1..0xE7)
    uint8_t buf2[7];
    ESP_RETURN_ON_ERROR(hw_i2c_read_reg(_BME280_REG_CALIB_H2, buf2, sizeof(buf2)), __tag_device_bme280, "read calib: i2c read (H2-H6)");
    calib->dig_H2 = (int16_t)(buf2[1] << 8 | buf2[0]);
    calib->dig_H3 = buf2[2];
    calib->dig_H4 = (int16_t)((int16_t)buf2[3] << 4 | (buf2[4] & 0x0F));
    calib->dig_H5 = (int16_t)((int16_t)buf2[5] << 4 | (buf2[4] >> 4));
    calib->dig_H6 = (int8_t)buf2[6];

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

float _bme280_compensate_temperature(_bme280_calib_t *const calib, const int32_t adc_T) {
    int64_t var1 = (((int64_t)(adc_T >> 3) - ((int64_t)calib->dig_T1 << 1)) * (int64_t)calib->dig_T2) >> 11;
    int64_t diff = (int64_t)(adc_T >> 4) - (int64_t)calib->dig_T1;
    int64_t var2 = (int64_t)((uint64_t)diff * (uint64_t)diff) >> 12;
    var2 = (var2 * (int64_t)calib->dig_T3) >> 14;
    calib->t_fine = (int32_t)(var1 + var2);
    int32_t T = (calib->t_fine * 5 + 128) >> 8;
    return (float)T / 100.0f;
}

// ------------------------------------------------------------------------------------------------------------------------

float _bme280_compensate_pressure(_bme280_calib_t *const calib, const int32_t adc_P) {
    int64_t var1 = (int64_t)calib->t_fine - 128000;
    int64_t var2 = (int64_t)((uint64_t)var1 * (uint64_t)var1) * (int64_t)calib->dig_P6;
    var2 = var2 + ((var1 * (int64_t)calib->dig_P5) << 17);
    var2 = var2 + (((int64_t)calib->dig_P4) << 35);
    var1 = ((int64_t)((uint64_t)var1 * (uint64_t)var1) * (int64_t)calib->dig_P3 >> 8) + ((var1 * (int64_t)calib->dig_P2) << 12);
    var1 = ((((int64_t)1) << 47) + var1) * ((int64_t)calib->dig_P1) >> 33;
    if (var1 == 0)
        return 0.0f;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)calib->dig_P9 * (int64_t)((uint64_t)(p >> 13) * (uint64_t)(p >> 13))) >> 25;
    var2 = ((int64_t)calib->dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)calib->dig_P7) << 4);
    return (float)((uint32_t)p) / 25600.0f; // Pa → hPa
}

// ------------------------------------------------------------------------------------------------------------------------

float _bme280_compensate_humidity(_bme280_calib_t *const calib, const int32_t adc_H) {
    int32_t v = calib->t_fine - 76800;
    v = (((((adc_H << 14) - (((int32_t)calib->dig_H4) << 20) - (((int32_t)calib->dig_H5) * v)) + 16384) >> 15) *
         (((((((v * ((int32_t)calib->dig_H6)) >> 10) * (((v * ((int32_t)calib->dig_H3)) >> 11) + 32768)) >> 10) + 2097152) * ((int32_t)calib->dig_H2) + 8192) >> 14));
    int32_t sq = (int32_t)((uint32_t)(v >> 15) * (uint32_t)(v >> 15));
    v = v - (((sq >> 7) * ((int32_t)calib->dig_H1)) >> 4);
    v = (v < 0) ? 0 : v;
    v = (v > 419430400) ? 419430400 : v;
    return (float)((uint32_t)v >> 12) / 1024.0f;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t _bme280_trigger_and_read_raw(int32_t *const adc_T, int32_t *const adc_P, int32_t *const adc_H) {

    // Trigger forced measurement: write ctrl_hum first, then ctrl_meas
    ESP_RETURN_ON_ERROR(hw_i2c_write_byte(_BME280_REG_CTRL_HUMI, _BME280_CTRL_HUMI_DEFAULT), __tag_device_bme280, "read_raw: i2c write (ctrl_hum)");
    ESP_RETURN_ON_ERROR(hw_i2c_write_byte(_BME280_REG_CTRL_MEAS, _BME280_CTRL_MEAS_DEFAULT), __tag_device_bme280, "read_raw: i2c write (ctrl_meas)");

    // Wait for measurement complete (typ. 8ms for 1x oversampling all channels)
    for (int i = 0; i < _BME280_READ_CYCLE_COUNT; i++) {
        hw_delay_ms_precise(_BME280_READ_DELAY_MS);
        uint8_t status;
        ESP_RETURN_ON_ERROR(hw_i2c_read_byte(_BME280_REG_STATUS, &status), __tag_device_bme280, "read_raw: i2c read (status)");
        if ((status & _BME280_STATUS_BIT_MEAS_CLEAR) == 0)
            break; // measuring bit cleared
    }

    // Read raw data (8 bytes from 0xF7)
    uint8_t data[8];
    ESP_RETURN_ON_ERROR(hw_i2c_read_reg(_BME280_REG_DATA, data, sizeof(data)), __tag_device_bme280, "read_raw: i2c read (data)");
    *adc_P = (int32_t)((uint32_t)data[0] << 12 | (uint32_t)data[1] << 4 | data[2] >> 4);
    *adc_T = (int32_t)((uint32_t)data[3] << 12 | (uint32_t)data[4] << 4 | data[5] >> 4);
    *adc_H = (int32_t)((uint32_t)data[6] << 8 | data[7]);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

bool bme280_is_ready(void) {
    uint8_t status;
    if (hw_i2c_read_byte(_BME280_REG_STATUS, &status) != ESP_OK)
        return false;
    return (status & _BME280_STATUS_BIT_IM_UPDATE) == 0; // im_update bit clear
}

D_WAIT_READY_FUNC(bme280_wait_ready, bme280_is_ready, _BME280_READY_DELAY_MS, false, __tag_device_bme280)

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_sleep(void) {

    ESP_RETURN_ON_ERROR(hw_i2c_write_byte(_BME280_REG_CTRL_MEAS, _BME280_MODE_SLEEP), __tag_device_bme280, "sleep: i2c write");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

static const char *_bme280_chip_name(const uint8_t id) {
    switch (id) {
    case 0x60:
        return "BME280/BMP390";
    case 0x58:
        return "BMP280";
    case 0x61:
        return "BME680/BME688";
    case 0x55:
        return "BMP180/BMP085";
    case 0x50:
        return "BMP388";
    default:
        return "unknown";
    }
}

void bme280_diagnose(void) {
    const uint8_t expect = _BME280_RTC_VALID() ? _BME280_CONFIG(i2c_addr) : (uint8_t)BME280_I2C_ADDR;
    if (hw_i2c_bus_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT) != ESP_OK) {
        ESP_LOGE(__tag_device_bme280, "diag: cannot open the i2c bus on gpio%d/%d", PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL);
        return;
    }
    uint8_t addr[8];
    const int n = hw_i2c_bus_scan(addr, (int)(sizeof(addr) / sizeof(addr[0])));
    if (n <= 0) {
        ESP_LOGE(__tag_device_bme280, "diag: module is unpowered, unconnected or faulty (check VCC at 3V3, and SDA/SCL to gpio%d/%d)", PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL);
    } else {
        for (int i = 0; i < n && i < (int)(sizeof(addr) / sizeof(addr[0])); i++) {
            i2c_master_dev_handle_t dev;
            if (hw_i2c_dev_add(addr[i], &dev) == ESP_OK) {
                uint8_t id = 0;
                if (hw_i2c_dev_reg_read(dev, _BME280_REG_CHIP_ID, &id, 1) != ESP_OK)
                    ESP_LOGW(__tag_device_bme280, "diag: 0x%02" PRIX8 " answers, but register 0x%02X would not read", addr[i], _BME280_REG_CHIP_ID);
                else if (_BME280_CHIP_ID_VALID(id) && addr[i] != expect)
                    ESP_LOGW(__tag_device_bme280, "diag: 0x%02" PRIX8 " chip-id=0x%02" PRIX8 " %s <-- THE SENSOR IS HERE, not at 0x%02" PRIX8 ". Set .i2c_addr = 0x%02" PRIX8 " in this board's bme280_config_t, or strap SDO the other way",
                             addr[i], id, _bme280_chip_name(id), expect, addr[i]);
                else
                    ESP_LOGW(__tag_device_bme280, "diag: 0x%02" PRIX8 " chip-id=0x%02" PRIX8 " %s", addr[i], id, _bme280_chip_name(id));
                (void)hw_i2c_dev_del(dev);
            }
        }
    }
    hw_i2c_bus_stop();
}

// ------------------------------------------------------------------------------------------------------------------------

bool bme280_detect(const bme280_config_t *const config) {

    const uint8_t addr = (config != NULL && config->i2c_addr != 0u) ? config->i2c_addr : (uint8_t)BME280_I2C_ADDR;

    if (hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, addr, BME280_I2C_MAX_PAYLOAD) != ESP_OK) {
        ESP_LOGD(__tag_device_bme280, "detect: i2c start failed");
        return false;
    }
    uint8_t id = 0;
    const bool read = (hw_i2c_read_byte(_BME280_REG_CHIP_ID, &id) == ESP_OK);
    hw_i2c_stop();

    const bool found = read && _BME280_CHIP_ID_VALID(id);
    if (found)
        ESP_LOGI(__tag_device_bme280, "detect: present at 0x%02" PRIX8 " (chip-id=0x%02" PRIX8 ")", addr, id);
    else if (read)
        ESP_LOGD(__tag_device_bme280, "detect: 0x%02" PRIX8 " answers but chip-id=0x%02" PRIX8 " is not a BME280/BMP280", addr, id);
    else
        ESP_LOGD(__tag_device_bme280, "detect: nothing at 0x%02" PRIX8, addr);
    return found;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_setup(const bme280_config_t *const config) {

    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_INPUT_GPIO(PIN_DEVICE_I2C_SDA) && GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_I2C_SDA));
    ESP_ERROR_CHECK_BOOLEAN(GPIO_IS_VALID_OUTPUT_GPIO(PIN_DEVICE_I2C_SCL));

    _BME280_RTC_INIT();
    _bme280_rtc.config = *config;
    if (_bme280_rtc.config.i2c_addr == 0u)
        _bme280_rtc.config.i2c_addr = BME280_I2C_ADDR;

    char sb1[STRATEGY_STR_MAX], sb2[STRATEGY_STR_MAX], sb3[STRATEGY_STR_MAX];
    ESP_LOGD(__tag_device_bme280, "setup: strategy_temp=%s, strategy_pres=%s, strategy_humi=%s", reading_strategy_to_str(&_BME280_CONFIG(strategy_temp), sb1, sizeof(sb1)),
             reading_strategy_to_str(&_BME280_CONFIG(strategy_pres), sb2, sizeof(sb2)), reading_strategy_to_str(&_BME280_CONFIG(strategy_humi), sb3, sizeof(sb3)));

    esp_err_t ret;

    ESP_RETURN_ON_ERROR(hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, _BME280_CONFIG(i2c_addr), BME280_I2C_MAX_PAYLOAD), __tag_device_bme280, "start: i2c start");

    ESP_GOTO_ON_ERROR(hw_i2c_read_byte(_BME280_REG_CHIP_ID, &_bme280_rtc.chip_id), bme280_setup_failed, __tag_device_bme280, "start: i2c read (chip-id)");
    ESP_LOGD(__tag_device_bme280, "product: chip-id=0x%02" PRIX8, _bme280_rtc.chip_id);
    ESP_GOTO_ON_FALSE(_BME280_CHIP_ID_VALID(_bme280_rtc.chip_id), DEV_ERR_PRODUCT_ID, bme280_setup_failed, __tag_device_bme280, "start: chip-id invalid: 0x%02" PRIX8 " (expected 0x%02" PRIX8 ")", _bme280_rtc.chip_id, BME280_CHIP_ID_VAL);

    if (BME280_DO_RESET_ON_STARTUP) {
        ESP_GOTO_ON_ERROR(hw_i2c_write_byte(_BME280_REG_RESET, _BME280_RESET_VAL), bme280_setup_failed, __tag_device_bme280, "start: i2c write (soft reset)");
        hw_delay_ms_yieldable(_BME280_START_DELAY_MS); // datasheet says 2ms, but sometimes even up to 20ms
    }

    ESP_GOTO_ON_FALSE(bme280_wait_ready(_BME280_WAIT_DELAY_MS), DEV_ERR_TIMEOUT, bme280_setup_failed, __tag_device_bme280, "start: wait ready");
    ESP_GOTO_ON_ERROR(_bme280_read_calibration(&_bme280_rtc.calib), bme280_setup_failed, __tag_device_bme280, "start: read calibration");

    hw_i2c_stop();

    ESP_LOGI(__tag_device_bme280, "setup: chip-id=0x%02" PRIX8, _bme280_rtc.chip_id);

    return ESP_OK;

bme280_setup_failed:
    (void)bme280_sleep();
    hw_i2c_stop();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_start(void) {

    ESP_RETURN_ON_FALSE(_BME280_RTC_VALID(), DEV_ERR_RTC, __tag_device_bme280, "start: rtc invalid");
    ESP_RETURN_ON_FALSE(_BME280_CHIP_ID_VALID(_bme280_rtc.chip_id), DEV_ERR_PRODUCT_ID, __tag_device_bme280, "start: chip-id invalid: 0x%02" PRIX8 " (expected 0x%02" PRIX8 ")", _bme280_rtc.chip_id, BME280_CHIP_ID_VAL);

    esp_err_t ret;

    ESP_RETURN_ON_ERROR(hw_i2c_start(PIN_DEVICE_I2C_SDA, PIN_DEVICE_I2C_SCL, I2C_FREQ_DEFAULT, _BME280_CONFIG(i2c_addr), BME280_I2C_MAX_PAYLOAD), __tag_device_bme280, "start: i2c start");
    if (BME280_DO_RESET_ON_STARTUP) {
        ESP_GOTO_ON_ERROR(hw_i2c_write_byte(_BME280_REG_RESET, _BME280_RESET_VAL), bme280_start_failed, __tag_device_bme280, "start: i2c write (soft reset)");
        hw_delay_ms_yieldable(_BME280_START_DELAY_MS); // datasheet says 2ms, but sometimes even up to 20ms
    }
    if (BME280_DO_CONF_CONFIG != _BME280_CONF_DEFAULT)
        ESP_GOTO_ON_ERROR(hw_i2c_write_byte(_BME280_REG_CONFIG, BME280_DO_CONF_CONFIG), bme280_start_failed, __tag_device_bme280, "start: i2c write (config)");

    ESP_LOGI(__tag_device_bme280, "started: chip-id=0x%02" PRIX8, _bme280_rtc.chip_id);

    return ESP_OK;

bme280_start_failed:
    (void)bme280_sleep();
    hw_i2c_stop();
    return ret;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_read(bme280_reading_t *const out, const int opts, const reading_strategy_t *const strategy) {
    (void)opts;

    const reading_strategy_t *st_temp = strategy ? strategy : &_BME280_CONFIG(strategy_temp);
    const reading_strategy_t *st_pres = strategy ? strategy : &_BME280_CONFIG(strategy_pres);
    const reading_strategy_t *st_humi = strategy ? strategy : &_BME280_CONFIG(strategy_humi);

    float temps[READINGS_MAX], press[READINGS_MAX], humid[READINGS_MAX];
    int count = 0;

    for (int i = 0; i < MIN_INT(MAX_INT(st_temp->num_total, MAX_INT(st_pres->num_total, st_humi->num_total)), READINGS_MAX); i++) {
        int32_t adc_T, adc_P, adc_H;
        if (_bme280_trigger_and_read_raw(&adc_T, &adc_P, &adc_H) == ESP_OK) {
            temps[count] = _bme280_compensate_temperature(&_bme280_rtc.calib, adc_T);
            press[count] = _bme280_compensate_pressure(&_bme280_rtc.calib, adc_P);
            humid[count] = _bme280_compensate_humidity(&_bme280_rtc.calib, adc_H);
            count++;
        } else
            ESP_LOGW(__tag_device_bme280, "reading %d failed, skipping", i);
    }
    ESP_RETURN_ON_FALSE(count > 0, DEV_ERR_BAD_READING, __tag_device_bme280, "bme280_read");

    readings_process(temps, count, st_temp, &out->temp_quality);
    out->temperature_c = out->temp_quality.value;
    readings_process(press, count, st_pres, &out->pres_quality);
    out->pressure_hpa = out->pres_quality.value;
    readings_process(humid, count, st_humi, &out->humi_quality);
    out->humidity_pct = out->humi_quality.value;

    ESP_LOGI(__tag_device_bme280, "T=%.1f°C P=%.1fhPa H=%.1f%%", (double)out->temperature_c, (double)out->pressure_hpa, (double)out->humidity_pct);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_stop(void) {

    if (bme280_sleep() == ESP_OK)
        if (_BME280_SLEEP_DELAY_MS > 0)
            hw_delay_ms_yieldable(_BME280_SLEEP_DELAY_MS);
    hw_i2c_stop();

    ESP_LOGI(__tag_device_bme280, "stopped");

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t bme280_test(device_test_result_t *const result, const uint32_t duration_ms) {

    result->passed = false;
    const uint32_t sleeping_ms = 1 * 1000;
    const uint32_t start_ms = hw_time_ms();

    esp_err_t rc;
    bme280_reading_t reading;

    const bme280_config_t config = _BME280_RTC_VALID() ? _bme280_rtc.config : bme280_config_default;

    if ((rc = bme280_setup(&config)) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "setup failed: %s", esp_err_to_name(rc));
        return rc;
    }

    if ((rc = bme280_start()) != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "start failed: %s", esp_err_to_name(rc));
        return rc;
    }

    while (rc == ESP_OK && (hw_time_ms() - start_ms) < duration_ms) {
        ESP_LOGD(__tag_device_bme280, "%s: bme280_read", __func__);
        if ((rc = bme280_read(&reading, 0, NULL)) != ESP_OK)
            break;
        if (!reading.temp_quality.valid || !reading.pres_quality.valid) {
            snprintf(result->detail, sizeof(result->detail), "quality fail T:%d P:%d H:%d", reading.temp_quality.valid, reading.pres_quality.valid, reading.humi_quality.valid);
            rc = DEV_ERR_BAD_READING;
        } else if ((hw_time_ms() - start_ms) < duration_ms)
            hw_delay_ms_yieldable((uint32_t)sleeping_ms);
    }

    esp_err_t rc2;
    if ((rc2 = bme280_stop()) != ESP_OK && rc == ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "stop failed: %s", esp_err_to_name(rc));
        return rc2;
    }

    if (rc != ESP_OK) {
        snprintf(result->detail, sizeof(result->detail), "read failed: %s", esp_err_to_name(rc));
        return rc;
    }

    result->passed = true;
    snprintf(result->detail, sizeof(result->detail), "T=%.1fC P=%.0fhPa H=%.0f%%", (double)reading.temperature_c, (double)reading.pressure_hpa, (double)reading.humidity_pct);
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
