# IO Operations & JSON Formatting Protocols

## 1. Thread-Safe Output Processing (`common_components/output`)
- **JSON Framing:** All calls to `output_write(const char *body_json)` must wrap data blocks where `body_json` strictly starts with an opened curly brace `{`.
- **Envelope Wrapping:** The engine wraps the payload inside a standardized metadata array footprint (`source`, `device_id`, `uptime_ms`, `msg`) before dumping to the transmission layer.
- **Re-entrancy:** `output_write` utilizes a static internal assembly buffer protected by a dedicated mutex. **It is not re-entrant.**

## 2. JSON Tokenization Primitives (`common_components/json_utils`)
- Always leverage the internal zero-allocation, pointer-walking engine handlers (`json_get_string_field`, `json_get_raw_field`) to pull data strings from frames. 
- Do not pull in heavy third-party parsing tools or allocate working memory slices on the heap.

## 3. Current Buffer Allocations
- `OUTPUT_LINE_BUF_SIZE`: Constant value 2048 bytes.
- `CONFIG_RULE_CMD_TMPL_LEN`: Default 256 bytes.
- Maximum placeholder token substitutions allowed per sequence: 4.