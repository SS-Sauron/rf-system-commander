# common_components/scan_parser/CMakeLists.txt
# Updated at C3: added json_utils to REQUIRES.

idf_component_register(
    SRCS "scan_parser.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        output
        watchdog
        scan_receiver
        json_utils        # Added at C3 — provides json_skip_ws, json_parse_string, etc.
)

component_compile_options(-Wvla -Werror=vla)
