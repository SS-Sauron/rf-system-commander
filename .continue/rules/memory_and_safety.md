# Embedded Memory Management & System Safety Guardrails

This project runs on resource-constrained hardware with tight execution constraints. You must strictly enforce these safety paradigms in every code modification or code generation task.

## 1. Static Memory Rules (Zero Post-Boot Heap Allocation)
- **Dynamic Allocation Ban:** `malloc`, `calloc`, or FreeRTOS queue allocations are permissible **only during boot initialization sequences** in `main.c`. 
- **Critical Paths:** No allocations inside runtime tasks, callbacks, or loop iterations. Every processing buffer size must be compile-time bounded via Kconfig parameters.
- **Strict VLA Ban:** Variable-Length Arrays (VLAs) on the stack are strictly forbidden. All compilation targets use `-Wvla -Werror=vla`. Ensure all local arrays have explicit literal or constant bounds.

## 2. ESP-IDF v6.1-dev & Picolibc Task Limits
- **Stack Thresholds:** Due to picolibc execution layers, **never allocate a task stack size lower than 4096 bytes** (`configMINIMAL_STACK_SIZE` is insufficient). Monitor watermarks closely.
- **Driver Split:** Utilize the modernized `esp_driver_uart` component layer interfaces exclusively; do not fall back to deprecated legacy UART drivers.

## 3. Watchdog Lifecycle (Task Monitor Pattern)
Every persistent execution loop must integrate with the central hardware-backed Task Watchdog Timer (TWDT) manager component (`common_components/watchdog`):
- **Registration Order:** Tasks must register *before* creation:
  `watchdog_register_task("task_name", max_interval_ms);` followed immediately by `xTaskCreate(...)`.
- **Feeding Pattern:** Call `watchdog_feed_task("task_name");` exactly at the top of the task's primary `for(;;)` loop.
- **Constraint:** Never write log statements (`ESP_LOGx`) inside the core watchdog monitor task loop itself to prevent deadlock cascades.

## 4. Mutex Lock Ordering
To eliminate race conditions and execution stalls, lock operations must always adhere to this unidirectional sequence:
1. Acquire `rule_table_mutex`
2. Acquire `output_mutex`
*Never invert this execution pipeline order.*