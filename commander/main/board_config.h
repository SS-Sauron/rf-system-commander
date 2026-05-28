/**
 * @file board_config.h
 * @brief Hardware pin assignments for the Commander ESP32.
 *
 * ALL hardware-specific constants live here.
 * No GPIO number, baud rate, or peripheral index may appear as a
 * magic number anywhere else in the commander firmware.
 *
 * Update this file for each board revision before building.
 * The file is included by any source that needs a hardware constant;
 * it must contain only preprocessor definitions (no code, no includes).
 *
 * COMPONENT LOCATION: commander/main/board_config.h
 */

#pragma once

/* ======================================================================
 * Scanner Input UART
 * One-way: Scanner TX → Commander RX.
 * The TX pin is wired on the PCB for future bidirectional use (Stage C6b
 * or later) but the TX driver is NOT installed in firmware at C0/C1.
 * ====================================================================== */

/** UART peripheral used for scanner input.
 *  Default: UART1, keeping UART0 free for local command I/O and logging.
 *  Must match CONFIG_SCANNER_UART_NUM in Kconfig. */
#define BOARD_SCANNER_UART_NUM      1

/** GPIO receiving JSON lines from the scanner's TX pin. */
#define BOARD_SCANNER_UART_RX_PIN   16

/** GPIO routed to the scanner's RX pin — unused in firmware until C6b.
 *  Defined here so the board layout is documented in code. */
#define BOARD_SCANNER_UART_TX_PIN   17

/* ======================================================================
 * Local Command & Log UART  (UART0)
 * Used by: cmd.c (local serial command parser), ESP_LOGx output.
 * Default UART0 pins on the ESP32; do not reassign without also
 * updating the UART driver configuration in cmd.c and any boot-log output.
 * ====================================================================== */

#define BOARD_CMD_UART_NUM          0
#define BOARD_CMD_UART_RX_PIN       3   /* U0RXD — standard ESP32 UART0 */
#define BOARD_CMD_UART_TX_PIN       1   /* U0TXD — standard ESP32 UART0 */

/* ======================================================================
 * Status LED  (optional)
 * A single GPIO LED for visual heartbeat / event indication.
 * Set BOARD_STATUS_LED_PIN to -1 to disable LED handling entirely.
 * ====================================================================== */

#define BOARD_STATUS_LED_PIN            2
#define BOARD_STATUS_LED_ACTIVE_HIGH    1   /* 1 = LED on when GPIO high */

/* ======================================================================
 * Bluetooth AVRCP (Stage C6)
 * No GPIO required for AVRCP; recorded here for documentation clarity.
 * The BT radio uses the internal antenna; external antenna selection
 * (if fitted) would be controlled via CONFIG_ESP32_PHY_INIT_DATA_IN_PARTITION.
 * ====================================================================== */

/* (No GPIO definitions needed for BT at this stage.) */
