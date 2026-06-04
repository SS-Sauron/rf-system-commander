# RF System Commander Architecture Blueprint

You are working on the **RF System Commander Firmware** (currently at Stage C4). The project is a headless, distributed embedded system built on **ESP-IDF v6.1-dev**.

## 1. Directory Structure & Placements
All new code must strictly adhere to the following component boundaries:
- `common_components/`: Pure logic and underlying drivers shared across targets.
  - `health/`: System heap/stack runtime telemetries.
  - `output/`: Thread-safe JSON serialization layer.
  - `watchdog/`: Dedicated heartbeat monitor task.
  - `scan_receiver/` & `scan_parser/`: UART1 RX stream processing and JSON schema parsing.
  - `json_utils/`: Zero-allocation pointer-based parsing primitives.
  - `rule_engine/`: NVS-backed rule evaluation logic.
  - `action_registry/`: Dynamic module registration and routing interface.
- `commander/`: Top-level application orchestrator firmware project.
- `scanner/`: Secondary input node placeholder.

## 2. Communication Contracts
- **Headless Serial Interaction:** Communication with external devices happens strictly via raw serial JSON lines over UART (and future ESP-NOW). 
- **Decoupled Modules:** No global variables or cross-component direct mutations are allowed. Inter-module communication must use well-defined C structures or public API boundary paths.
- **Versioned Protocols:** `scan_module_t` and `action_module_t` must maintain explicit `api_version` attributes. Registration actions must reject runtime mismatches.