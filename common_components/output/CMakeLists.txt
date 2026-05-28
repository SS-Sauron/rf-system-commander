
idf_component_register(
    SRCS "output.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_timer
)

# Safety flag: forbid variable-length arrays (VLAs).
# VLAs allocate an unknown amount of stack at runtime — banned in this
# firmware. The compiler will error if any VLA is introduced by mistake.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)
