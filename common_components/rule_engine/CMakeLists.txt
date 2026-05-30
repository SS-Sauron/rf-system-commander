# common_components/rule_engine/CMakeLists.txt

idf_component_register(
    SRCS "rule_engine.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos          # FreeRTOS task, queue, semaphore
        log               # ESP_LOGI / ESP_LOGW / ESP_LOGE
        nvs_flash         # nvs_open / nvs_get_str / nvs_get_u16 / nvs_close
        json_utils        # json_skip_ws, json_parse_string, json_skip_value,
                          # json_get_string_field — field extraction during evaluation
        action_registry   # action_registry_execute() — dispatch interface
        scan_parser       # scan_event_t, SCAN_PARSER_API_VERSION
        output            # output_write() for any warnings emitted during evaluation
        watchdog          # watchdog_register_task / watchdog_feed_task
)

component_compile_options(-Wvla -Werror=vla)
