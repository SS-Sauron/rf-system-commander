idf_component_register(
    SRCS "scan_receiver.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        freertos
        log
        esp_system
        output
        watchdog
)

# Safety flag: forbid variable-length arrays (VLAs).
# VLAs allocate an unknown amount of stack at runtime — banned in this
# firmware. The compiler will error if any VLA is introduced by mistake.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)
