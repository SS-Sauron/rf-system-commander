/**
 * @file dummy_action.h
 * @brief Dummy action module — public symbol declaration.
 *
 * COMPONENT LOCATION: common_components/dummy_action/
 *
 * PURPOSE
 * -------
 * Exposes the single symbol that main.c needs to register this module:
 *   action_registry_register(&dummy_action_module);
 *
 * The module struct itself is defined in dummy_action.c. Only main.c
 * (and test code) should include this header. The action registry accesses
 * the module exclusively through the pointer passed at registration time.
 *
 * WHEN TO REMOVE THIS MODULE
 * --------------------------
 * The dummy module stays in the firmware indefinitely as a testing tool.
 * It has zero hardware dependencies and negligible DRAM cost (~50 bytes
 * for its struct + a static execution counter). Even in production builds
 * it is useful for verifying that the rule engine and registry are alive
 * without requiring a connected Bluetooth device.
 * If binary size becomes a concern, gate it behind CONFIG_DUMMY_ACTION_ENABLE.
 */

#pragma once

#include "action_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The dummy action module instance.
 *
 * Defined in dummy_action.c. Pass &dummy_action_module to
 * action_registry_register() exactly once at boot.
 */
extern action_module_t dummy_action_module;

#ifdef __cplusplus
}
#endif
