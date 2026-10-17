# Anveshak Rover CAN 2.0A Architecture

Oct 9, 2026 · @Nharen

> Live, editable version: https://claude.ai/code/artifact/455c9bfd-96bd-4eb0-93cc-b2e3a0f068b2
>
> Decisions as of Oct 9, 2026: CAN 2.0A at 500 kbps (RPi MCP2515 crystal unknown, designed for 8 MHz worst case), 50 Hz control cycle, Option A leadership (operator chooses the leading host, RPi can take over any time, no default, auto-takeover off), ROS 2 inside each host only, each host independently on Humble or Jazzy, hosts linked by CAN plus a UDP gateway link, debug levels 0-3 (off unless switched on; default 1 testing / 0 competition; separate flight recorder switch, on by default), website control panel (driving, speeds, rates, frame lab, profiles), TCAN1044AV transceivers, Orin Nano, Tang Nano 20K FPGA, physical + wireless hardware E-stop.

## Summary

The rover uses **one classic CAN 2.0A bus at 500 kbps** with 11-bit identifiers, 8-byte frames and a **50 Hz control cycle**. 500 kbps is the fastest speed guaranteed to work with the RPi's MCP2515 whatever crystal its board turns out to have; if the crystal is 16 MHz, the whole bus can later move to 1 Mbps with no other change. Today's traffic (six drive nodes, the FPGA, both hosts) uses about **34% of the bus** in the worst case, and about 47% with the planned arm and science nodes. Either host can lead, but **the operator always chooses which one**: nothing drives until a leader is picked, and the RPi can take the lead from the Orin at any time.

Topology (diagram in the live doc): Orin Nano and RPi, linked by Ethernet, plus a service port, all at the electronics-box end (120 Ω) of one CAN 2.0A bus at 500 kbps. Along the bus: drive nodes 4–9 (STM32 + TCAN1044AV), FPGA (Tang Nano 20K, node 20), ESP32-S3 nodes 16–19, and planned arm nodes 10–15 and power board (node 21), with 120 Ω at the far end. Either host can lead; the operator chooses which.

Both hosts sit at the terminated end of the bus; every node hangs off the same pair of wires, and only the master sends commands.

**Key decisions**

| Topic | Decision | Why |
| --- | --- | --- |
| Protocol | CAN 2.0A, 11-bit IDs, 8-byte payloads | ESP32-S3 and the RPi's MCP2515 can't do CAN FD; 2.0A works on every part |
| Bitrate | 500 kbps, sample point about 80% | An MCP2515 with an 8 MHz crystal (worst case) can't reach 1 Mbps; 500 kbps works with any crystal |
| Control rate | 50 Hz (20 ms cycle); motor PID runs locally at about 1 kHz | Keeps 500 kbps under 50% load with room for more nodes; velocity setpoints don't need more |
| Topology | One linear daisy chain, 120 Ω at each physical end | Standard, simplest to debug |
| Leadership | Operator picks the leading host; either can take over at any time; no automatic default | Team requirement: the RPi must be able to lead even with the Orin present |
| Loss of the leader | Nodes stop on their own within 100 ms; the other host alerts the operator and offers to take over | Matches "ask me"; automatic failover is a config switch, off by default |
| IDs | 3-bit class + 5-bit node + 3-bit sub-type | Priority by class, room for 29 nodes and later additions |
| Message definitions | One DBC file generates code for firmware, hosts and dashboard | Prevents mismatched layouts |
| Host software | ROS 2 inside each host only; each host may run Humble or Jazzy, independently; CI tests both | Hosts talk to each other over CAN and a small UDP gateway link, never over ROS, so mixed distros can't break anything |
| Firmware updates | STM32 CAN bootloader; ESP32-S3 OTA partitions; both over ISO-TP | Reflash any node from either host |
| Debugging | Web dashboard with live controls (driving, speeds, message rates, frames); error codes, black-box logs, incident reports; debug levels 0–3, off unless switched on | Every fault visible and traceable |
| Safety | Physical + wireless hardware E-stop, CAN E-stop, node watchdogs, arm/disarm | Rover stops on any single failure |

**Hardware this doc is designed for**

| Device | CAN controller | Transceiver | Role |
| --- | --- | --- | --- |
| Jetson Orin Nano | Built-in mttcan, classic mode | TCAN1044AV on the 40-pin header | Host; leads when chosen |
| Raspberry Pi | MCP2515 on a SPI HAT; crystal unknown, so designed for the worst case (8 MHz) | SN65HVD230 | Host; leads when chosen |
| STM32 nodes | Exact part unknown: bxCAN (F0/F1/F4) or FDCAN in classic mode (G0/G4/H7); this doc covers both | TCAN1044AV | Drive and other actuator nodes |
| ESP32-S3 nodes | Built-in TWAI; flash size unknown, so designed for 4 MB | TCAN1044AV | Sensor / science / auxiliary nodes |
| FPGA (Tang Nano 20K) | CAN controller core in the FPGA fabric | TCAN1044AV | Sensor node |

More nodes will be added later; the ID plan and bus-load budget leave room for them (sections 5 and 7).

## CAN 2.0A in brief, and why it shapes this design

Five behaviours of CAN decide most of the rules in this document: dominant/recessive signalling, ID-based arbitration, acknowledgement, bit stuffing and error confinement.

**1. Dominant and recessive bits**

CAN is a two-wire differential bus. A **dominant** bit (logic 0) is driven: CANH goes up to about 3.5 V and CANL down to about 1.5 V. A **recessive** bit (logic 1) is not driven: the termination resistors pull both wires to about 2.5 V. If any node sends dominant while others send recessive, the bus reads dominant. This "wired-AND" behaviour is what makes arbitration work.

**2. Arbitration: lower ID wins**

A classic 2.0A frame looks like this:

| Field | Bits | Purpose |
| --- | --- | --- |
| Start of frame | 1 | Dominant bit that syncs all nodes |
| Identifier | 11 | Message ID, also its priority |
| RTR, IDE, r0 | 3 | Frame type flags (always data frames, standard IDs here) |
| DLC | 4 | Payload length, 0–8 bytes |
| Data | 0–64 | Payload |
| CRC + delimiter | 16 | 15-bit CRC checked by every receiver |
| ACK slot + delimiter | 2 | Receivers acknowledge here |
| End of frame + interframe space | 10 | Recessive gap before the next frame |

When two nodes start at the same moment, both send their ID bit by bit and read the bus back. A node that sends recessive but reads dominant has lost; it stops at once and retries automatically after the frame. The frame with the **lowest ID always wins**, without being damaged and without delay. That's why the ID scheme in section 5 puts E-stop at 0x000 and telemetry at high IDs.

The flip side: arbitration only separates **different IDs**. If two nodes send the **same ID with different data**, both get to the data field, see mismatched bits, and flag errors. Rule for this design: **every ID has exactly one sender** at any time. The only exception is E-stop, whose payload is fixed so simultaneous copies are bit-identical.

**3. Acknowledgement needs a second node**

The sender transmits the ACK slot recessive; any node that received the frame correctly drives it dominant. If nobody acknowledges, the sender counts an error and retries forever. **A single node alone on a bench will never see its frames succeed.** Always test with at least two nodes (a USB CAN adapter counts).

**4. Bit stuffing makes frames longer than they look**

After five identical bits in a row, the sender inserts one opposite bit so receivers stay in sync. In the worst case this adds about 24 bits to an 8-byte frame. An 8-byte frame is therefore **111 bits without stuffing and up to 135 bits with it** (including the 3-bit interframe gap). The bus-load maths in section 7 always uses the worst case of 135 bits.

**5. Error confinement and bus-off**

Every controller keeps two counters: transmit errors (TEC) and receive errors (REC). Errors raise them by 8 (TEC) or 1 (REC); successful frames lower them by 1.

| State | Condition | Behaviour |
| --- | --- | --- |
| Error-active | TEC and REC below 128 | Normal; signals errors with a dominant error frame that aborts the bad frame for everyone |
| Error-passive | TEC or REC 128 or more | Still communicates, but signals errors quietly and waits longer between its own frames |
| Bus-off | TEC above 255 | Disconnects itself completely; must wait 128 × 11 recessive bits before rejoining |

This is how CAN stops a faulty node from jamming the bus. For us it means:

- Each node reports TEC, REC and bus-off count, and the dashboard shows them. Rising counters warn of wiring or noise problems long before frames are lost.
- Each controller is set to **recover from bus-off automatically**, and each node counts recoveries.
- A node that loses communication must make itself safe on its own, because it can't be told to stop. That's what the command watchdog is for (section 9).

## Physical layer: wiring, termination and transceivers

One straight daisy-chained cable runs from the electronics box past every node, with a 120 Ω resistor at each of the two physical ends and nowhere else. Even at 500 kbps, signal reflections from bad termination or long branches are the most common cause of mysterious errors, so most rules here are about keeping the line clean.

**Bus order**

Orin (terminated end) → RPi → service port → drive nodes 1–6 → FPGA → ESP32-S3 / other nodes → last node (terminated end).

The hosts sit at one end because they're in the same box. The order of the nodes along the cable doesn't matter electrically; choose whatever gives the shortest total cable.

**Cable and connectors**

- Use **twisted pair** for CANH and CANL (any 120 Ω twisted pair, or a pair from Cat5e cable). Twisting makes noise couple equally into both wires, and the receiver only looks at the difference.
- Run a **ground wire** in the same cable. Transceivers need a shared reference; without it the two ends can drift apart (see Grounding below).
- Every node has **two connectors**, in and out, joined on the PCB. The "stub" from the bus to the transceiver is then a few centimetres of trace. Keep any unavoidable branch under **30 cm** (comfortable at 500 kbps, and still fine if we move to 1 Mbps).
- Use **locking connectors** (JST-GH, Molex Micro-Fit, or Deutsch DT for exposed runs). Vibration loosens friction-fit Dupont headers and screw terminals.
- Suggested 4-pin pinout, used by many robotics projects: 1 = power (optional), 2 = CANH, 3 = CANL, 4 = GND. Keep the same pinout on every board.
- Add a **service port** near the hosts: a short branch with a connector where a laptop USB-CAN adapter can be plugged in for debugging or rescue flashing.

**Termination**

A CAN cable has a characteristic impedance of about 120 Ω. A resistor of the same value at each end absorbs the signal instead of reflecting it, and also sets the recessive voltage.

- Fit **exactly two** 120 Ω resistors, one at each physical end. The transceiver at the hosts' end (the Orin's) and the last node carry them.
- Remove or disable all others. The RPi HAT has a 120 Ω jumper: take it **off**, because the RPi is not at the end. Check every TCAN board and SN65 breakout for an on-board 120 Ω and remove it if that board isn't at an end (the v1 doc already planned this).
- **Check with a multimeter**, everything powered off, between CANH and CANL anywhere on the bus:

| Reading | Meaning |
| --- | --- |
| About 60 Ω | Correct: two 120 Ω in parallel |
| About 120 Ω | One terminator missing or a broken cable |
| About 40 Ω | Three terminators fitted; remove one |
| Below 40 Ω | More extra terminators, or a short |

- Optional upgrade: **split termination**, two 60 Ω resistors in series with 4.7 nF from their midpoint to ground. It filters common-mode noise from the motors better than a single 120 Ω.

**Transceivers: TCAN1044AV on the nodes and Orin, SN65HVD230 on the RPi**

All ISO 11898-2 transceivers interoperate, so the TCAN1044AV and the SN65HVD230 work together on one bus. Both are comfortably fast enough: the SN65HVD230 is rated to 1 Mbps and the TCAN1044AV well beyond that.

The TCAN1044AV has two supply pins and a mode pin, and each must be wired correctly or the node will look dead:

| Pin | Connect to | Why |
| --- | --- | --- |
| VCC | 5 V | Powers the CAN bus driver; the part needs 5 V here even on a 3.3 V board |
| VIO | 3.3 V (the MCU's supply) | Sets the logic level of TXD, RXD and STB. At 3.3 V, RXD never exceeds 3.3 V, which protects the ESP32-S3, the Orin Nano and the Tang Nano 20K, none of which tolerate 5 V |
| STB | GND, or an MCU pin driven low | Low = normal mode. High = standby: the transceiver stops transmitting and only listens for a wake-up, so a floating or high STB makes the node appear dead on the bus |
| TXD / RXD | MCU CAN TX / CAN RX | TX to TXD, RX to RXD (a classic mistake is crossing them) |

If a board needs software control of standby, drive STB from a GPIO that defaults low at reset (add a pull-down). Otherwise tie it to GND.

For the RPi's **SN65HVD230**, check the **Rs pin** (pin 8). Tied straight to GND gives high-speed mode; a 10–100 kΩ resistor to GND gives slope control (slower edges, fine at 500 kbps but limits a later move to 1 Mbps); pulled high puts it in standby, where it receives but never transmits.

**Grounding**

Large motor currents flowing through ground wires create voltage differences of a volt or more between boards. Transceivers tolerate a limited common-mode range (roughly −2 V to +7 V for the SN65HVD230), and beyond it frames corrupt.

- Run the CAN signal ground in the CAN cable, from board to board. Never rely on the motor power return or the chassis as the CAN reference.
- Join logic ground and motor ground at **one** point on each board, close to the power input.
- If the RPi HAT is an **isolated** type (the Waveshare 2-CH CAN HAT is), its CAN side has its own ground: connect its GND terminal to the bus ground wire, or the isolation leaves it floating.

**Noise protection**

- Route CAN cables away from motor phase wires, and cross them at right angles where unavoidable.
- Fit a TVS diode for CAN (e.g. PESD2CAN or SM24CANB) at each board's connector, if the boards don't already have one.
- On the noisiest motor boards, a common-mode choke in the CAN lines helps.
- If the cable is shielded, connect the shield to ground at **one end only**, to avoid ground loops.

## Controller setup per device

Every device runs at exactly **500 kbps** with a sample point close to 80%, clocked from a crystal. This section gives working settings for each controller, the hardware filters that keep CPUs from drowning in traffic, how we handle the RPi's MCP2515 with an unknown crystal, and how to move the whole bus to 1 Mbps later if we want more headroom.

**How CAN bit timing works**

Each bit is divided into equal slices called **time quanta** (TQ), produced by dividing the controller's clock by a prescaler. A bit has three parts:

| Segment | Length | Purpose |
| --- | --- | --- |
| Sync | Always 1 TQ | Where an edge is expected |
| Segment 1 (propagation + phase 1) | Several TQ | Covers signal travel time along the cable |
| Segment 2 (phase 2) | Several TQ | Margin after the sample |

The bit is **sampled** at the end of segment 1. The sample point is (1 + seg1) ÷ (1 + seg1 + seg2). The **SJW** (synchronisation jump width) is how many TQ a node may stretch or shrink a bit to stay in step with the other nodes' clocks.

Rules for our bus:

- Bitrate exactly 500 kbps on every device.
- Sample point between 75% and 81% on every device. On a short rover cable this range is forgiving.
- **Use a crystal**, never an internal RC oscillator. CAN needs clock accuracy of roughly ±0.5% or better; STM32 internal HSI oscillators are only specified to about ±1%. Use HSE on every STM32.
- The bitrate is one constant in the shared firmware library and one line in each host's network config, so changing it later (to 1 Mbps) touches nothing else.

**STM32 nodes**

We don't know the exact STM32 parts yet, so the shared library supports both CAN peripherals ST uses: **bxCAN** (F0, F1, F4 families) and **FDCAN** (G0, G4, H7 families, used here in classic mode only). Find the part number on the chip and pick its row. The last column is the prescaler to use if the bus later moves to 1 Mbps (everything else stays the same).

| Family (example) | Peripheral | CAN clock | Prescaler | Seg1 (BS1) | Seg2 (BS2) | TQ per bit | Sample point | Prescaler for 1 Mbps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| F103 | bxCAN | 36 MHz (APB1) | 4 | 13 | 4 | 18 | 77.8% | 2 |
| F407 / F405 | bxCAN | 42 MHz (APB1) | 4 | 16 | 4 | 21 | 81.0% | 2 |
| F446 | bxCAN | 45 MHz (APB1) | 6 | 11 | 3 | 15 | 80.0% | 3 |
| G431 / G474 | FDCAN, classic mode | 160 MHz | 16 | 15 | 4 | 20 | 80.0% | 8 |
| G0B1 | FDCAN, classic mode | 64 MHz | 8 | 12 | 3 | 16 | 81.3% | 4 |

Set SJW to 1 TQ on bxCAN and 1–3 TQ on FDCAN. If the CAN clock in CubeMX's clock tree differs from the table, recompute: CAN clock (MHz) ÷ prescaler ÷ TQ-per-bit must equal 0.5.

Other STM32 settings:

- **bxCAN**: turn on ABOM (automatic bus-off management) so the node rejoins by itself; turn on TXFP so transmit mailboxes go out in request order (multi-frame bootloader transfers share one ID and must never be reordered; priority between messages is handled in the software transmit queue instead); leave NART off so automatic retransmission stays on.
- **FDCAN in classic mode**: set the frame format to classic and bit-rate switching off. Leave automatic retransmission on. FDCAN does not leave bus-off by itself: on the bus-off interrupt, clear the INIT bit to start recovery.

**ESP32-S3 nodes**

The ESP32-S3 has one TWAI controller (Espressif's name for CAN 2.0), clocked at 80 MHz. ESP-IDF's ready-made 500 kbps timing is prescaler 8, seg1 15, seg2 4, SJW 3: 20 TQ per bit and an 80% sample point. For 1 Mbps later, the prescaler becomes 4.

Things specific to the ESP32-S3:

- After bus-off the driver **does not restart by itself**. The shared library watches for the bus-off alert, starts recovery, and restarts the controller when recovery completes.
- Size the receive queue for bursts (32 frames is plenty) and read it from a dedicated high-priority task pinned to core 1, leaving Wi-Fi on core 0.
- TWAI's acceptance mask uses **1 = don't care**, the opposite of STM32 bxCAN: a common cause of "I receive nothing" bugs.

**Tang Nano 20K (FPGA sensor node)**

The FPGA needs a CAN controller implemented in its logic. Use an existing open-source core rather than writing one: for example an SJA1000-compatible Verilog core (such as the OpenCores CAN core) or the CTU CAN FD core in classic mode. Clock it from the board's crystal, set 500 kbps with an 80% sample point, and wire its TX/RX to a TCAN1044AV with VIO at 3.3 V. The node behaves like any other: it sends a heartbeat, its sensor frames and NODE\_EVENT reports, and stops sending if it goes bus-off.

**Hardware filters on nodes (same recipe on STM32, ESP32-S3 and FPGA)**

Thanks to the ID layout in section 5, two mask filters give each node exactly what it needs:

| Filter | Matches | Gets the node |
| --- | --- | --- |
| A: node field = my node ID (mask 0x0F8) | Any class addressed to me | My commands, parameter requests, bootloader traffic |
| B: class = 0 (mask 0x700) | IDs 0x000–0x0FF | E-stop, SYNC, network commands, host heartbeats, lead requests |

Everything else (other nodes' feedback, telemetry, heartbeats) is dropped in hardware without ever interrupting the CPU. On bxCAN this is two filter banks in mask mode; on FDCAN two standard filter elements; on the ESP32-S3 TWAI "dual filter" mode.

**Jetson Orin Nano**

The Orin Nano's CAN controller (mttcan) is built into its processor and buffers frames in hardware, so it copes easily with the full traffic. It is exposed on the 40-pin header and needs:

1. A **TCAN1044AV** wired to the header's CAN TX and CAN RX pins (see NVIDIA's pinout for the exact pins), with VIO at 3.3 V, VCC at 5 V and STB to GND.
2. The header pins switched to their CAN function (pinmux), using the Jetson-IO tool or the register settings in NVIDIA's CAN guide.
3. Kernel modules loaded: `can`, `can_raw`, `mttcan`.
4. The interface brought up in classic mode: `ip link set can0 type can bitrate 500000 sample-point 0.8 restart-ms 100 berr-reporting on`, then `ip link set can0 up`.

The Orin Nano has one CAN controller, so one bus. If we ever need a second bus, it would go on a USB-CAN adapter.

**Raspberry Pi with MCP2515 + SN65HVD230**

The MCP2515 is a separate CAN controller chip that talks to the RPi over SPI. Two things about it shape our design.

*1. Its crystal sets the fastest possible bitrate.* Its shortest time quantum is 2 ÷ (crystal frequency), and a bit needs at least 8 TQ for reliable timing. We don't know the crystal yet, so we designed for the worst common case, 8 MHz:

| HAT crystal | Shortest TQ | TQ per bit at 500 kbps | 500 kbps? | 1 Mbps possible? |
| --- | --- | --- | --- | --- |
| 8 MHz | 250 ns | 8 | Yes (sample point 75%) | No |
| 12 MHz | 167 ns | 12 | Yes (75% or 83%) | Marginal; avoid |
| 16 MHz | 125 ns | 16 | Yes (81%) | Yes |

So **500 kbps works whatever the crystal is**. The one thing that must still be right is the oscillator value told to Linux, because Linux calculates all the timing from it; a wrong value gives a wrong bitrate and nothing but error frames. To find the crystal: read the marking on the small metal can next to the MCP2515 (for example "8.000" or "16.000"). Then set it in `/boot/firmware/config.txt`:

`dtoverlay=mcp2515-can0,oscillator=8000000,interrupt=25,spimaxfrequency=10000000` (replace 8000000 with the real value)

and bring the interface up with `bitrate 500000 sample-point 0.75`. Check the result with `ip -details link show can0`, which prints the clock and the bit timing actually used.

*2. It holds only two received frames.* The RPi must read each frame over SPI before the next two arrive. At 500 kbps and a 50 Hz cycle our bus carries about 1,300 frames per second, one every 770 µs on average, which an RPi handles comfortably when the CAN interrupt thread has real-time priority. Frame losses show up as **overruns**:

- Run SPI at 10 MHz (`spimaxfrequency` above) and give the CAN interrupt thread real-time priority.
- Watch the counter with `ip -details -statistics link show can0`; the gateway raises HOST\_RX\_OVERRUN and the dashboard shows it.
- Test this especially with the **RPi leading**, since the leader must not miss feedback.
- If overruns ever appear, lower non-essential rates first (section 7). The fallback, without touching any node, is a USB-CAN adapter on the RPi (candleLight / gs\_usb firmware), which buffers frames itself.

**Common host settings (Orin Nano and RPi)**

- Bring `can0` up automatically at boot with a systemd-networkd `.network` file holding the bitrate, `restart-ms 100` (automatic bus-off recovery) and `berr-reporting on` (error frames and counters visible to software).
- Raise the transmit queue with `ip link set can0 txqueuelen 1000`, so bursts of commands are never dropped by the kernel.

**Moving to 1 Mbps later (optional)**

If the bus gets crowded as nodes are added, 1 Mbps halves the load (about 24% instead of 47% with all planned nodes). It needs:

1. The RPi's MCP2515 crystal confirmed as 16 MHz, or the crystal swapped for a 16 MHz one (a small soldering job) and the `oscillator` value updated.
2. The SN65HVD230's Rs pin tied straight to GND.
3. Every node reflashed with the 1 Mbps prescaler (table above), and both hosts' network files changed, all at once. A node at the wrong bitrate fills the bus with error frames.
4. A repeat of the bus tests in section 16.

## Identifier scheme

The 11-bit ID is split into **3 bits of class, 5 bits of node ID and 3 bits of sub-type**. Class comes first so it sets priority; node ID is in the middle so two simple hardware filters work; sub-type picks the message within a class.

```
bit:   10  9  8 | 7  6  5  4  3 | 2  1  0
       class    | node ID       | sub-type

ID = (class << 8) | (node << 3) | sub
```

**Why this layout**

- **Priority follows class.** Lower IDs win arbitration (section 2), so everything in class 0 beats everything in class 1, and so on. Safety-critical messages get low classes; bulk data gets high ones.
- **5 node bits = 32 node IDs.** The v1 layout had 4 node bits (16 IDs, with 0 and 15 reserved), which a rover with drive, arm, science and power boards can outgrow quickly.
- **Node ID in the middle** lets a node accept "anything addressed to me" with one mask (section 4).
- **3 sub-type bits** give 8 messages per node per class, which is enough because each class has a narrow purpose.

**Classes**

| Class | ID range | Purpose | Who sends |
| --- | --- | --- | --- |
| 0 | 0x000–0x0FF | E-stop, SYNC, network management, host heartbeats | Hosts (E-stop: anyone) |
| 1 | 0x100–0x1FF | Commands to nodes | Master host only |
| 2 | 0x200–0x2FF | Fast feedback (speed, current, encoders, faults) | Nodes |
| 3 | 0x300–0x3FF | Slow telemetry (voltage, temperature, error counters) | Nodes |
| 4 | 0x400–0x4FF | Sensor streams (FPGA, IMU, science) | Sensor nodes |
| 5 | 0x500–0x5FF | Service: parameters, node info | Hosts and nodes |
| 6 | 0x600–0x6FF | Node heartbeats | Every node |
| 7 | 0x700–0x7FF | Bootloader and firmware transfer | Master and the node being flashed |

**Node IDs**

| Node ID | Assignment |
| --- | --- |
| 0 | Broadcast ("all nodes") |
| 1 | Jetson Orin |
| 2 | Raspberry Pi |
| 3 | Service tool (laptop on the service port) |
| 4–9 | Drive nodes 1–6 (STM32) |
| 10–15 | Arm nodes (reserved) |
| 16–19 | Science / auxiliary nodes (ESP32-S3 or STM32) |
| 20 | FPGA sensor node |
| 21 | Power distribution / battery monitor (reserved) |
| 22–30 | Spare |
| 31 | Unconfigured board (new board, or ID switch unreadable) |

Nodes read their ID from a DIP switch or solder jumpers at boot, so every drive board runs the **same firmware file**, and a spare board can replace any drive node by setting its switches.

**Class 0 allocations**

Class 0 is special: it carries network-wide messages, so the node field is used as a message number rather than a destination.

| ID | Name | Sender | Meaning |
| --- | --- | --- | --- |
| 0x000 | ESTOP | Anyone | Stop everything now. Fixed payload so simultaneous senders don't collide. |
| 0x001 | ESTOP\_RELEASE | Master | Clear the E-stop latch, after operator action |
| 0x002 | SYNC | Master | Start of each 20 ms control cycle (section 9) |
| 0x003 | NMT | Master | Network management: ARM, DISARM, reset node, enter bootloader, fault reset (target node in payload) |
| 0x008 | HOST\_HB\_ORIN | Orin | Host heartbeat, 20 Hz |
| 0x010 | HOST\_HB\_RPI | RPi | Host heartbeat, 20 Hz |
| 0x018 | HOST\_HB\_TOOL | Service tool | Optional |

Class 0 also carries **LEAD\_REQUEST** frames, sub-type 1 of each host's heartbeat slot: **0x009** sent by the Orin, **0x011** by the RPi, **0x019** by the service tool. Each carries the node ID of the host the operator wants to lead (section 8). Giving every sender its own ID keeps the one-sender-per-ID rule even if two people press buttons at once.

**0x004 DEBUG\_LEVEL** (master only) sets the debug level of one node or all nodes; see the debug levels section.

**Service and bootloader sub-types**

These classes have more than one possible sender, so the sub-type identifies the sender, which keeps the "one sender per ID" rule even when both hosts talk to the same node:

| Class | Sub 0 | Sub 1 | Sub 2 | Sub 4 | Sub 5 | Sub 6 |
| --- | --- | --- | --- | --- | --- | --- |
| 5 (service) | Request from Orin | Request from RPi | Request from tool | Response to Orin | Response to RPi | Response to tool |
| 7 (bootloader) | Host → node | Node → host |  |  |  |  |

**Worked example: drive node 1 (node ID 4)**

| Message | Class | Node | Sub | ID |
| --- | --- | --- | --- | --- |
| Command | 1 | 4 | 0 | 0x120 |
| Feedback A (speed, current) | 2 | 4 | 0 | 0x220 |
| Feedback B (encoders) | 2 | 4 | 1 | 0x221 |
| Telemetry page 0 | 3 | 4 | 0 | 0x320 |
| Telemetry page 1 | 3 | 4 | 1 | 0x321 |
| Parameter request from Orin | 5 | 4 | 0 | 0x520 |
| Parameter response to Orin | 5 | 4 | 4 | 0x524 |
| Heartbeat | 6 | 4 | 0 | 0x620 |
| Bootloader host → node | 7 | 4 | 0 | 0x720 |
| Bootloader node → host | 7 | 4 | 1 | 0x721 |

Drive node 2 (ID 5) uses the same messages at 0x128, 0x228 and so on: add 8 for each node.

**Automatic node IDs**

Boards without an ID switch (the ESP32 bench nodes, flashed with identical firmware) get their ID from a host instead:

| ID | Name | Sender | Payload |
| --- | --- | --- | --- |
| 0x5FB | NODE\_ID\_REQUEST | Unconfigured board | MAC (48 bit, the 6 MAC bytes as one integer, sent least-significant byte first), node type, HW target (0 ESP32, 1 ESP32-S3) |
| 0x5F8 / 0x5F9 / 0x5FA | NODE\_ID\_ASSIGN from Orin / RPi / tool | Allocator host | MAC, assigned ID, flags |

These are the unused sub-types of node 31's service slot, so no other ID moves.

1. A board with no ID saved in flash stays silent for a random 100–600 ms (seeded from its MAC), then sends NODE\_ID\_REQUEST about every 500 ms with jitter. It sends no heartbeat until it has an ID, so ID 31 is never shared by two heartbeats.
2. The **allocator** answers: the master, or when no master is alive the live host with the lowest tie-break. Exactly one host answers, so a lone RPi on the bench addresses boards before anyone picks a leader. An ID is an address, not a command, so this doesn't touch the master-only rules for actuation.
3. The allocator keeps a MAC → ID table on disk (`node_id_table` in the gateway config). The same board always gets the same ID back. New boards get the lowest free ID from 16–19, then 22–30, skipping the FPGA (20), the power node (21) and any ID already heard on the bus.
4. The board checks the MAC, saves the ID and starts as a normal node. Two boards asking at the same instant collide; the jitter separates them, and a few error frames during allocation are expected.

Boards using automatic IDs send the shared NODE\_HB layout at 0x600 + 8 × ID, and the generic 4 × u16 sensor layout at 0x400/0x401 + 8 × ID. The gateway decodes both for any node 16–30.

**Bench text messages**

Short text for bench tests and debugging, on the lowest-priority class so it can never delay control traffic:

| ID | Sender | Payload |
| --- | --- | --- |
| 0x700 + 8 × node + 2 / 3 / 4 | Orin / RPi / tool, to one node (node 0 = every node) | SEQ (1–255), up to 7 ASCII characters |
| 0x700 + 8 × node + 5 | The node | SEQ of the host text it is echoing, or 0 for its own text; up to 7 characters |

A node echoes every text it receives, so the sender knows it arrived. The DBC holds the broadcast copies (0x702–0x704) and a TEXT\_FROM\_NODE template; every node uses the same layouts at its own IDs. Sub-types 0 and 1 of class 7 stay reserved for firmware transfer. Text is never a command: it changes nothing on the node.

## Message catalog

Every frame on the bus is listed here with its exact layout. All multi-byte fields are **little-endian** (native on STM32, ESP32, Orin and RPi, so no byte swapping), and values are **scaled integers**, not floats, so 8 bytes hold as much as possible. The DBC file (end of this section) is the authoritative copy; this section explains it.

**Overview**

| ID (drive node 1 shown) | Name | Length | Rate | Sender → receiver |
| --- | --- | --- | --- | --- |
| 0x000 | ESTOP | 1 B | Event | Anyone → all |
| 0x001 | ESTOP\_RELEASE | 2 B | Event | Master → all |
| 0x002 | SYNC | 1 B | 50 Hz | Master → all |
| 0x003 | NMT | 4 B | Event | Master → one or all nodes |
| 0x004 | DEBUG\_LEVEL | 7 B | Event | Master → one or all nodes |
| 0x008 / 0x010 | HOST\_HB | 8 B | 20 Hz | Each host → all |
| 0x009 / 0x011 / 0x019 | LEAD\_REQUEST | 2 B | Operator action | Orin / RPi / service tool → hosts |
| 0x120 | DRIVE\_CMD | 8 B | 50 Hz | Master → drive node |
| 0x220 | DRIVE\_FB\_A | 8 B | 50 Hz | Drive node → hosts |
| 0x221 | DRIVE\_FB\_B | 8 B | 50 Hz | Drive node → hosts |
| 0x320 / 0x321 | DRIVE\_TELEM\_0 / \_1 | 8 B | 10 Hz (1 Hz at debug level 0) | Drive node → hosts |
| 0x322 / 0x323 | DRIVE\_DEBUG\_A / \_B | 8 B | Only at debug level 2 (10 Hz) or 3 (50 Hz) | Drive node → hosts |
| 0x327 | NODE\_EVENT | 8 B | Event | Any node → hosts |
| 0x4A0 / 0x4A1 | FPGA\_SENSORS\_A / \_B | 8 B | 50 Hz | FPGA → hosts |
| 0x520–0x526 | SERVICE | 8 B | On demand | Host ↔ node |
| 0x620 | NODE\_HB | 8 B | 10 Hz | Every node → hosts |
| 0x720 / 0x721 | BOOT | 8 B | During flashing | Master ↔ node |

**ESTOP (0x000, 1 byte)**: byte 0 is always 0xE5. Every sender uses the identical payload, so if the RPi, the Orin and the power board all send E-stop at the same instant, their frames are bit-for-bit the same and merge into one successful frame instead of colliding.

**ESTOP\_RELEASE (0x001, 2 bytes)**: byte 0 master node ID, byte 1 master term (low byte).

**SYNC (0x002, 1 byte)**: byte 0 cycle counter, wraps at 255. Nodes use it to timestamp and to detect missed cycles.

**NMT (0x003, 4 bytes)**

| Byte | Field |
| --- | --- |
| 0 | Target node ID (0 = all nodes) |
| 1 | Command: 1 ARM, 2 DISARM, 3 RESET, 4 ENTER\_BOOTLOADER, 5 FAULT\_RESET |
| 2 | Master node ID |
| 3 | Master term (low byte) |

**HOST\_HB (0x008 Orin, 0x010 RPi; 8 bytes, 20 Hz)**: see section 8 for how these drive the master election.

| Byte | Field |
| --- | --- |
| 0 | Role: 0 booting, 1 standby, 2 master, 3 observer |
| 1 | Tie-break number (lower wins if two hosts claim the lead at the same instant) |
| 2–3 | Term (u16), increases every time a new master takes over |
| 4 | Node ID this host believes is master |
| 5 | Flags: bit 0 autonomy ready, bit 1 operator link up, bit 2 E-stop latched, bit 3 rover armed |
| 6–7 | Uptime (u16, seconds) |

**LEAD\_REQUEST (0x009 / 0x011 / 0x019; 2 bytes)**: byte 0 = node ID of the host that should lead (1 Orin, 2 RPi), byte 1 = request counter (so a repeated button press is recognised as new). Sent when the operator picks or changes the leader; section 8 explains what the hosts do with it.

**DEBUG\_LEVEL (0x004, 7 bytes, master only)** and **DRIVE\_DEBUG\_A / \_B (class 3, sub 2 / 3)**: switch debugging on and off and carry PID internals while it is on. Layouts are in the debug levels section.

**DRIVE\_CMD (class 1, sub 0; 8 bytes, 50 Hz)**

| Byte | Field | Type and scale |
| --- | --- | --- |
| 0 | bits 0–3 sequence counter; bits 4–5 sender host (1 Orin, 2 RPi, 3 tool); bits 6–7 term (low 2 bits) | Packed |
| 1 | Mode: 0 coast, 1 brake, 2 velocity, 3 open-loop duty | u8 enum |
| 2–3 | Target channel A | i16, 0.01 rad/s (±327 rad/s), or 0.01% duty in mode 3 |
| 4–5 | Target channel B | Same as A |
| 6–7 | Acceleration limit | u16, 0.01 rad/s² |

Nodes store a new command when it arrives but only **apply** it at the next SYNC, so all six wheels change speed in the same instant even though their commands arrive one after another.

**DRIVE\_FB\_A (class 2, sub 0; 8 bytes, 50 Hz)**

| Byte | Field | Type and scale |
| --- | --- | --- |
| 0 | Node state (section 9) | u8 enum |
| 1 | Fault flags: bit 0 overcurrent A, 1 overcurrent B, 2 over-temperature, 3 under-voltage, 4 encoder A fault, 5 encoder B fault, 6 command timeout, 7 E-stop latched | Bitfield |
| 2–3 | Measured speed A | i16, 0.01 rad/s |
| 4–5 | Measured speed B | i16, 0.01 rad/s |
| 6 | Current A | u8, 0.2 A (0–51 A) |
| 7 | Current B | u8, 0.2 A |

**DRIVE\_FB\_B (class 2, sub 1; 8 bytes, 50 Hz)**: bytes 0–3 encoder count A (i32), bytes 4–7 encoder count B (i32). Raw counts, so the host can do odometry with no rounding; counts per revolution is a node parameter.

**DRIVE\_TELEM\_0 (class 3, sub 0; 8 bytes, 10 Hz)**

| Byte | Field | Type and scale |
| --- | --- | --- |
| 0–1 | Supply voltage | u16, mV |
| 2 / 3 | Driver temperature A / B | i8, °C |
| 4 | MCU temperature | i8, °C |
| 5 / 6 | TEC / REC (CAN error counters) | u8 each |
| 7 | Bus-off recovery count since boot | u8, saturates at 255 |

**DRIVE\_TELEM\_1 (class 3, sub 1; 8 bytes, 10 Hz)**: bytes 0–1 / 2–3 output duty A / B (i16, 0.01%); byte 4 watchdog reset count; byte 5 last reset cause (power-on, brown-out, watchdog, software, pin); bytes 6–7 commands missed since boot (u16).

**FPGA\_SENSORS\_A / \_B (0x4A0 / 0x4A1; 8 bytes, 50 Hz)**: four u16 sensor values each, sensors 1–4 and 5–8, as in v1. Scaling per sensor is defined in the DBC.

**NODE\_HB (class 6, sub 0; 8 bytes, 10 Hz, every node including ESP32-S3s and the FPGA)**

| Byte | Field |
| --- | --- |
| 0 | Node state |
| 1 | Node type (1 drive, 2 arm, 3 science, 4 FPGA, 5 power, ...) |
| 2 | Protocol version (from the DBC) |
| 3 | Flags: bits 0–1 CAN error state (active / passive / recovered from bus-off), bit 2 running bootloader, bit 3 unsaved parameter changes, bits 4–5 current debug level |
| 4–5 | Firmware build ID (low 16 bits of the git commit hash) |
| 6–7 | Uptime (u16, seconds) |

The heartbeat lets the host build a live list of every node: alive or missing, what firmware it runs, whether it rebooted (uptime dropped) and whether its CAN link is healthy.

**NODE\_EVENT (class 3, sub 7; 8 bytes; sent when an event happens, e.g. 0x327 for drive node 1)**: severity, 16-bit error code, event sequence number, detail value and node uptime. Layout, error codes and rate limits are in the error reporting section.

**SERVICE (class 5; 8 bytes)**: requests and responses share one layout.

| Byte | Request | Response |
| --- | --- | --- |
| 0 | Opcode: 1 GET, 2 SET, 3 SAVE, 4 RESTORE\_DEFAULTS, 5 INFO, 6 LOG\_READ, 7 LOG\_CLEAR | Low 4 bits opcode; high 4 bits status (0 OK, 1 bad index, 2 out of range, 3 not allowed while armed, 4 flash error) |
| 1 | Transaction ID (chosen by the host) | Same transaction ID |
| 2–3 | Parameter index (u16) | Same index |
| 4–7 | Value for SET (u32, i32 or f32 depending on the parameter) | Current value |

INFO uses the index field to page through fixed information: 0 full git hash (two responses), 2 firmware version, 3 MCU unique ID, 4 build date.

**BOOT (class 7)**: multi-frame transfers using ISO-TP; the commands are described in section 10.

**ESP32-S3 and other node types** follow the same pattern: commands in class 1, fast data in class 2 or 4, slow data in class 3, a heartbeat in class 6. Their layouts are added to the DBC as each node is designed.

**The DBC file is the single source of truth**

- Keep `rover.dbc` in a small shared git repo (e.g. `anveshak-can-defs`), used as a submodule by every firmware and host project.
- **Firmware**: generate C pack/unpack functions with `cantools generate_c_source rover.dbc`. Nobody hand-writes byte shuffling.
- **Hosts**: decode in Python with `cantools`, or use generated C++ headers in the gateway.
- **Tools**: SavvyCAN, `cantools monitor` and the dashboard load the same file, so raw frames appear as named, scaled signals.
- **Protocol version**: bump it in the DBC on every incompatible change. Nodes report it in their heartbeat, and the master refuses to ARM a node running an older protocol.
- **CI check** on every change: no duplicate IDs; each ID matches the class / node / sub-type rule; every message has one named sender.

## Bus load

At 500 kbps with a 50 Hz control cycle, today's traffic uses about **34% of the bus in the worst case**, and about 47% once the planned arm and science nodes are added. Both are under our 50% ceiling. Going further means trimming rates or moving to 1 Mbps, which halves every number in this section.

**Step 1: how long one frame takes**

A standard-ID frame with *n* data bytes has 47 + 8n bits before stuffing (including the 3-bit gap before the next frame). Bit stuffing can add one bit for every four in the stuffable part (start of frame through CRC, 34 + 8n bits), so the worst case is:

| Payload | Bits, no stuffing | Bits, worst-case stuffing | Time at 500 kbps | Time at 1 Mbps |
| --- | --- | --- | --- | --- |
| 1 byte (SYNC, ESTOP) | 55 | 65 | 130 µs | 65 µs |
| 8 bytes (almost everything) | 111 | 135 | 270 µs | 135 µs |

Real traffic stuffs less than the worst case, so the measured load will usually be a few points lower. We plan with the worst case so the numbers never surprise us.

**Step 2: frames per second × time per frame**

Load = sum over all messages of (frames per second × worst-case bits per frame) ÷ 500,000.

| Traffic | Frames per second | Load at 500 kbps |
| --- | --- | --- |
| SYNC (50 Hz) | 50 | 0.7% |
| Host heartbeats (2 hosts × 20 Hz) | 40 | 1.1% |
| Drive commands (6 nodes × 50 Hz) | 300 | 8.1% |
| Drive feedback A, speed and current (6 × 50 Hz) | 300 | 8.1% |
| Drive feedback B, encoders (6 × 50 Hz) | 300 | 8.1% |
| Drive telemetry (6 nodes × 2 pages × 10 Hz) | 120 | 3.2% |
| FPGA sensors (2 frames × 50 Hz) | 100 | 2.7% |
| Node heartbeats (8 nodes × 10 Hz) | 80 | 2.2% |
| **Total today** | **1,290** | **about 34%** (17% at 1 Mbps) |
| Planned: 6 arm nodes, command + feedback at 25 Hz | 300 | 8.1% |
| Planned: 4 ESP32-S3 / science nodes, 2 frames at 10 Hz | 80 | 2.2% |
| Planned: heartbeats for 10 more nodes | 100 | 2.7% |
| **Total with planned nodes** | **1,770** | **about 47%** (24% at 1 Mbps) |

NODE\_EVENT reports, service traffic and lead requests are occasional and add well under 1%.

**Step 3: why the ceiling is 50%, not 100%**

- **Waiting time for lower priorities.** CAN always serves the lowest ID first. The fuller the bus, the longer high-ID messages (telemetry, service, events) wait. Below 50% they still get through within one cycle.
- **Retransmissions.** A frame hit by noise is resent automatically; on a nearly full bus, a burst of errors pushes other frames into the next cycle.
- **Room for events.** E-stops, parameter changes, event reports and firmware flashing all need space on top of the steady traffic.
- **The RPi's MCP2515** must read every frame in time (section 4); fewer frames per second is easier for it.

**How long the important messages wait**

A frame can't interrupt one already on the wire, so the worst wait for an E-stop is one 8-byte frame (270 µs) plus its own 130 µs: **under 0.5 ms**. SYNC and the six drive commands at the start of each cycle are all on the wire within about 2 ms. Each 20 ms cycle carries about 7 ms of traffic, and the bus is idle the rest of the time.

**What each new node costs**

Use this when adding nodes later:

| New node type | Typical traffic | Load at 500 kbps |
| --- | --- | --- |
| Actuator with two motors (like a drive node) at 50 Hz | Command + 2 feedback frames at 50 Hz, 2 telemetry pages at 10 Hz, heartbeat | About 4.9% |
| Actuator at 25 Hz (arm joint) | Command + 1 feedback frame at 25 Hz, heartbeat | About 1.6% |
| Sensor node | 2 data frames at 10 Hz, heartbeat | About 0.8% |

When the planned total approaches 50%, use the knobs below or move to 1 Mbps (section 4).

**Knobs if the load grows**

| Change | Saves at 500 kbps |
| --- | --- |
| Move the whole bus to 1 Mbps | Half of everything |
| Encoder feedback (DRIVE\_FB\_B) from 50 Hz to 25 Hz | About 4% |
| Drive telemetry from 10 Hz to 5 Hz | About 1.6% |
| FPGA sensors from 50 Hz to 25 Hz | About 1.4% |
| Node heartbeats from 10 Hz to 5 Hz | About 1.1% now, more as nodes are added |
| Pause telemetry during firmware flashing | About 3% while flashing |

**Measure it, don't just trust the maths**

- `canbusload can0@500000 -r -t -b` (from can-utils) shows live load every second.
- The dashboard shows the same figure and, for every ID, the measured rate next to the rate the DBC expects (section 13).

## Hosts: choosing which one leads

Both hosts run identical gateway software and can each drive the rover. **The operator always decides which host leads**: there is no built-in default, nothing moves until a leader is chosen, and the operator can hand the lead to the other host (for example from the Orin to the RPi) at any time. The leading host is called the **master** in the rest of this doc; it is the only host that sends commands. The other host is **standby**: it listens to everything, keeps full rover state, and is ready to lead when asked.

**CAN itself has no master, so why does this?**

CAN lets any node transmit whenever the bus is free. "Master" here is not about the bus, it is about **whose commands the motors obey**:

- If both hosts sent to the same command ID (e.g. 0x120) with different targets, the frames would collide in the data field and keep failing (section 2).
- Even on different IDs, a wheel receiving "2 rad/s" from one host and "0" from the other would jump between them.
- The node's command watchdog only means something if exactly one source is expected.

So commands, SYNC, NMT and E-stop release come only from the master. Either host may always send E-stop, its own heartbeat, lead requests and service requests on its own IDs.

**What happens in each configuration**

| Hosts present | At start-up | Operator action needed |
| --- | --- | --- |
| Orin + RPi | Both standby; nodes disarmed; dashboard asks "Which host should lead?" | Pick Orin or RPi |
| Orin only | Orin standby; dashboard asks to confirm Orin as leader | One click |
| RPi only | RPi standby; dashboard asks to confirm RPi as leader | One click |
| Neither | Nodes stay disarmed (no commands, no SYNC) | Start a host |

**How the operator chooses**

Three equivalent ways, all ending in the same LEAD\_REQUEST frame on the bus:

- The **dashboard** on either host: "Make Orin lead" / "Make RPi lead" buttons, always visible in the header.
- The **ROS 2 service** `/can/request_lead` on either host (for scripts or a base-station GUI).
- The **service tool** on a laptop at the service port.

The request travels as a **LEAD\_REQUEST** frame on CAN (0x009 from the Orin, 0x011 from the RPi, 0x019 from the tool; section 5). Using CAN, not Ethernet, means the choice reaches the other host even if the Ethernet link between the hosts is down.

**Host heartbeat and the claim rules**

Each host sends HOST\_HB at 20 Hz (layout in section 6) with its role and a **term** number. The term goes up by one every time a host takes the lead; everyone trusts the highest term they have seen.

1. **Start as standby.** A host that boots never takes the lead by itself. It sends heartbeats with role = standby and waits.
2. **Claim only when chosen.** A host takes the lead only when a LEAD\_REQUEST names it (or, if the optional auto-takeover switch is on, when the master has vanished; see below).
3. **Claiming** = setting its term to the highest term seen + 1, announcing role = master in its next heartbeat (within 50 ms), then sending SYNC and zero-speed commands.
4. **Higher term wins.** A master that sees another master with a higher term steps down to standby within one heartbeat and stops sending commands. If two hosts claim with the same term at the same instant, the lower tie-break number (Orin = 1, RPi = 2) keeps it; the dashboard shows the result.
5. **Taking over at any time.** If the RPi is chosen while the Orin leads, the RPi claims with a higher term and the Orin steps down by rule 4. No cooperation from the Orin is needed: nodes follow the new term at once and ignore anything the Orin still sends (the dashboard flags it as CMD\_FROM\_NON\_MASTER). If a misbehaving host keeps transmitting on command IDs, stop its gateway.

**What nodes do**

- Every node listens to HOST\_HB and remembers the current master's node ID and term.
- DRIVE\_CMD carries the sender's host number and the low bits of its term. A node ignores commands that don't come from the current master.
- **When the term changes, every node disarms**: outputs go to zero and the node waits for an explicit ARM from the new master. A handover therefore always stops the rover first, and the new leader resumes deliberately. This prevents a half-ready host (autonomy not started, stale targets) from driving the wheels the instant it takes over.

**Timeline: operator hands the lead from the Orin to the RPi while driving**

| Time after the click | What happens |
| --- | --- |
| 0 ms | Operator clicks "Make RPi lead"; a LEAD\_REQUEST goes out on CAN |
| Up to 50 ms | RPi claims: heartbeat with role master and term + 1 |
| About the same moment | Nodes see the new term: outputs to zero, DISARMED. Orin sees a higher term and stops sending commands |
| When the operator is ready | Operator (or autonomy on the RPi) sends ARM; the RPi drives |

**When the leader disappears**

| Time after the master's last frame | What happens |
| --- | --- |
| 0 ms | Last command and heartbeat from the master (say the Orin crashed) |
| 100 ms | Node command watchdogs expire: every motor stops, nodes DISARMED |
| 250 ms | Standby (RPi) has missed 5 heartbeats: raises LEADER\_LOST; dashboard shows "Orin stopped responding. Make RPi lead?" |
| When the operator clicks | RPi claims the lead; operator re-arms |

The rover is stopped within 100 ms in every case. By default nothing restarts it without the operator.

**Optional auto-takeover**

Each host's config has `auto_takeover`, **off by default**. If it is turned on, a standby host claims the lead automatically 250 ms after the master disappears. Nodes still stay disarmed until someone sends ARM, so even then the rover never restarts on its own. This can be useful for long autonomous runs; leave it off while testing.

**Who produces the drive targets**

- Autonomy and teleoperation can run on either host and publish targets on ROS 2.
- The master's gateway takes the latest targets (from its own host, or from the other host over the gateway link (section 12)) and sends them on the bus every 20 ms.
- The standby's gateway decodes all bus traffic too, so both hosts always have live rover state and the dashboard works on both.

**Split brain**

If a fault stops one host hearing the other (for example a failed receiver), both could believe they are master. Nodes follow only the highest term, so the rover still obeys one host; the host with the lower term sees the other's heartbeat and steps down. The dashboard raises TWO\_MASTERS whenever two masters appear within one second, because it always means a hardware or wiring fault.

**Setup for two hosts**

- Both hosts connect to the bus at the electronics-box end (section 3); only the transceiver at the physical end of the cable carries a 120 Ω terminator.
- Link the hosts with Ethernet for the gateway link (section 12). Leadership doesn't depend on Ethernet, so a broken cable can't create two masters.
- Each host's config file sets `host_id` (1 Orin, 2 RPi), `tie_break` (Orin 1, RPi 2) and `auto_takeover` (false).

## Node firmware

All nodes, STM32 and ESP32-S3 alike, are built on one shared library, `anveshak_can_core`. It owns everything CAN-related: driver, safety states, watchdogs, heartbeat, parameters and bootloader entry. A node's own code only implements its job (motor control, reading sensors), so every node behaves the same way on the bus and a fix is made once.

**Layers**

| Layer | What it contains | Written once or per node |
| --- | --- | --- |
| Application | Motor PID, encoder reading, current sensing, sensor drivers | Per node type |
| Messages | Pack/unpack code generated from the DBC; dispatch of received frames to handlers | Generated |
| Core services | State machine, command and master tracking, watchdogs, heartbeat and telemetry, parameters, ISO-TP, bootloader entry | Once |
| CAN driver | One small API (init, send, receive, error state, recover) with three back ends: STM32 bxCAN, STM32 FDCAN (classic mode), ESP32-S3 TWAI | Once per peripheral type |
| Board support | Pin map, clocks, ID switch reading, motor-enable pins, LEDs | Per PCB |

The library builds with CMake for STM32 (CubeMX-generated HAL underneath) and as an ESP-IDF component for the ESP32-S3. Each node project includes it as a git submodule, alongside the DBC repo.

**Node states**

| State | Outputs | Entered when | Left when |
| --- | --- | --- | --- |
| BOOT | Off | Power-up or reset | Clocks, CAN and ID switch initialised, self-test passed |
| DISARMED | Off (brake or coast, set by a parameter) | After BOOT; command watchdog expired; master term changed; DISARM received; E-stop released | ARM received from the current master |
| ARMED | Following commands | ARM from the current master | Watchdog expiry, DISARM, fault, E-stop, master change |
| FAULT | Off | Over-current, over-temperature, under-voltage, encoder failure | Condition gone **and** FAULT\_RESET received |
| ESTOP | Off, latched | ESTOP frame or the node's own E-stop input | ESTOP\_RELEASE from the master |
| BOOTLOADER | Off (hardware-enforced) | ENTER\_BOOTLOADER command, or no valid application | New firmware committed and booted |

The state goes out in every feedback frame and heartbeat, so the dashboard always shows *why* a wheel isn't turning.

**The 20 ms control cycle (50 Hz)**

| Time in cycle | Event |
| --- | --- |
| 0 ms | Master sends SYNC. Every drive node applies the command it received during the previous cycle and samples its encoders and currents. |
| 0–2 ms | Master sends the six DRIVE\_CMDs (class 1). Nodes send DRIVE\_FB\_A and \_B (class 2). Arbitration puts commands first automatically. |
| 2–20 ms | Telemetry, heartbeats, FPGA data, events and service traffic fill gaps. The bus is idle about two-thirds of the time. |

The motor PID runs locally on each node at a much higher rate (for example 1 kHz on a timer interrupt), using the latest setpoint. CAN carries only setpoints and measurements and is never inside the fast control loop, which is why 50 Hz is plenty for a rover.

Applying commands at SYNC adds one cycle (20 ms) of delay, in exchange for all six wheels changing speed at the same instant. For a skid-steer rover, that synchrony matters more than 20 ms of latency.

**Watchdogs (three layers)**

1. **Command watchdog, 100 ms.** No valid command from the current master for 100 ms (5 missed cycles) → outputs to zero, state DISARMED. This is what stops the rover if the master crashes, its gateway stops, or the cable is cut.
2. **Hardware watchdog, about 50 ms.** STM32 independent watchdog (IWDG) or ESP32-S3 task watchdog, fed only from the main loop. If the firmware hangs, the chip resets, comes back DISARMED, and reports the reset cause.
3. **Master watchdog, 300 ms.** No host heartbeat at all for 300 ms → the node flags "no master" in its heartbeat for the dashboard.

**CAN handling inside the node**

- **Receive**: the CAN interrupt copies frames into a ring buffer and returns at once; the main loop (STM32) or a high-priority task (ESP32-S3) processes them. Never do real work inside the interrupt.
- **Transmit**: a small software queue sorted by priority (E-stop, then feedback, then telemetry, then service and bootloader). The hardware mailboxes are fed in order from it.
- **Filters**: the two-filter recipe from section 4, so the node only ever sees frames meant for it.
- **Bus-off**: STM32 bxCAN recovers automatically (ABOM). STM32 FDCAN and ESP32-S3 TWAI need the driver to restart recovery; the core library does that. Every recovery is counted and reported. If a node goes bus-off more than 5 times in a minute, it stops transmitting and lights its fault LED, because something is physically wrong.
- **ESP32-S3 specifics**: pin the CAN task to one core (core 1) and leave Wi-Fi/Bluetooth on the other (core 0), so radio activity never delays CAN.

**Command checks on every node**

- Accept commands only from the current master and term.
- Clamp targets to the node's maximum speed and duty parameters.
- Ramp toward the target at no more than the commanded acceleration limit.
- Treat a sequence counter that stops changing as a lost master, even if frames still arrive.

**Parameters: tune without reflashing**

- Each node type has a parameter table: index, name, type, minimum, maximum, default. It is written in a YAML file next to the DBC and turned into C code at build time.
- Examples: PID gains, current limits, maximum speed, encoder counts per revolution, brake-or-coast when disarmed, telemetry rate.
- The SERVICE messages (section 6) read and write parameters live. SAVE writes them to a dedicated flash area (STM32) or NVS (ESP32-S3), so they survive power cycles.
- Safety-related parameters (limits, gains) can only be changed while DISARMED.

**Node ID and status LEDs**

- The ID switch is read once at boot. An unreadable or zero switch makes the board use ID 31 (unconfigured) and refuse to arm.
- One status LED pattern for every board: slow blink DISARMED, solid ARMED, fast blink BOOTLOADER, solid red FAULT or ESTOP. It saves a lot of time in the pit.

**Testing before hardware**

The core library and state machine compile on a laptop too. Unit tests run against Linux's virtual CAN interface (`vcan0`) in CI, so most bugs are found before anything is flashed.

## Firmware updates over CAN

Every node can be reflashed from either host, or from the dashboard, without opening the rover. STM32 nodes get a small permanent CAN bootloader; ESP32-S3 nodes use their built-in A/B update partitions. Both receive the image over the same multi-frame transport, ISO-TP, on class 7 IDs.

**Why not the STM32's built-in ROM bootloader**

Every STM32 has a factory bootloader, but it is entered by holding the BOOT0 pin high at reset (we can't reach that pin over CAN), it uses fixed IDs that every node would share (so all nodes would answer at once), and on many parts it doesn't support CAN at all. A small custom bootloader solves all three. [Katapult](https://github.com/Arksine/katapult) (a CAN bootloader for STM32 from the Klipper 3D-printer project) and [OpenBLT](https://www.feaser.com/openblt/) are good references to borrow from.

**ISO-TP in one paragraph**

A CAN frame carries at most 8 bytes, but a firmware chunk is kilobytes. ISO-TP (ISO 15765-2) splits a message into a *first frame* (length + 6 data bytes) and *consecutive frames* (7 data bytes each, numbered). After the first frame, the receiver answers with a *flow control* frame saying how many frames it can take before the next pause and how far apart they must be. That stops a fast host from overrunning a slow microcontroller. Linux has ISO-TP built into the kernel (the `can-isotp` module), and small C libraries such as `isotp-c` do the same on the node.

**Speed**

At 500 kbps a worst-case 8-byte frame takes 270 µs, so ISO-TP can move at most about 25 KB/s of payload. Flash erase and write times and flow-control pauses bring the practical rate to roughly **8–12 KB/s** (about double that if the bus later moves to 1 Mbps).

| Image | Typical size | Time over CAN at 500 kbps |
| --- | --- | --- |
| STM32 drive node application | 32–128 KB | About 3–15 s |
| ESP32-S3 application with Wi-Fi stack | About 1 MB | About 1.5–2 minutes |

For ESP32-S3 nodes, updating over Wi-Fi in the pit is much faster (see below); CAN is the path that always works, including on the field.

**STM32 flash layout**

The exact addresses depend on how the chip's flash is divided. Two common cases:

| Region | F0 / F1 / G0 / G4 (2 KB pages) | F4 (16 KB, 64 KB and 128 KB sectors) | Contents |
| --- | --- | --- | --- |
| Bootloader | 0x0800\_0000, 32 KB | 0x0800\_0000, sectors 0–1 (32 KB) | Write-protected, never changes |
| Parameters | 0x0800\_8000, 4 KB (2 pages) | 0x0800\_8000, sectors 2–3 (32 KB) | Saved node parameters |
| Application header | 0x0800\_9000, 256 B | 0x0801\_0000, 256 B | Magic number, node type, version, length, CRC32, git hash |
| Application | 0x0800\_9100 onward | 0x0801\_0100 onward | The node firmware |

The application's linker script starts at the application address, and its startup code points the vector table register (VTOR) there. Write-protect the bootloader sectors through the option bytes, so a bug in the application or a bad update can never erase the bootloader.

**STM32 boot sequence**

1. The bootloader starts. Motor-driver enable pins stay low, guaranteed by **hardware pull-down resistors**, not just firmware.
2. It reads the node ID switch and starts CAN at 500 kbps with the same filters as the application.
3. If the application left a "stay in bootloader" flag in a backup register (set when it received ENTER\_BOOTLOADER), it stays and waits for the host.
4. Otherwise it listens for 300 ms for a bootloader request to its node ID. This is the rescue path if an application crashes immediately after starting.
5. It checks the application header: magic number, node type equal to this board's type, length, CRC32 over the whole image.
6. Valid → jump to the application. Invalid → stay in the bootloader and send heartbeats with state BOOTLOADER, so the dashboard shows the node needs flashing.

**Bootloader commands (ISO-TP on class 7: sub 0 host → node, sub 1 node → host)**

| Command | Host sends | Node replies |
| --- | --- | --- |
| INFO | Nothing | Bootloader version, MCU unique ID, flash size, page or sector layout, node type, current application header |
| ERASE | Start offset and length | OK when done (an F4 128 KB sector takes 1–2 s: the host waits up to 5 s) |
| WRITE | Offset and up to 4 KB of data | OK after the block is programmed and read back |
| VERIFY | Expected CRC32 of the whole image | OK or MISMATCH |
| COMMIT | Nothing | Writes the application header last, so a half-written image is never considered valid |
| BOOT | Nothing | Resets into the new application |
| ABORT | Nothing | Erases the partial image and stays in the bootloader |

**Safety rules for flashing**

- **Node type check.** The image header names the node type; the bootloader refuses an image for a different type, so arm firmware can never be written to a drive board.
- **Only when disarmed.** The flasher refuses to start unless every node is DISARMED and the requesting host is master.
- **Quieter bus.** The master tells nodes to pause telemetry while flashing, giving the transfer more room.
- **Confirm afterwards.** After BOOT, the tool waits for the node's heartbeat and checks the build ID matches the image before reporting success.

**ESP32-S3 nodes**

The ESP32-S3 always runs Espressif's own second-stage bootloader, so there is no separate CAN bootloader. Instead the **running application** receives the update:

1. The flash partition table has two application slots (`ota_0`, `ota_1`). We don't know the modules' flash size, so the layout below fits the smallest common ESP32-S3 module (4 MB); on 8 or 16 MB modules, simply make the two app slots bigger. Partitions: bootloader and partition table (first 36 KB), nvs 24 KB, otadata 8 KB, phy\_init 4 KB, ota\_0 1,920 KB, ota\_1 1,920 KB, coredump 64 KB, leaving about 120 KB spare. Each app slot holds a typical 1 MB application with room to grow.
2. The application receives the image over the same ISO-TP channel and writes it into the slot it isn't running from, using ESP-IDF's OTA functions.
3. When the image is complete and its checksum is correct, it marks the new slot as the boot slot and restarts.
4. **Rollback** is enabled in the build. The new firmware is on probation until it calls the "mark app valid" function, which it does only after it has started CAN, seen a master and run for a few seconds. If it crashes before that, the ESP32-S3 automatically boots the previous slot on the next reset.

Limitation: because the application does the updating, an ESP32-S3 whose application can't run at all can't be updated over CAN. Rollback makes that very unlikely; the fallback is its USB port.

**Wi-Fi updates for ESP32-S3 in the pit (optional)**

If the rover network is up, the same application can fetch its new image over Wi-Fi (HTTP OTA) in a few seconds. Use this for quick development; keep CAN as the path that works on the field and in the competition configuration.

**Host flashing tool**

One Python command-line tool, used on both hosts, on laptops and by the dashboard:

```
anv-flash list                            # all nodes: ID, type, state, firmware build
anv-flash flash --node 4 drive.bin        # one node
anv-flash flash --type drive drive.bin    # every drive node, one at a time
anv-flash rescue --uid 0x1234ABCD app.bin # a board stuck in the bootloader or on ID 31
```

**When everything else fails**

Keep an SWD header (STM32) or USB port (ESP32-S3) reachable on every board. With write-protected STM32 bootloaders and ESP32 rollback, it should only be needed for first-time programming.

## Safety and fault handling

The rover must come to a safe stop on any single failure: a host crash, a cut cable, a hung node, a noisy bus. Software can't guarantee that on its own, so the foundation is a **hardware E-stop** that removes motor power through a contactor, independent of every processor. Everything on CAN sits on top of that.

**The E-stop chain**

1. **Hardware loop (physical and wireless).** The motor-power contactor's coil is powered through one series loop containing:
   - the **physical E-stop button(s)** on the rover, normally-closed contacts;
   - the output relay of a **wireless E-stop receiver**, chosen so that its relay is closed only while it receives a valid signal from the handheld transmitter. Pressing the transmitter's button, its battery dying, or the rover going out of range all open the relay.

   Opening any contact in the loop drops the contactor and removes motor power, whatever the software is doing. Use a dedicated wireless E-stop or industrial radio remote with fail-safe output, never a Wi-Fi or software path. Logic power (hosts, nodes, CAN) stays on, so the rover keeps reporting what happened.
2. **Sensing the loop.** A node with a spare input (the power board once it exists; until then, one drive node with an optocoupled input) monitors the loop. When it opens, that node sends ESTOP on CAN and reports which source opened it as a NODE\_EVENT (ESTOP\_PHYSICAL or ESTOP\_WIRELESS), so the dashboard says *which* E-stop was pressed.
3. **CAN E-stop.** ESTOP (0x000), the highest-priority frame on the bus, accepted from any sender: either host, the dashboard (through a host), the service tool, or the loop-sensing node. Every node latches it.
4. **Release.** Nodes stay in ESTOP until all three happen: the physical and wireless E-stops are reset (the contactor closes), the operator triggers ESTOP\_RELEASE from the master, and the master sends ARM. Closing the contactor alone never restarts motion.

**What happens when things fail**

| Failure | Who notices | What happens |
| --- | --- | --- |
| Master host crashes, or its gateway stops | Nodes (100 ms command watchdog); standby (250 ms heartbeat loss) | Nodes stop and disarm; standby raises LEADER\_LOST and offers to take over; the operator chooses and re-arms |
| Both hosts gone | Nodes (command watchdog) | All nodes stop and stay disarmed |
| Node firmware hangs | Node's hardware watchdog; hosts (heartbeat missing for 300 ms) | Node resets and returns DISARMED; dashboard shows the reset cause |
| Node goes bus-off | Node's error counters | Automatic recovery, counted and reported; silences itself after 5 per minute |
| Cable cut part-way along the bus | Nodes beyond the cut (no commands); hosts (their heartbeats vanish) | Those nodes stop; the master stops the remaining drive nodes too and raises an alarm |
| CANH shorted to CANL, or a dead short to ground | Every controller (bus-off) | All nodes stop on their command watchdogs; the bus recovers when the short is removed |
| Electrical noise from motors | Rising TEC/REC, error frames on the dashboard | Warning long before frames are lost; fix wiring, grounding or add chokes |
| Over-current or over-temperature at a node | The node itself | FAULT: outputs off, fault bits in feedback; needs FAULT\_RESET to clear |
| Two hosts both acting as master | Nodes (term numbers); hosts; dashboard | Nodes obey only the highest term; the other host steps down; alarm raised |
| Node running an older protocol version | Master (heartbeat protocol field) | Master refuses to arm it until it is updated |
| RPi MCP2515 dropping frames | RPi overrun counter | Dashboard warning; as standby, harmless; as master, reduce rates or switch master |

**Checks every node makes on every command**

- The command must come from the current master and term.
- Targets are clamped to the node's configured maximum speed and duty.
- Speed changes are ramped at no more than the acceleration limit, so a corrupted or extreme target can't cause a sudden jump.
- A sequence counter that stops changing counts as a lost master even if frames keep arriving (protects against a host stuck re-sending its last command).

**Power-up, brown-out and reset**

- Motor-driver enable pins have **pull-down resistors**, so an unprogrammed board, a board in reset or in the bootloader can never drive a motor.
- Enable the MCU's brown-out reset. Report the last reset cause in telemetry, so a battery sag shows up on the dashboard as "brown-out" instead of an unexplained reboot.
- After any reset, a node starts DISARMED and needs an explicit ARM.

**What the operator sees**

Every safety event (E-stop, fault, master change, bus-off, missing node) appears on the dashboard's event timeline with a timestamp and the node involved (section 13). Most field problems are diagnosed from that timeline plus the raw CAN log.

## Host software

Each host runs one program, the **gateway** (`anv_can_gateway`), which is the only software that touches `can0`. It runs the election, sends SYNC and commands when master, decodes every frame, tracks node health and bridges everything to ROS 2. Autonomy, teleoperation and the dashboard all talk to the gateway, never to the bus directly. That keeps the "one sender per ID" rule enforceable in one place.

**Inside the gateway**

| Module | Job | Language |
| --- | --- | --- |
| CAN I/O | Linux SocketCAN raw socket; kernel-level ID filters; kernel timestamps on every frame | C++ |
| Election | Sends HOST\_HB at 20 Hz and LEAD\_REQUEST when the operator picks a leader; runs the rules in section 8; exposes current role and term | C++ |
| Cycle scheduler | When master: every 20 ms sends SYNC, then the six DRIVE\_CMDs from the latest targets. Sends zero speed if targets are older than 100 ms. | C++ |
| Decoder | Turns every frame into typed values using code generated from the DBC | C++ |
| Node registry | Per node: alive or missing, state, firmware build, uptime, error counters, measured vs expected message rates | C++ |
| ROS 2 bridge | Publishes state, receives targets, offers services (arm, disarm, E-stop, parameters, handover) | C++ (rclcpp) |
| Recorder | Writes every frame with its timestamp to a rolling log | C++, or `candump -l` alongside |

The timing-critical parts are C++ so the 20 ms cycle stays steady even when the Orin is busy with perception. Tools around the gateway (flasher, dashboard back end, scripts, tests) are Python, using `python-can`, `cantools` and the same DBC.

**Running the 20 ms cycle reliably**

- Use a timer based on the monotonic clock (`clock_nanosleep` with absolute deadlines), not a sleep of 20 ms in a loop, which drifts.
- Give the CAN thread real-time priority (`SCHED_FIFO`) and, on the RPi, pin it to one CPU core.
- Log any cycle that starts more than 2 ms late; the dashboard shows the count.

**ROS 2 interface**

| Kind | Name | Content |
| --- | --- | --- |
| Subscribes | `/cmd/drive` | Six wheel targets (or a twist that the gateway converts) |
| Publishes | `/can/drive/feedback` | Speeds, currents, encoder counts, states, faults for all wheels |
| Publishes | `/can/sensors/fpga` | Decoded FPGA sensor values |
| Publishes | `/can/nodes` | The node registry |
| Publishes | `/can/bus` | Load, frames per second, error state, master and term |
| Publishes | `/diagnostics` | Standard ROS diagnostics, for existing tools |
| Services | `/can/arm`, `/can/disarm`, `/can/estop`, `/can/param_get`, `/can/param_set`, `/can/request_lead` | Actions from autonomy, teleop or the dashboard |

The standby gateway publishes the same feedback topics with a `role` field, so any consumer can tell which host is in charge.

**Humble and Jazzy, on either host, at the same time**

Each host can run ROS 2 Humble or Jazzy independently (for example the Orin on Humble and the RPi on Jazzy), and everything keeps working. That is possible because nothing that crosses between hosts depends on ROS:

| What crosses between hosts | How it travels | Depends on the ROS distro? |
| --- | --- | --- |
| Leadership, E-stop, node state, all rover data | CAN | No |
| Drive targets from a non-leading host's autonomy or teleop, shared state, debug-level changes | **Gateway link**: a small UDP protocol between the two gateways over Ethernet | No |
| Dashboard | WebSocket from the gateway to the browser | No |

ROS 2 is used only *inside* each host, between the gateway and that host's own autonomy, teleop and tools, so it always runs natively. Humble and Jazzy are not guaranteed to talk to each other over DDS, which is why no cross-host traffic relies on it.

**The gateway link**

- UDP on the rover Ethernet, one port, messages defined in the same definitions repo as the DBC (so both sides always agree on the format), each with a sequence number and a timestamp.
- Carries: drive targets forwarded to the leading host, the debug level, dashboard control actions, and a 10 Hz "I'm alive" with the host's role.
- If the link drops, nothing unsafe happens: the leader just stops receiving the other host's targets, and after 100 ms of stale targets it sends zero speed (the same rule as for local targets).

**One codebase for both distros**

- Use only stable `rclcpp` APIs and standard message packages; our own messages and services live in one interface package (`anveshak_can_msgs`).
- The few build differences between Humble and Jazzy (for example, how CMake links ROS dependencies) are handled with `if` checks on `ROS_DISTRO` in CMake, in one place.
- CI builds and tests every change on both: Humble on Ubuntu 22.04 and Jazzy on Ubuntu 24.04, for arm64 and x86, using the official ROS Docker images. A change that breaks either distro can't be merged.
- Lichtblick connects through `foxglove_bridge`, which is available for both distros.

**Setting up each host**

- Same code and same config on both; only `host_id` (1 Orin, 2 RPi) and `priority` differ.
- `can0` is configured at boot by a systemd-networkd `.network` file (bitrate 500 kbps, `restart-ms 100`, `berr-reporting on`), not by hand.
- The gateway runs as a systemd service with `Restart=always`, so a crash restarts it within a second. (While it is down, the other host or the node watchdogs keep the rover safe.)

**Logging and replay**

- **Always record.** Every frame with its timestamp goes to a rolling log; keep the last 20 runs or so. It costs very little and is the first thing you'll want after any field problem.
- ROS-level data goes into MCAP bags (ROS 2's recording format) alongside.
- **Replay**: `canplayer` from can-utils plays a recorded log back onto a virtual CAN interface (`vcan0`), so a field failure can be re-run through the gateway and dashboard at the desk.

**Simulator**

A Python script that pretends to be every node on `vcan0`: sends heartbeats, turns commands into plausible feedback, and injects faults on request (node drops off, bus-off, over-current). With it the software team can develop the gateway, autonomy and dashboard without the rover, and CI can test the election by killing simulated hosts.

**Useful command-line tools (can-utils)**

| Tool | Use |
| --- | --- |
| `candump -td can0` | Watch live traffic with time between frames |
| `cansniffer can0` | See which IDs are changing |
| `cansend can0 120#...` | Send one frame by hand (service tool only, rover disarmed) |
| `canbusload can0@1000000 -r -t -b` | Live bus load |
| `ip -details -statistics link show can0` | Error state, error counters, overruns |

## Master dashboard

The dashboard is a web page served by the gateway on each host, so any laptop or phone on the rover network opens it in a browser with nothing to install. It shows the health of the bus and every node at a glance, and lets the team inspect traffic, tune parameters and flash firmware. For general plotting and replay, pair it with an existing ROS tool rather than rebuilding one.

**How data gets from a node to your browser**

The nodes (STM32, ESP32-S3, FPGA) never connect to the network or the dashboard; the STM32s have no Wi-Fi at all, and they don't need it. Only the hosts are on the network:

```
STM32 / ESP32-S3 / FPGA  --CAN-->  gateway on the RPi or Orin  --Wi-Fi / Ethernet-->  browser on a laptop or phone
                                    (serves the dashboard)
```

- **Node to dashboard**: a node puts frames on CAN (feedback, heartbeats, events); the gateway decodes them and pushes them to the browser.
- **Dashboard to node**: every button, slider or setting goes from the browser to the gateway, which turns it into CAN frames for the node.
- Your laptop connects to whichever host is reachable over the rover's network or radio link; the ESP32-S3s' own Wi-Fi is only used, optionally, for fast firmware updates in the pit.

**Two tools, two jobs**

| Tool | Use it for | Effort |
| --- | --- | --- |
| **Custom CAN dashboard** (served by the gateway) | Bus and node health, election state, raw frames, parameters, flashing, E-stop, arm/disarm, handover | A few weeks of work, built in stages |
| **[Lichtblick](https://github.com/lichtblick-suite/lichtblick)** (open-source fork of Foxglove Studio) or **PlotJuggler**, connected through the Foxglove WebSocket bridge | Plotting any ROS topic, camera and 3D views, replaying MCAP bags | An afternoon |

The custom part is worth building because no generic tool understands node states, terms, parameters or the bootloader.

**How it works**

- **Back end**: a Python web server (FastAPI) running beside the gateway. It reads decoded state and raw frames from the gateway and uses the same DBC for names and units.
- **Link to the browser**: one WebSocket per open page. The server only sends what that page shows, reduced to 10–20 updates per second, so it works over a weak radio link to the base station.
- **Front end**: a single-page web app (Svelte or React) with a fast plotting library such as uPlot for live charts.
- **Both hosts serve it.** Open whichever is reachable; the header shows which host is master.

**Pages**

| Page | What it shows | What you can do |
| --- | --- | --- |
| Overview | Master host and term, standby status, bus state (error-active / passive / bus-off), bus load %, frames per second, error frames per second, RPi overrun count, E-stop state | E-stop, arm all, disarm all, choose or change which host leads (shown as a prompt whenever no leader is chosen or the leader is lost) |
| Nodes | One row per node: name, ID, state, time since last heartbeat, firmware build, uptime, TEC/REC, bus-off count, last reset cause, temperatures, fault bits, all colour-coded | Reset a node, clear a fault |
| Live signals | Any signal from the DBC, as scrolling plots with pause and cursor | Export to CSV |
| Message rates | Expected vs measured rate for every ID; missing, late or unexpected IDs highlighted | none |
| Raw traffic | candump-style live view with filters by ID, node or class; decoded values beside the raw bytes | Send a frame by hand (service login, rover disarmed) |
| Parameters | Each node's parameter table with current value, limits and default | Read, write, save to flash, restore defaults |
| Firmware | Firmware build of every node, mismatches flagged | Upload an image, flash one node or all nodes of a type, progress bar |
| Test bench | One motor at a time | Jog with a hold-to-run button; step-response test for PID tuning |
| Events and logs | Timeline of E-stops, faults, master changes, bus-off events, nodes appearing or disappearing; list of recorded runs | Start or stop recording; download the raw CAN log or MCAP bag |

**Control panel: driving, speeds, rates and frames from the website**

The dashboard can also *control* the rover: drive it, raise or lower speeds, change how often each message is sent, tune parameters, and compose, send or override frames. Every control goes browser → gateway → CAN; the browser never touches the bus directly, so all the safety rules still apply.

*1. Who is in control.* Drive targets can come from autonomy, a teleop joystick at the base station, or the dashboard. The leading gateway accepts targets from **one source at a time**, shown in the header. "Take manual control" switches it to the dashboard (autonomy and teleop are then ignored); "Release" hands it back. Only one browser can hold control at once (a control lock, released automatically if that browser disconnects). A dashboard opened on the standby host works too: its requests reach the leader over the gateway link.

*2. Drive controls.*

| Control | What it does |
| --- | --- |
| On-screen joystick, arrow keys / WASD, or a USB gamepad (read by the browser) | Forward speed and turn rate for the whole rover |
| Speed + / − buttons | Raise or lower the target in fixed steps (step size adjustable) |
| Per-wheel sliders (12 channels, optionally locked together per side) | Drive individual channels, with target and measured speed shown side by side |
| Speed cap and acceleration limit sliders | Upper limits for this session, enforced by the gateway and again by each node |
| Mode selector | Velocity, open-loop duty, brake or coast |
| Arm, disarm, E-stop | Always visible on this page |

Driving from the browser is **hold-to-run**: targets are sent only while a button or key is held, refreshed 20 times a second over the WebSocket. If the gateway hears nothing new for 200 ms (released key, closed tab, lost Wi-Fi), it sends zero speed.

*3. Rates page.* Lists every periodic message with its current rate, the allowed range and a selector to change it, for example feedback, telemetry, heartbeats, FPGA sensors, debug pages and the control cycle itself.

- **Node-sent rates** are node parameters (e.g. `rate_fb_hz`, `rate_telem_hz`, `rate_hb_hz`), changed through SERVICE messages. **Host-sent rates** (the SYNC / control cycle and host heartbeats) are gateway settings.
- A **projected bus-load bar** updates as you move a selector, before anything is applied; the gateway refuses changes that would exceed 50%.
- **Safety floors** can't be overridden: host heartbeats stay at 20 Hz or above, node heartbeats at 5 Hz or above, and the control cycle stays between 25 and 50 Hz at 500 kbps (the node command watchdog stays at 100 ms).
- Changes apply immediately and are lost at reboot unless you press **Save as default**, which writes them to each node's flash.

*4. Frame lab (debug level 2 or higher).*

- **Compose and send**: pick any message from the DBC and get a form with each signal by name, unit and allowed range (sliders or number boxes), or switch to raw mode (ID + hex bytes). Send once, or repeat at a chosen rate for a chosen time.
- **Override**: change a signal in frames the gateway itself sends, for example "drive node 3 channel A target = 1.5 rad/s" or "scale all drive targets by 0.5". Overrides are highlighted on every page and expire after a set time.
- **What can't be changed**: CAN gives no way to alter a frame another node transmits. To change what a node sends (its rates, scaling or behaviour), change its parameters or send it a command. The frame lab therefore refuses to transmit IDs that belong to nodes (feedback, telemetry, heartbeats), because that would impersonate the node and collide with it. Such frames can be injected only on the simulator's virtual bus.
- All frames go out through the **leading** host's gateway, keeping the one-sender-per-ID rule. Raw frames in classes 0, 1 and 7 (network control, commands, bootloader) are allowed only while the rover is disarmed; E-stop is always allowed.

*5. Live tuning.* The Parameters and Test bench pages get sliders for PID gains and limits, with a step-response plot that updates after each change (best with the node at debug level 3).

*6. Profiles.* Named sets of rates, debug levels, speed caps and limits, for example **Competition**, **Testing**, **PID tuning** and **Bench**, applied in one click. Profiles are files in the definitions repo, so they are reviewed and versioned like code.

*7. Audit and undo.* Every control action (who, from which browser, what changed, old and new value) is recorded as a CONTROL\_ACTION event and shown on the event timeline. "Undo last change" restores the previous value of any rate, parameter or override.

*From scripts.* The same controls are ROS 2 services on the gateway, identical on Humble and Jazzy: `/can/take_control`, `/can/set_rate`, `/can/send_frame`, `/can/set_override`, `/can/apply_profile`.

**Safety rules in the dashboard**

- Anyone on the rover network can **view**. Anything that moves hardware, changes parameters or flashes firmware needs a login and a confirmation.
- Jog and test actions are **hold-to-run**: release the button, close the tab or lose the connection, and the target goes to zero.
- The **E-stop button is on every page** and goes straight to the gateway, ahead of any other request.

**Small features that pay off**

- **Node health score** (green / yellow / red) combining heartbeat age, error counters, faults and resets, so a problem is visible from across the pit.
- **Event timeline**: usually the fastest way to understand a field failure ("node 6 bus-off at 14:02:11, then drive disarmed").
- **Desk tools** on the same DBC: SavvyCAN or `cantools monitor` on a laptop with a USB-CAN adapter on the service port.

**Build order**

1. Overview and Nodes pages: most of the debugging value.
2. Raw traffic and message rates.
3. Parameters.
4. Firmware page, reusing the `anv-flash` library.
5. Test bench, events and logs.

6) Control panel: control lock and drive controls first, then the Rates page with the bus-load guard, then profiles; the frame lab last.

## Debug levels: switching debugging on and off

Debugging is **off unless you turn it on**. A small set of safety information is always sent; everything else (extra events, debug data, recordings, incident reports) belongs to one of four debug levels, chosen from the dashboard or ROS 2 for the whole rover or per node. Every node returns to its saved default level after a reboot, so a forgotten debug session never survives a power cycle.

**Always on, whatever the level**

| Item | Why it can't be switched off |
| --- | --- |
| E-stop, SYNC, commands, feedback | The rover can't drive safely without them |
| Host and node heartbeats | Leader-loss and missing-node detection depend on them |
| Fault bits in feedback; NODE\_EVENT for **error** and **critical** severity | The operator must always see real faults |
| CAN error counters and bus-off counts (in the heartbeat flags and telemetry) | Early warning of wiring problems |

These cost about 31% of the bus today, most of it the commands and feedback the rover needs anyway.

**The four levels**

| Level | Name | What nodes send in addition | What the hosts do | Extra bus load today |
| --- | --- | --- | --- | --- |
| 0 | Off | Telemetry slowed to 1 Hz | Live dashboard only; critical events written to the log | −3% (saves) |
| 1 | Normal | Warnings; telemetry at 10 Hz | Event log file; Errors page | 0 (the baseline) |
| 2 | Debug | Info events; debug pages (PID setpoint, measurement, integrator, output) at 10 Hz | Raw CAN recording, MCAP recording, incident reports, all dashboard pages | About +3% |
| 3 | Trace | Debug pages at 50 Hz, for **one node at a time** | Same as level 2 | About +3% |

Debug pages are two new messages per actuator node, class 3 sub-types 2 and 3 (DRIVE\_DEBUG\_A / \_B, e.g. 0x322 / 0x323 for drive node 1), each with four i16 values for one channel: setpoint, measured value, integrator, output.

**Bus-load guard.** Before applying a level, the gateway computes the resulting load from the DBC rates. If it would exceed 50% it refuses and suggests a smaller change (for example level 2 on selected nodes only, or debug pages at 5 Hz). With all planned nodes on a 500 kbps bus, rover-wide level 2 is right at the limit, so expect to use it per node.

**DEBUG\_LEVEL frame (0x004, 7 bytes, master only)**

| Byte | Field |
| --- | --- |
| 0 | Target node ID (0 = all nodes) |
| 1 | Level, 0–3 |
| 2 | Category mask: bit 0 events, bit 1 telemetry, bit 2 debug pages, bit 3 error counts by type (lets you turn on, say, only debug pages) |
| 3–4 | Auto-off timer in minutes (0 = until changed); the node returns to its default level when it expires |
| 5 | Master node ID |
| 6 | Master term (low byte) |

Each node reports its current level in its heartbeat (NODE\_HB flags byte, bits 4–5), so the dashboard always shows what every node is really doing.

**How you switch it**

- **Dashboard**: a debug control in the header for the whole rover, and a per-node selector on the Nodes page. While any node or host is above level 1, a coloured **"DEBUG ON"** banner shows on every page with the remaining auto-off time.
- **ROS 2** (same on Humble and Jazzy): the gateway parameter `debug_level` (`ros2 param set /anv_can_gateway debug_level 2`) for the whole rover, and the service `/can/set_debug` (node, level, minutes, category mask) for one node.
- Whichever host you use, the request reaches the leading host over the gateway link, and only the leader puts DEBUG\_LEVEL on the bus (one sender per ID).
- **Auto-off**: levels 2 and 3 default to a 30-minute timer, renewable from the dashboard.

**Defaults**

- Every node has a `debug_default` parameter and each gateway a matching setting: **1 (normal) for testing, 0 (off) for competition**. Switching the whole rover's defaults is one click with the dashboard's profiles (section 13).
- The debug code stays in the firmware so levels can change at run time. If flash ever gets tight, level-3 trace code can be removed with a build flag.

**Flight recorder (separate switch, on by default)**

The gateway keeps the last 60 seconds of raw CAN traffic in memory (about 1.3 MB) and writes it to disk only when a critical event happens, or when you press "save last minute" on the dashboard. It catches the crash you didn't have debugging on for, without recording continuously. It has its own setting, `flight_recorder` (true / false), independent of the debug level.

## Error reporting and diagnostics

Every problem anywhere in the system becomes a **numbered, timestamped error event** with a severity, the node it came from, and a human-readable explanation with likely causes. Nodes report events over CAN as they happen and keep a black-box log that survives resets; crashes leave a crash record; the gateway adds bus-level and host-level events; and one click on the dashboard produces an incident report with everything needed to debug it.

**Five layers of error information**

| Layer | Where it lives | What it captures |
| --- | --- | --- |
| 1. Live status | Fault bits and state in feedback and heartbeats (section 6) | What is wrong *right now* |
| 2. Event reports | NODE\_EVENT frames on CAN, sent as things happen | What happened, when, with details |
| 3. Node black box | RAM ring buffer + flash log on each node, read over SERVICE | History that survives resets and missed frames |
| 4. Crash records | Reserved RAM (STM32) or core dump partition (ESP32-S3) | Exactly where firmware crashed |
| 5. Host incident reports | Gateway event log + raw CAN log + snapshots | The full picture around a failure, as one file |

**One error-code registry for everything**

All error codes are defined once in `errors.yaml` in the shared definitions repo, next to the DBC. Each entry has a code, name, default severity, a one-line description, likely causes and the suggested fix. The build turns it into a C header for firmware and a Python table for the gateway and dashboard, so a code shown on the dashboard always comes with its explanation. CI rejects duplicate codes.

Codes are 16 bits: the high byte is the category, the low byte the specific error.

| Code | Name | Severity | Typical cause |
| --- | --- | --- | --- |
| **0x01xx** | **CAN and communication** |  |  |
| 0x0101 | CAN\_ERROR\_PASSIVE | Warning | Error counter passed 128: wiring, noise, termination |
| 0x0102 | CAN\_BUS\_OFF | Error | Error counter passed 255: this node's transceiver, wiring or bitrate |
| 0x0103 | CAN\_BUS\_OFF\_RECOVERED | Info | Rejoined the bus after bus-off |
| 0x0104 | CAN\_RX\_OVERFLOW | Warning | Receive buffer full, frames lost: CPU too busy or filters too wide |
| 0x0105 | CAN\_TX\_TIMEOUT | Warning | Frame not sent within 20 ms: bus jammed or node error-passive |
| 0x0106 | CMD\_TIMEOUT | Error | No command from master for 100 ms |
| 0x0107 | MASTER\_CHANGED | Info | New term seen; node disarmed |
| 0x0108 | CMD\_FROM\_NON\_MASTER | Warning | Commands from a host that isn't master (detail = sender) |
| 0x0109 | CAN\_ACK\_ERROR | Warning | Nobody acknowledged: node alone on bus, or everyone else at another bitrate |
| **0x02xx** | **Power** |  |  |
| 0x0201 | UNDERVOLTAGE | Error | Supply below limit (detail = mV) |
| 0x0202 | OVERVOLTAGE | Error | Supply above limit (detail = mV) |
| 0x0203 | BROWNOUT\_RESET | Error | MCU reset by brown-out: battery sag, bad connector |
| **0x03xx** | **Motor and driver** |  |  |
| 0x0301 / 0x0302 | OVERCURRENT\_A / \_B | Error | Current above limit (detail = mA) |
| 0x0303 | DRIVER\_FAULT\_PIN | Error | Motor driver reported a fault |
| 0x0304 | STALL\_DETECTED | Warning | Commanded speed but no movement for 500 ms |
| 0x0305 | OUTPUT\_SATURATED | Warning | PID at 100% output for over 1 s: load too high or gains wrong |
| **0x04xx** | **Thermal** |  |  |
| 0x0401 | DRIVER\_OVERTEMP | Error | Driver temperature above limit (detail = °C) |
| 0x0402 | MCU\_OVERTEMP | Warning | MCU temperature above limit |
| **0x05xx** | **Sensors and encoders** |  |  |
| 0x0501 / 0x0502 | ENCODER\_A / \_B\_NO\_COUNTS | Error | Motor driven but encoder not counting: unplugged or broken |
| 0x0503 | ENCODER\_DIRECTION | Error | Encoder counts opposite to drive direction: swapped wires |
| 0x0504 | SENSOR\_READ\_FAIL | Warning | I2C/SPI sensor didn't answer (detail = sensor index) |
| **0x06xx** | **System** |  |  |
| 0x0601 | WATCHDOG\_RESET | Error | Firmware hung and the hardware watchdog reset it |
| 0x0602 | HARD\_FAULT | Critical | Firmware crashed; crash record available |
| 0x0603 | STACK\_OVERFLOW | Critical | A task overran its stack (ESP32-S3 / RTOS) |
| 0x0604 | PARAMS\_CORRUPT | Error | Parameter area failed its CRC; defaults loaded |
| 0x0605 | FLASH\_WRITE\_FAIL | Error | Saving parameters or logs failed |
| 0x0606 | NODE\_ID\_INVALID | Critical | ID switch unreadable or zero; running as node 31 |
| 0x0607 | SELF\_TEST\_FAIL | Error | Power-on self-test failed (detail = which test) |
| **0x07xx** | **Boot and update** |  |  |
| 0x0701 | APP\_CRC\_FAIL | Critical | Application image invalid; staying in bootloader |
| 0x0702 | WRONG\_NODE\_TYPE | Error | Image for a different node type rejected |
| 0x0703 | OTA\_ROLLBACK | Error | ESP32-S3 new image failed and rolled back |
| **0x08xx** | **Protocol** |  |  |
| 0x0801 | PROTOCOL\_MISMATCH | Error | Node's protocol version differs from the master's |
| 0x0802 | BAD\_FRAME | Warning | Known ID with the wrong length or an invalid value |
| **0x09xx** | **Safety** |  |  |
| 0x0901 | ESTOP\_PHYSICAL | Critical | A physical E-stop button opened the contactor loop |
| 0x0902 | ESTOP\_WIRELESS | Critical | Wireless E-stop opened the loop: button pressed, transmitter battery flat, or out of range |
| 0x0903 | ESTOP\_CAN | Critical | ESTOP frame received over CAN (detail = sender, when known) |
| **0x80xx** | **Host and gateway (raised by the gateway, not by nodes)** |  |  |
| 0x8001 | NODE\_MISSING | Error | No heartbeat from a node for 300 ms |
| 0x8002 | NODE\_RETURNED | Info | A missing node is back (uptime shows whether it rebooted) |
| 0x8003 | NODE\_REBOOTED | Warning | Uptime went down: the node reset |
| 0x8004 | MESSAGE\_RATE\_LOW | Warning | An ID arriving slower than the DBC rate |
| 0x8005 | UNKNOWN\_ID | Warning | A frame with an ID not in the DBC |
| 0x8006 | CYCLE\_LATE | Warning | Control cycle started more than 2 ms late |
| 0x8007 | TWO\_MASTERS | Critical | Two hosts claiming master |
| 0x8008 | HOST\_RX\_OVERRUN | Warning | Host's CAN controller dropped frames (RPi MCP2515) |
| 0x8009 | BUS\_ERROR\_BURST | Warning | More than 10 bus error frames in a second (detail = error type) |
| 0x800A | GATEWAY\_RESTARTED | Warning | Gateway process crashed and was restarted |
| 0x800B | LEADER\_LOST | Critical | Master's heartbeat missing for 250 ms; the other host offers to take over |
| 0x800C | NO\_LEADER | Info | Hosts are up but no leader has been chosen yet |
| 0x800D | LEAD\_CHANGED | Info | A host took the lead (detail = new master and who requested it) |
| 0x800E | CONTROL\_ACTION | Info | Someone changed a rate, parameter, override, control source or debug level from the dashboard or ROS 2 (detail = what changed) |
| 0x800F | CHANGE\_REFUSED | Warning | A requested change was refused: bus load over 50%, below a safety floor, or rover armed |
| 0x8010 | GATEWAY\_LINK\_LOST | Warning | No messages from the other host's gateway over Ethernet for 1 s |

**NODE\_EVENT message (class 3, sub 7; 8 bytes; sent when an event happens)**

Drive node 1 sends it on 0x327; any node *n* on (3 << 8) | (n << 3) | 7.

| Byte | Field |
| --- | --- |
| 0 | bits 0–1 severity (0 info, 1 warning, 2 error, 3 critical); bit 7 set = condition cleared (e.g. over-temperature ended) |
| 1–2 | Error code (u16) |
| 3 | Event sequence number (u8, increments with every event from this node) |
| 4–5 | Detail value (u16: measured value, channel, sensor index, sender, depending on the code) |
| 6–7 | Node uptime at the event (u16, units of 0.1 s) |

Rules that keep events useful without flooding the bus:

- **Edge-triggered.** A node sends an event when a condition starts and again (with the "cleared" bit) when it ends, not continuously while it lasts.
- **Rate-limited.** At most one event per code per second per node. Repeats inside that second are counted in the black-box log but not sent.
- **Sequence numbers.** The gateway checks for gaps. A gap means an event frame was lost, and the gateway reads the missing ones from the node's black box automatically.
- **Bus cost** is negligible: even 20 events per second across the whole rover adds 0.3% load.

**Node black box**

Every node keeps its own history, so nothing is lost when a host misses a frame, reboots, or wasn't connected at the time.

- **RAM log**: the last 64 events (code, severity, detail, uptime, boot count, repeat count).
- **Flash log**: errors and criticals are also written to a small flash area (STM32: a reserved page; ESP32-S3: an NVS namespace), keeping the last 32. It survives resets and power cycles, so a node that rebooted can still tell you why.
- **Boot count**: incremented on every start and saved in flash, so events can be tied to a specific power-on.
- **Read and clear** with two new SERVICE opcodes: **6 LOG\_READ** (index field = entry number, newest first; responses carry code, detail, uptime and boot count over two frames) and **7 LOG\_CLEAR**.
- **Flash wear**: writes happen only for errors and criticals and are rate-limited, so even a constant fault writes at most once a second.

**Power-on self-test**

At boot, before going DISARMED, each node checks what it can without moving anything: supply voltage in range, driver fault pin clear, encoder lines not stuck, sensors answering, parameter CRC valid, ID switch valid. Each failed check raises SELF\_TEST\_FAIL with the test number in the detail field. The pit crew sees a board problem before the rover moves.

**Crash records**

*STM32.* When the firmware crashes, the CPU enters the HardFault handler. Ours copies the crash state into a small RAM area that is not cleared at reset: the program counter and link register at the moment of the crash, the status register and the fault status registers (CFSR, HFSR, MMFAR, BFAR), plus a magic number. It then resets. On the next boot the node sees the magic number, raises HARD\_FAULT, writes the record to the flash log, and returns it through SERVICE INFO. Because the heartbeat carries the firmware build ID, the host can fetch that build's ELF file from CI and run `arm-none-eabi-addr2line` to turn the program counter into a **source file and line number**. The dashboard does this automatically.

*ESP32-S3.* ESP-IDF can save a core dump to a flash partition on a crash (panic). On the next boot the node reads the reset reason, raises HARD\_FAULT or STACK\_OVERFLOW with the crashing program counter in the event, and keeps the full core dump in flash. The full dump is fetched over USB or Wi-Fi in the pit (it is too large to pull quickly over CAN) and decoded with ESP-IDF's core dump tool.

*Watchdog resets* (WATCHDOG\_RESET) record which task or loop last ran, if the firmware sets a "breadcrumb" variable in its main loop.

**Decoding CAN bus errors**

CAN controllers can report *what kind* of error occurred, which usually points straight at the cause.

- **On hosts**: with `berr-reporting on` (section 4), Linux delivers every bus error to the gateway as a special error frame that includes the error type and where in the frame it happened. `candump -e can0` shows them decoded.
- **On STM32**: the "last error code" field (LEC in bxCAN, PSR in FDCAN) gives the same types; the core library counts them per type and includes the totals in telemetry.
- **On ESP32-S3**: the TWAI driver raises bus-error alerts; the core library counts them and reports error-passive and bus-off transitions.

What each error type usually means:

| Error type | Usually caused by |
| --- | --- |
| ACK error | The sender is alone on the bus, every other node is at a different bitrate, or a transceiver is in standby (SN65HVD230 Rs pin pulled high) |
| Stuff or form errors on many nodes | Bitrate or sample-point mismatch, missing termination, reflections from long branches |
| CRC errors | Noise or reflections; check termination (should read 60 Ω) and grounding |
| Bit errors | Two senders on the same ID, a faulty transceiver, or a short on the bus |
| Errors only while motors run | Grounding, missing common ground wire, CAN cable routed beside motor wires |
| One node going bus-off repeatedly | That node: its connector, its transceiver, its bitrate setting or its crystal |
| Errors rising with temperature | Crystal or connector problems; poor solder joints |

The dashboard shows these explanations next to the counts.

**Gateway event log and incident reports**

- The gateway turns every NODE\_EVENT, every bus error burst and its own checks (missing nodes, late cycles, two masters, overruns) into one **event stream**. Each event carries the host timestamp, node, code, severity, detail, explanation from the registry, and the raw frame.
- Events are published on ROS 2 as `/can/errors` and stored in a per-run log file (one JSON line per event), next to the raw CAN log.
- **Incident report**: on any critical event, or on demand from the dashboard, the gateway saves a bundle:
  - the raw CAN log from 30 s before to 10 s after the event
  - all gateway events in that window
  - a snapshot of every node: state, firmware build, uptime, boot count, error counters, last telemetry
  - every node's black-box log, fetched over SERVICE
  - parameters of the nodes involved
  - crash records with source lines, if any
  - an auto-written summary page: what happened, in time order, in plain words
- The bundle is one zip file, named with the date, time and first error code, so the team can attach it to an issue and debug it at the desk with replay (section 12).

Which of these run depends on the debug level (previous section): the live event stream and critical-event logging always run; the event log file from level 1; raw CAN recording, MCAP and incident reports from level 2. The flight recorder, if switched on, can still produce an incident report at level 0 or 1, from its last 60 seconds.

**Errors page on the dashboard**

This extends the dashboard in section 13 with one more page:

| Part of the page | Shows | Actions |
| --- | --- | --- |
| Active problems | Conditions not yet cleared, worst first, with node and duration | Acknowledge |
| Event list | Every event this run, filterable by node, code, severity, time | Click for explanation, likely causes, raw frames around it |
| Counts | Matrix of nodes × error categories, so a node or a category that stands out is obvious | Reset counts |
| Bus errors | Error frames per second by type, with the cause table above | none |
| Black boxes | Each node's flash log, including events from before this run | Read, clear |
| Crashes | Crash records with decoded source file and line | Download raw record |
| Incident reports | Saved bundles | Create one now, download |

**Build order**

1. Error registry and NODE\_EVENT in the core library (cheap, immediately useful).
2. Gateway event stream and the Errors page list.
3. Black-box flash log and LOG\_READ.
4. STM32 crash records with automatic source-line decoding.
5. Incident report bundles.

## Bring-up plan, tests and assumptions

Build the system in small stages, each with a test that has to pass before moving on. Most CAN problems are much easier to find with two nodes on a bench than with fifteen in a rover.

**Bring-up stages**

1. **Two devices on the bench at 500 kbps.** One STM32 node and a laptop with a USB-CAN adapter (remember: a lone node gets no ACK). Check the TCAN1044AV wiring first (VCC 5 V, VIO 3.3 V, STB low). Then check bit timing with `candump`, then the heartbeat, then a command and its feedback, using the DBC-generated code.
2. **ESP32-S3 node and the Tang Nano 20K join.** Same tests; confirm the bus-off recovery code works by briefly shorting CANH to CANL.
3. **Gateway and simulator on `vcan0`.** Leader choice, lead changes, leader loss, the 20 ms cycle and the ROS 2 interface, with no hardware at all, on both Humble and Jazzy.
4. **Orin Nano on the real bus.** Pinmux, TCAN1044AV and 500 kbps; choose the Orin as leader with two or three nodes; dashboard Overview and Nodes pages working.
5. **RPi joins.** Find the MCP2515 crystal, set the matching `oscillator` value, confirm 500 kbps. Run full traffic with the **RPi leading** and check the overrun counter.
6. **Leadership.** Hand the lead Orin → RPi → Orin while driving on blocks; kill each leader's gateway; unplug each host; power-cycle both in different orders.
7. **E-stops.** Physical button, wireless transmitter button, wireless transmitter switched off, and wireless out of range; each must cut motor power and show the right source on the dashboard.
8. **Bootloader and OTA.** Normal flash; flash interrupted halfway; flash with the wrong node type; rescue a node stuck in the bootloader; ESP32-S3 rollback after a deliberately crashing image.
9. **Error reporting.** Force a HardFault on an STM32 node and check the dashboard shows the right source file and line; pull an encoder cable and check ENCODER\_NO\_COUNTS; generate an incident report and replay it.
10. **Full rover, motors loaded.** Watch error counters and error frames while stalling motors and driving over rough ground.

**Test checklist**

- [ ] CANH–CANL measures about 60 Ω with everything off
- [ ] Every device runs at 500 kbps with a sample point between 75% and 81%
- [ ] Every TCAN1044AV has STB low, VIO at 3.3 V and VCC at 5 V
- [ ] Bus load under 50% at full rate (`canbusload`)
- [ ] RPi shows zero overruns over 10 minutes of full traffic while leading
- [ ] With both hosts up, nothing moves until the operator picks a leader
- [ ] Handing the lead to the other host while driving stops the rover; nothing moves until re-armed
- [ ] Killing the leader's gateway stops all motors within 100 ms and raises LEADER\_LOST within 250 ms; nothing moves until the operator chooses and re-arms
- [ ] During every lead change, the raw log shows commands from only one host at a time
- [ ] Unplugging any one node leaves the rest running and appears on the dashboard within 300 ms
- [ ] ESTOP from each possible sender stops every node; release needs operator action
- [ ] The physical and the wireless E-stop each cut motor power with all software still running, and the dashboard names the source
- [ ] Shorting CANH to CANL disarms every node; the bus recovers when the short is removed
- [ ] An interrupted STM32 flash leaves the node in the bootloader, recoverable from the dashboard
- [ ] A crashing ESP32-S3 image rolls back to the previous one by itself
- [ ] A forced HardFault produces a crash record decoded to the correct source line
- [ ] A node with an older protocol version refuses to arm
- [ ] Zero error frames over 30 minutes of driving with motors loaded

* [ ] Each node boots at its `debug_default` level; levels 2–3 switch off by themselves when the timer runs out
* [ ] The gateway refuses a debug level or rate change that would push the bus over 50%
* [ ] Releasing the dashboard's drive key, closing the tab, or dropping Wi-Fi stops the rover within 200 ms
* [ ] The frame lab refuses node-owned IDs and refuses command frames while armed
* [ ] Everything works with the Orin on Humble and the RPi on Jazzy, and the other way round

**Assumptions this design is built on**

| Item | Assumption | If it turns out different |
| --- | --- | --- |
| RPi MCP2515 crystal | Unknown; designed for the worst case (8 MHz) | 16 MHz allows a later move to 1 Mbps (section 4) |
| STM32 parts | Unknown; firmware supports both bxCAN and FDCAN | Pick the bit-timing row and flash layout once known |
| Transceivers | TCAN1044AV on nodes, Orin Nano and FPGA; SN65HVD230 on the RPi | Any ISO 11898-2 transceiver works; check its logic voltage |
| Orin | Orin Nano, one CAN controller | A second bus would need a USB-CAN adapter |
| FPGA | Tang Nano 20K with an open-source CAN core | No impact on the rest of the design |
| ESP32-S3 flash | Unknown; partition plan fits 4 MB | Larger flash simply gets larger app slots |
| Node count | 6 drive nodes, FPGA and some ESP32-S3 nodes now; arm, science and power boards later | Use the per-node cost table in section 7 |
| Leadership | Option A: operator chooses; RPi can take the lead any time; no default; auto-takeover off | Turn on `auto_takeover` per host if wanted |
| ROS 2 | Each host runs Humble or Jazzy, independently and in any combination | No CAN changes needed |
| E-stop | Physical and wireless, both in the hardware contactor loop | Remove whichever isn't required |
| Drive motors | Under 51 A per channel and under 327 rad/s, the limits of the current message fields | Rescale the fields in the DBC |

**Sources**

- [NVIDIA Jetson Orin CAN developer guide](https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/HR/ControllerAreaNetworkCan.html)
- [TI TCAN1044V/AV datasheet, operating modes (STB pin)](https://www.ti.com/document-viewer/TCAN1044V/datasheet/operating-modes-t4977274-18)
- [TI TCAN1044AV product page](https://www.ti.com/product/TCAN1044AV-Q1)
- [Waveshare 2-CH CAN HAT+ (MCP2515 + SN65HVD230)](https://www.waveshare.com/product/iot-communication/wired-comm-converter/2-ch-can-hat-plus.htm)
- [candleLight firmware for USB-CAN adapters (gs\_usb)](https://github.com/candle-usb/candleLight_fw)
- [Katapult CAN bootloader](https://github.com/Arksine/katapult)
- [Lichtblick](https://github.com/lichtblick-suite/lichtblick)
- Original v1 architecture report (team Google Doc)
