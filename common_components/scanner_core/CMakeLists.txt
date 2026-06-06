# common_components/scanner_core/CMakeLists.txt

idf_component_register(
    SRCS "scanner_core.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos   # FreeRTOS timer + task + task notification
        log        # ESP_LOGI / ESP_LOGW / ESP_LOGE
        output     # output_write() — used only if no event callback is set
        watchdog    # watchdog_reset()
)

component_compile_options(-Wvla -Werror=vla)
