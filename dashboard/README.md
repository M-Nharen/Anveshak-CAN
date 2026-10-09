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

- **Viewing needs no login.** Arm, disarm, leader choice, release E-stop and driving need the
  password. **E-stop never needs it** and goes over its own gateway connection.
- **Hold-to-run.** The browser sends `drive` at 20 Hz only while input is held; the backend sends
  zero speed if nothing arrives for 200 ms (and straight away when the holder disconnects).
  While a browser drives, the backend refreshes targets to the gateway at 50 Hz (the gateway
  itself zeroes any target older than 100 ms).
- **Control lock.** One browser at a time; released when that browser disconnects.
- **Steering.** Left side = drives 1-3, right side = 4-6 (an assumption, `Settings.left_drives`).
  Wheel speed = joystick x speed level x speed cap, all clamped; both motor channels follow the side.
- **Bus load** is an estimate: (received + sent frames per second) x 111 bits / 500 kbit/s.
- **Not done here (later work):** control-source arbitration between autonomy / teleop / dashboard
  inside the gateway (the header shows only whether *this* dashboard holds control), the link to a
  gateway on the other host, and the other pages.
