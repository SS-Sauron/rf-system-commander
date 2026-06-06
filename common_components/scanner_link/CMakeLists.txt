# common_components/scanner_link/CMakeLists.txt

idf_component_register(
    SRCS "scanner_link.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos        # FreeRTOS task, semaphore
        esp_driver_uart # UART driver (ESP-IDF v6.x split component)
        log             # ESP_LOGI / ESP_LOGW / ESP_LOGE
        watchdog        # watchdog_register_task / watchdog_feed_task
        json_utils      # json_get_string_field for incoming command parsing
        scanner_core    # scanner_core_start_ble / stop_ble (command dispatch)
)

component_compile_options(-Wvla -Werror=vla)
