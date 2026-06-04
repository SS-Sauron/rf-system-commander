# common_components/health/CMakeLists.txt
#
# ESP-IDF component registration for the shared health monitor.
# Both scanner and commander add this via EXTRA_COMPONENT_DIRS
# in their top-level CMakeLists.txt.

idf_component_register(
    SRCS "health.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos      # FreeRTOS task primitives, uxTaskGetSystemState
        log           # ESP_LOGI / ESP_LOGW / ESP_LOGE
        heap          # esp_get_free_heap_size, heap_caps_get_largest_free_block
        esp_timer     # esp_timer_get_time() — used by health_get_snapshot (C5)
        watchdog       # health_get_snapshot() needs to know if the watchdog is active
)
