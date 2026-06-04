idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."          # exposes board_config.h to all srcs in this component
    REQUIRES
        nvs_flash
        freertos
        log
        health
        output
        watchdog
        scan_receiver
        scan_parser
        json_utils
        action_registry
        rule_engine
        dummy_action
        command_parser
        bt_media_action
)

# Safety flag: forbid variable-length arrays (VLAs).
# VLAs allocate an unknown amount of stack at runtime — banned in this
# firmware. The compiler will error if any VLA is introduced by mistake.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)