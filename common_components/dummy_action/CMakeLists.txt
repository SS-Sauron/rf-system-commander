# common_components/dummy_action/CMakeLists.txt

idf_component_register(
    SRCS "dummy_action.c"
    INCLUDE_DIRS "."
    REQUIRES
        action_registry   # action_module_t type + ACTION_MODULE_API_VERSION
        output            # output_write() called from dummy_execute()
        log               # ESP_LOGI / ESP_LOGW / ESP_LOGE
)

component_compile_options(-Wvla -Werror=vla)
