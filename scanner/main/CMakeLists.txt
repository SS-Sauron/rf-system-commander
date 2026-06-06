# scanner/main/CMakeLists.txt

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
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
        scanner_core
        scanner_console
        scanner_link
)

# VLA ban — enforced on scanner main just as on every other component.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wvla -Werror=vla)
