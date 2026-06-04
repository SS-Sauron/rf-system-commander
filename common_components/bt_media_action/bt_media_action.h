/* common_components/bt_media_action/bt_media_action.h */

#pragma once

#include "action_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bluetooth AVRCP/A2DP action module instance.
 *
 * Pass &bt_media_action_module to action_registry_register() once during
 * boot, after action_registry_init().
 *
 * Supported execute() commands (JSON object):
 *   {"action":"play"}
 *   {"action":"pause"}
 *   {"action":"vol_up"}
 *   {"action":"vol_down"}
 *   {"action":"connect","target":"AA:BB:CC:DD:EE:FF"}
 *   {"action":"disconnect"}
 *
 * The "target" field is optional on media commands; if provided and the
 * module is disconnected, it will connect to that address first.
 */
extern action_module_t bt_media_action_module;

#ifdef __cplusplus
}
#endif
