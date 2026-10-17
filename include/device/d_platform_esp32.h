
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_platform_esp32.h
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#ifndef PLATFORM_ESP32
#define PLATFORM_ESP32
#endif

#include "d_platform.h"

// ------------------------------------------------------------------------------------------------------------------------

/* WHAT A DURABLE RECORD STORE LOOKS LIKE HERE, for anything that has to site and size one without
   knowing which platform it is on. A labelled flash partition is the store; the reset-surviving
   ring in front of it is small because RTC RAM is small. */
#define D_PERSIST_STORE_PARTITION 1
#define D_PERSIST_POOL_SZ_DEFAULT 2048u

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wredundant-decls"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "rom/ets_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* <time.h> FOR hw_clock() BELOW, and it needs the suppression: IDF carries its own
   esp_libc/platform_include/time.h which redeclares clock_settime/clock_gettime/clock_getres on top
   of newlib's, and the project builds with -Werror=redundant-decls. The demotion is scoped to this
   one include rather than being turned off for the component -- nothing of ours is excused by it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <time.h>
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

/* THE WALL CLOCK, and the one place a port has to think about one. hw_time_ms() is what anything
   that must HAPPEN runs on -- it exists from reset and never steps -- so this is for the other two
   jobs only: an instant a reader reads as a date, and a stamp another host compares against its
   own clock. time_t is the platform's own, 2038 and all: when that has to change, it changes here.
   See hw_clock_is_set() in d_common.h -- the value may be nothing but seconds since boot. */
static inline time_t hw_clock(void) {
    return time(NULL);
}

#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------

/* WHETHER A SUPPLY FLOOR IS ENFORCED IN HARDWARE, and at what setting, for anything that wants to
   record what actually stops this node. Declared here and not read from sdkconfig by the caller:
   CONFIG_ESP_BROWNOUT_DET_LVL is a vendor Kconfig name, and a node header asking for one by that
   name is how platform knowledge gets back into the layer above. Absent means no detector -- which
   is the honest answer on a host, where the supply is somebody else's problem.

   NOTE THE PLACEMENT: after the IDF includes above, because sdkconfig.h arrives with them. Moved
   up beside D_PERSIST_* it would test a macro that is not defined yet and silently come out false,
   with nothing to see in the build. */
#if defined(CONFIG_ESP_BROWNOUT_DET) && defined(CONFIG_ESP_BROWNOUT_DET_LVL)
#define D_PLATFORM_SUPPLY_FLOOR_LVL CONFIG_ESP_BROWNOUT_DET_LVL
#endif

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------

/* THE EFUSE MAC IS THE BOARD, burned at the factory and not removable -- so there is no second
   source worth having and alt32 says so by answering 0. */
static inline bool platform_identity_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        mac[i] = 0;
    return esp_efuse_mac_get_default(mac) == ESP_OK;
}

static inline uint32_t platform_identity_alt32(void) {
    return 0u;
}

static inline const char *platform_identity_alt_name(char *const buf, const size_t size) {
    if (buf != NULL && size > 0)
        buf[0] = '\0';
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------

/* IDF OWNS THE CEILING, and esp_log_level_set() is how it is moved -- but it cannot be read back,
   so the last value set is mirrored here. CONFIG_LOG_DEFAULT_LEVEL is where it starts, and
   CONFIG_LOG_MAXIMUM_LEVEL is a compile-time cap this cannot raise past: a build that compiled
   debug out stays quiet however high the ceiling is set. */
static platform_log_level_t _platform_log_level = (platform_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;

static inline platform_log_level_t platform_log_level_get(void) {
    return _platform_log_level;
}

static inline void platform_log_level_set(const platform_log_level_t level) {
    esp_log_level_set("*", (esp_log_level_t)level);
    _platform_log_level = level;
}

static inline platform_reset_t platform_reset_reason(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
    case ESP_RST_USB: // development "power on"
        return PLATFORM_RESET_POWER_ON;
    case ESP_RST_SW:
        return PLATFORM_RESET_SOFTWARE;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return PLATFORM_RESET_WATCHDOG;
    case ESP_RST_BROWNOUT:
        return PLATFORM_RESET_BROWNOUT;
    case ESP_RST_PANIC:
        return PLATFORM_RESET_PANIC;
    case ESP_RST_DEEPSLEEP:
        return PLATFORM_RESET_DEEPSLEEP;
    case ESP_RST_EXT:
        return PLATFORM_RESET_EXTERNAL;
    default:
        return PLATFORM_RESET_UNKNOWN;
    }
}

static inline const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:
        return "POWERON (cold boot / power cycle)";
    case ESP_RST_EXT:
        return "EXT (external reset pin)";
    case ESP_RST_SW:
        return "SW (esp_restart / software)";
    case ESP_RST_PANIC:
        return "PANIC (exception/abort crash)";
    case ESP_RST_INT_WDT:
        return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:
        return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP (wake from deep sleep)";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT (power dip — check USB/cable/supply)";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB (reset over USB peripheral)";
    case ESP_RST_JTAG:
        return "JTAG";
    default:
        return "UNKNOWN";
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
