# common_components/action_registry/CMakeLists.txt
# Updated at C4: no longer needs output in REQUIRES for the stub log.
# output is still needed by action_registry.c for error path emit_error().

idf_component_register(
    SRCS "action_registry.c"
    INCLUDE_DIRS "."
    REQUIRES
        log       # ESP_LOGI / ESP_LOGW / ESP_LOGE
        output    # output_write() for error path emit_error() calls
)

component_compile_options(-Wvla -Werror=vla)
