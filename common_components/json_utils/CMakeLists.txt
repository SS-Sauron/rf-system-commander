# common_components/json_utils/CMakeLists.txt
#
# Zero-dependency JSON parsing utilities shared between scan_parser,
# rule_engine, and any future component that reads JSON fields.

idf_component_register(
    SRCS "json_utils.c"
    INCLUDE_DIRS "."
    # No REQUIRES: this component has no ESP-IDF dependencies.
    # It uses only the C standard library (string.h, stdbool.h, etc.),
    # which is always available.
)

component_compile_options(-Wvla -Werror=vla)
