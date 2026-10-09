# anveshak_can_core: diagnostics (A4)

Opt-in per node: call `anv_node_diag_start(node, debug_default, now_ms)` after `anv_node_init()`
and before `anv_node_boot_complete()`. A node that never calls it sends no NODE_EVENT and reports
debug level 0, exactly as before.

| Module | What it does |
| --- | --- |
| `anv_diag.[ch]` | NODE_EVENT (edge-triggered, cleared bit, 1 per code per second, sequence number), RAM ring of 64, flash log of the last 32 errors/criticals, boot count |
| `anv_debug.[ch]` | Debug levels 0-3, category mask, auto-off timer, telemetry/debug-page rates, heartbeat floor |
| `anv_node.c` | Wires both into the node: DEBUG_LEVEL frame, NODE_HB level, automatic events, LOG_READ/LOG_CLEAR, INFO pages |

Applications raise their own conditions with `anv_node_raise()` / `anv_node_lower()` (a condition
that stays true: OVERCURRENT_A) and `anv_node_event()` (a one-off: BAD_FRAME). Severity always comes
from the generated registry (`defs/errors.yaml`).

## Board hooks (all optional)

`log_load` / `log_save` (one blob of `ANV_DIAG_BLOB_SIZE` bytes, CRC-checked), `take_crash_record`,
`reset_cause`, `self_test`. Without `log_load/save` the log is RAM-only.

## SERVICE additions

Requests use the normal SERVICE layout (opcode, transaction id, index, value).

* **LOG_READ (6)**, `index` = entry number, 0 = newest. Two responses per entry:
  * index `n`: `value_raw = code << 16 | detail`
  * index `n | 0x8000`: `status` = flags (bit 0 cleared, bit 1 kept off the bus by the debug level),
    `value_raw = uptime_0.1s << 16 | boot_count`. Severity follows from the code.
  * An index past the end gets one response with status 1 (bad index).
* **LOG_CLEAR (7)**: wipes RAM and flash log (boot count kept). Status 3 while armed.
* **INFO pages** (read with opcode 5), on top of the existing 0 to 4:

| Page | Value |
| --- | --- |
| 16 | crash record present (0/1) |
| 17-23 | crash PC, LR, xPSR, CFSR, HFSR, MMFAR, BFAR (ESP32-S3: PC only) |
| 24 | boot count |
| 25 | entries in the log |
| 26 | last reset cause (`anv_reset_cause_t`) |
| 27 | self-test failure bitmask (bit n = test n+1; 0 = passed) |
| 28 | crash record breadcrumb |
| 32-37 | CAN error counts: stuff, form, ack, bit, crc, other |

## Notes

* Rate limiting folds events inside a second into one frame (latest detail wins); the log entry's
  `repeat` says how many were folded.
* The flash log is not written periodically while the node is armed (a flash erase can stall the
  CPU), except for criticals, which are written at once.
* Category-mask bit 3 (error counts by type) is stored and reported but gates nothing yet: the
  counts are always available on the INFO pages.
