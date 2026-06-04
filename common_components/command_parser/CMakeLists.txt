# common_components/command_parser/CMakeLists.txt

idf_component_register(
    SRCS "command_parser.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos          # FreeRTOS task primitives
        esp_driver_uart   # UART driver (ESP-IDF v6.x split component)
        log               # ESP_LOGI / ESP_LOGE
        watchdog          # watchdog_register_task / watchdog_feed_task
        rule_engine       # rule_engine_add_rule, delete, enable, list, test, count
        action_registry   # action_registry_get_module_count, get_module
        health            # health_get_snapshot
)

component_compile_options(-Wvla -Werror=vla)
