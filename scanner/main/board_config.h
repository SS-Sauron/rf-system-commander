/* scanner/main/board_config.h
 *
 * Physical pin assignments for the Scanner ESP32 board.
 * All UART pin Kconfig symbols shadow these values and take precedence;
 * this header serves as a single reference for hardware bringup.
 *
 * UART1 — Data link to Commander
 *   TX = GPIO17  →  Commander UART1 RX (GPIO16)
 *   RX = GPIO16  ←  Commander UART1 TX (GPIO17, reserved/future)
 *
 * UART2 — Local command console
 *   RX = GPIO18
 *   TX = GPIO21
 */

#pragma once

#define BOARD_DATA_UART_TX_PIN   17
#define BOARD_DATA_UART_RX_PIN   16
#define BOARD_CONSOLE_UART_RX_PIN 18
#define BOARD_CONSOLE_UART_TX_PIN 21
