# common_components/scan_parser/CMakeLists.txt

idf_component_register(
    SRCS "scan_parser.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos          # FreeRTOS task and queue primitives
        log               # ESP_LOGI / ESP_LOGW / ESP_LOGE
        output            # output_write() for structured JSON output
        watchdog          # watchdog_register_task() / watchdog_feed_task()
        scan_receiver     # SCAN_RECEIVER_LINE_BUF constant from scan_receiver.h
)

# Apply VLA ban to this component only (matches the rest of our owned components).
# NOTE: ESP-IDF v6.x — do NOT apply globally; IDF internals use VLAs.
idf_component_get_property(lib ${COMPONENT_NAME} COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wvla -Werror=vla)