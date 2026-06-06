# common_components/scanner_console/CMakeLists.txt

idf_component_register(
    SRCS "scanner_console.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos        # FreeRTOS task primitives
        esp_driver_uart # UART driver (ESP-IDF v6.x split component)
        log             # ESP_LOGI / ESP_LOGE
        watchdog        # watchdog_register_task / watchdog_feed_task
        health          # health_get_snapshot (STATUS command)
        scanner_core    # scanner_core_start_ble / stop_ble
)

component_compile_options(-Wvla -Werror=vla)
