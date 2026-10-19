
// ------------------------------------------------------------------------------------------------------------------------
// TIMER
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_timer.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

#define hw_timer_handle_t esp_timer_handle_t
#define hw_timer_conf_t   esp_timer_create_args_t

// ------------------------------------------------------------------------------------------------------------------------

static inline int64_t hw_timer_get_time(void) {
    return esp_timer_get_time();
}

// ------------------------------------------------------------------------------------------------------------------------

hw_timer_handle_t hw_timer_create(const hw_timer_conf_t *const args) {
    hw_timer_handle_t handle;
    ESP_ERROR_CHECK(esp_timer_create(args, &handle));
    return handle;
}

// ------------------------------------------------------------------------------------------------------------------------

void hw_timer_delete(const hw_timer_handle_t handle) {
    ESP_ERROR_CHECK(esp_timer_delete(handle));
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void hw_timer_start_periodic(const hw_timer_handle_t handle, const uint64_t period_us) {
    ESP_ERROR_CHECK(esp_timer_start_periodic(handle, period_us));
}

// ------------------------------------------------------------------------------------------------------------------------

static inline void hw_timer_stop(const hw_timer_handle_t handle) {
    ESP_ERROR_CHECK(esp_timer_stop(handle));
}

// ------------------------------------------------------------------------------------------------------------------------
