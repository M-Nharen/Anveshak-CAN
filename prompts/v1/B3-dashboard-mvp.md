# B3 — Dashboard, minimum version

**Who:** person B. **Needs:** B1 merged (B2 not needed). **Folders:** `dashboard/`, the dashboard
CI job.

## Goal

A web page served by the gateway's host that shows the bus and every node live, and lets the
operator choose the leader, arm/disarm, E-stop and drive the rover with hold-to-run controls.

## Read first

`docs/architecture.md` section "Master dashboard": "How data gets from a node to your browser",
"How it works", the Overview and Nodes rows of the "Pages" table, and parts 1–2 of "Control
panel" (who is in control, drive controls). Also "Safety rules in the dashboard".

## Build

1. **Backend** (`dashboard/backend/`, FastAPI): talks to the gateway through its local API. A
   small C++ ↔ Python bridge (a Unix socket or local UDP JSON protocol exposed by the gateway is
   fine; add it to the gateway in its own small file and note it in the PR). WebSocket push at
   10–20 Hz, sending only what the open page needs.
2. **Frontend** (`dashboard/frontend/`, Svelte + uPlot), built to static files that the backend
   serves:
   - **Header on every page**: which host leads and the term, "No leader — choose one" prompt,
     buttons "Make Orin lead" / "Make RPi lead", E-stop button, armed/disarmed state,
     control-source indicator.
   - **Overview**: bus state, bus load %, frames/s, error frames/s, RPi overrun count.
   - **Nodes**: one row per node with state, heartbeat age, firmware build, uptime, TEC/REC,
     temperatures, fault bits, colour-coded health.
   - **Drive**: "Take manual control" / "Release"; on-screen joystick and keyboard (WASD/arrows);
     speed + / − buttons; speed cap and acceleration limit sliders; per-wheel target vs
     measured. **Hold-to-run**: targets sent at 20 Hz only while held; the backend zeroes them
     if nothing arrives for 200 ms.
3. **Control lock**: only one browser can hold manual control; it is released automatically when
   that browser disconnects.
4. Simple login: viewing needs no login; anything that moves hardware needs it.

## Done when

- Backend tests (pytest) pass, including: drive targets go to zero 200 ms after the last
  message, and a second browser can't take control while the first holds it.
- Frontend builds (`npm run build`) in CI.
- Running gateway + simulator + dashboard locally, the operator can choose a leader, arm, drive
  with the keyboard, and see the simulated wheels respond.

## Out of scope

Rates page, parameters, firmware page, errors page, debug levels, frame lab (B4–B6).
