# /* common_components/bt_media_action/CMakeLists.txt */

idf_component_register(
    SRCS "bt_media_action.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos        # FreeRTOS task, queue
        log             # ESP_LOGI / ESP_LOGW / ESP_LOGE / ESP_LOGD
        action_registry # action_module_t interface + ACTION_MODULE_API_VERSION
        output          # output_write() — NOT used directly; pulled for consistency
        watchdog        # watchdog_register_task / watchdog_feed_task
        bt              # esp_bt.h, esp_bt_main.h, esp_a2dp_api.h, esp_avrc_api.h
        json_utils      # json_get_string_field for execute() JSON parsing
)

component_compile_options(-Wvla -Werror=vla)
