# Dashboard (minimum version)

A web page served by a host's gateway: bus and node health, leader choice, arm/disarm, E-stop and
hold-to-run driving. Spec: `docs/architecture.md`, "Master dashboard".

```
browser --WebSocket--> dashboard/backend (FastAPI) --Unix socket, JSON lines--> anv_can_gateway --CAN--> nodes
```

## Run it (with the simulator)

```
# 1. gateway with the bridge on (needs a CAN interface; the simulator can run on vcan0)
anv_can_gateway --interface vcan0 --bridge-socket /tmp/anv-gateway.sock
# 2. the dashboard backend, serving the built frontend
(cd dashboard/frontend && npm install && npm run build)
pip install -r dashboard/backend/requirements.txt
ANV_DASH_GATEWAY_SOCKET=/tmp/anv-gateway.sock ANV_DASH_STATIC=dashboard/frontend/dist \
ANV_DASH_PASSWORD=changeme python -m anv_dashboard      # from dashboard/backend, port 8080
```

Open `http://<host>:8080`. Choose a leader ("Make Orin lead"), **Arm**, open **Drive**, **Take manual
control** and hold W/A/S/D or the arrow keys. For frontend work, `npm run dev` proxies to the backend.

Settings (environment): `ANV_DASH_GATEWAY_SOCKET`, `ANV_DASH_PASSWORD` (if unset, a random one is
printed at start), `ANV_DASH_STATIC`, `ANV_DASH_HOST`, `ANV_DASH_PORT`, `ANV_DASH_MAX_WHEEL_SPEED`,
`ANV_DASH_DEFAULT_CAP`.

## Test

```
cd dashboard/backend && pip install -r requirements.txt && pytest
cd dashboard/frontend && npm install && npm run build
```

## Behaviour worth knowing

- **Viewing needs no login.** Arm, disarm, leader choice, release E-stop, driving, rates,
  parameters and the test bench need the password. **E-stop never needs it** and goes over its
  own gateway connection.
- **Hold-to-run.** The browser sends `drive` at 20 Hz only while input is held; the backend sends
  zero speed if nothing arrives for 200 ms (and straight away when the holder disconnects).
  While a browser drives, the backend refreshes targets to the gateway at 50 Hz (the gateway
  itself zeroes any target older than 100 ms). The Test bench's jog is the same rule, independently.
- **Control lock.** One browser at a time; released when that browser disconnects.
- **Steering.** Left side = drives 1-3, right side = 4-6 (an assumption, `Settings.left_drives`).
  Wheel speed = joystick x speed level x speed cap, all clamped; both motor channels follow the side.
- **Bus load** (Overview) is an estimate: (received + sent frames per second) x 111 bits / 500 kbit/s.
- **Rates page** (`anv_dashboard/rates.py`): every periodic message's rate, with the floors from
  `docs/architecture.md` (host heartbeat >= 20 Hz, node heartbeat >= 5 Hz, control cycle 25-50 Hz)
  and a 50% bus-load guard, checked against both the catalog estimate and the gateway's actually
  measured load (the catalog alone never reaches 50% at the parameters' own ranges). Host rates
  are gateway settings (`set_cycle_rate_hz`/`set_host_hb_rate_hz`, RAM-only); node rates go through
  SERVICE SET on the node's `rate_fb_hz`/`rate_telem_hz`/`rate_hb_hz` parameter. "Save as default"
  writes the node rates to flash (SERVICE SAVE); host rates have no flash to write to.
- **Parameters page** (`anv_dashboard/paramcodec.py`, `.../views.py`): the drive node parameter
  table from `defs/params/drive.yaml` (via the generated `anv_can_defs` package). A write is
  refused, without ever reaching the gateway, if it is out of range or if the parameter is
  `disarmed_only` and the rover is armed; the node's own refusal (SERVICE STATUS != 0) is
  surfaced the same way as a second line of defence.
- **Firmware page**: build ID per node, flagging a mismatch within a node type. The flash button
  is disabled with a note: `anv-flash` (A3) hasn't merged yet.
- **Test bench**: single-motor jog, PID gains (reuses the Parameters guard), and a step-response
  sampler that records target vs. measured speed while the jog runs.
- **Audit and undo** (`anv_dashboard/audit.py`): every rate/parameter change and refusal is kept
  in an in-memory log (`Hub.audit`); "Undo last change" re-applies the previous value through the
  same guarded path and is itself logged.
- **Not done here (later work):** control-source arbitration between autonomy / teleop / dashboard
  inside the gateway (the header shows only whether *this* dashboard holds control), the link to a
  gateway on the other host, and the other pages (raw traffic, frame lab, profiles, events/logs).
