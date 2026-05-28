idf_component_register(
    SRCS "watchdog.c"
    INCLUDE_DIRS "."
    # NOTE: esp_task_wdt merged into esp_system in ESP-IDF v6.x.
    # If porting back to v5.x, restore: REQUIRES esp_task_wdt
    REQUIRES
        freertos
        log
        esp_system
)

# Safety flag: forbid variable-length arrays (VLAs).
# VLAs allocate an unknown amount of stack at runtime — banned in this
# firmware. The compiler will error if any VLA is introduced by mistake.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)
