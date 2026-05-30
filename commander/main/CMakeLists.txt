
idf_component_register(
    SRCS
        "main.c"
        # ---- Uncomment as each stage is implemented ----
        # Stage C2: "scan_parser.c"
        # Stage C3: "rule_engine.c"
        # Stage C4: "action_registry.c"
        #           "dummy_action.c"
        # Stage C5: "cmd.c"
        #           "settings.c"
        # Stage C6: "bt_media_action.c"
        # Stage C7: "wifi_deauth_action.c"
        #           "gpio_action.c"
    INCLUDE_DIRS
        "."          # exposes board_config.h to all srcs in this component
    REQUIRES
        nvs_flash    # nvs_flash_init(), nvs_open(), nvs_set_str(), etc.
        freertos     # FreeRTOS primitives used directly in main.c
        log          # ESP_LOGI / ESP_LOGE in main.c
        health       # shared health monitor component
        output       # shared JSON output component
        watchdog
        # ---- Add as stages are implemented ----
        esp_driver_uart       # (UART driver for scanner input)
        scan_receiver
        scan_parser
        # Stage C3: json            (cJSON or similar for rule parsing)
        # Stage C6: bt              (Bluedroid AVRCP controller)
)

# Safety flag: forbid variable-length arrays (VLAs).
# VLAs allocate an unknown amount of stack at runtime — banned in this
# firmware. The compiler will error if any VLA is introduced by mistake.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)

