// Anveshak bench node for WROOM (ESP32) and ESP32-S3 boards, Arduino-ESP32 core 3.3+.
//
// Every board runs this same sketch. A board with no node ID saved asks the RPi for one over CAN
// (NODE_ID_REQUEST with its MAC), saves the answer and restarts as a normal rover node: heartbeat
// at 10 Hz, a 4-value sensor frame at 10 Hz, E-stop / NMT / leader tracking from anveshak_can_core.
// See README.md and docs/architecture.md "Automatic node IDs".
//
// src/anv/ is a copy of firmware/core and firmware/generated made by sync_core.sh: edit those, not it.
#include <Arduino.h>
#include <Preferences.h>

#include "esp_mac.h"

#include "board_pins.h"
#include "led.h"
#include "ota_can.h"
#include "twai_arduino_driver.h"

#include "src/anv/anv_node.h"
#include "src/anv/ids.h"
#include "src/anv/rover.h"

// ---------------- configuration ----------------
constexpr uint32_t kBitrate = 500000;
constexpr uint8_t kNodeType = 3;  // NODE_HB NODE_TYPE 3 = science / auxiliary
constexpr uint8_t kProtocolVersion = 1;
constexpr uint32_t kHbRateHz = 10;
constexpr uint32_t kSensorRateHz = 10;
constexpr size_t kTxqCapPerPrio = 8;
constexpr uint16_t kRawTextId = 0x7FF;  // serial "raw" text frames: lowest priority, unused by the DBC
// Bench text (docs/architecture.md, TEXT_TO_NODE / TEXT_FROM_NODE): class 7, sub 2/3/4 = from the
// Orin / RPi / tool, to this node or to node 0 (everyone); sub 5 = this node's replies.
constexpr uint8_t kTextSubFromHostFirst = 2, kTextSubFromHostLast = 4, kTextSubFromNode = 5;
constexpr size_t kTextMaxLen = 7;
constexpr uint32_t kRequestFirstMinMs = 100, kRequestFirstSpanMs = 500;  // first request jitter
constexpr uint32_t kRequestEveryMs = 400, kRequestSpanMs = 200;          // then every 400-600 ms


// ---------------- state ----------------
enum class Mode : uint8_t { WaitingForId, Running };

static twai_ard_ctx_t s_can_ctx;
static acc_can_driver_t s_can;
static anv_node_t s_node;
static acc_can_frame_t s_txq_storage[ACC_CAN_PRIO_COUNT * kTxqCapPerPrio];
static anv_param_store_t s_params;  // empty: no tunable parameters on the bench node
static anv_rate_sched_t s_sensor_sched;
static Preferences s_prefs;

static Mode s_mode = Mode::WaitingForId;
static uint8_t s_node_id = ROVER_NODE_UNCONFIGURED;
static uint64_t s_mac = 0;
static uint32_t s_next_request_ms = 0;
static uint32_t s_rx_frames = 0;
static bool s_diag = true;
static bool s_host_heard = false;  // any host heartbeat since boot (lets a new image confirm itself)
static bool s_rollback_reported = false;

// ---------------- board hooks for anveshak_can_core ----------------
static uint32_t hook_now_ms(void*) { return millis(); }
static void hook_outputs_off(void*) {}  // no actuators on the bench node
static void hook_feed_watchdog(void*) {}
static void hook_reset(void*) { ESP.restart(); }  // NMT RESET from the dashboard

static const anv_board_hooks_t s_hooks = {
    .ctx = nullptr,
    .read_node_id = nullptr,  // the ID comes from NVS, not a switch
    .set_outputs_off = hook_outputs_off,
    .feed_hw_watchdog = hook_feed_watchdog,
    .now_ms = hook_now_ms,
    .enter_bootloader = nullptr,
    .request_reset = hook_reset,
    .storage_load = nullptr,
    .storage_save = nullptr,
};

// ---------------- helpers ----------------
static uint64_t read_mac() {
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  uint64_t v = 0;
  for (uint8_t b : m) v = (v << 8) | b;  // m[0] is the first byte as printed: aa:bb:...
  return v;
}

static void print_mac(uint64_t mac) {
  Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x", (unsigned)(mac >> 40) & 0xFF, (unsigned)(mac >> 32) & 0xFF,
                (unsigned)(mac >> 24) & 0xFF, (unsigned)(mac >> 16) & 0xFF, (unsigned)(mac >> 8) & 0xFF,
                (unsigned)mac & 0xFF);
}

// IDs a board may take: auxiliary 16-30 plus drive/arm IDs for later reuse; never hosts or 31.
static bool valid_node_id(uint8_t id) { return id >= ROVER_NODE_DRIVE1 && id < ROVER_NODE_UNCONFIGURED; }

static bool bus_off() {
  acc_can_error_state_t st;
  return s_can.get_error_state(s_can.ctx, &st) == ACC_CAN_OK && st.state == ACC_CAN_BUS_OFF;
}

static const char* state_name(anv_state_t st) {
  switch (st) {
    case ANV_STATE_BOOT: return "BOOT";
    case ANV_STATE_DISARMED: return "DISARMED";
    case ANV_STATE_ARMED: return "ARMED";
    case ANV_STATE_FAULT: return "FAULT";
    case ANV_STATE_ESTOP: return "ESTOP";
    case ANV_STATE_BOOTLOADER: return "BOOTLOADER";
  }
  return "?";
}

// ---------------- waiting for an ID ----------------
static void send_id_request() {
  rover_node_id_request_t req;
  rover_node_id_request_init(&req);
  req.mac = s_mac;
  req.node_type = kNodeType;
  req.hw_target = BENCH_HW_TARGET;
  acc_can_frame_t f;
  f.id = ROVER_NODE_ID_REQUEST_FRAME_ID;
  const int n = rover_node_id_request_pack(f.data, &req, sizeof(f.data));
  if (n < 0) return;
  f.dlc = static_cast<uint8_t>(n);
  (void)s_can.send(s_can.ctx, &f, ACC_CAN_PRIO_SERVICE);
}

// Any host's NODE_ID_ASSIGN: same layout on all three IDs, so one unpacker reads them all.
static void on_frame_while_waiting(const acc_can_frame_t& f) {
  if (f.id != ROVER_NODE_ID_ASSIGN_ORIN_FRAME_ID && f.id != ROVER_NODE_ID_ASSIGN_RPI_FRAME_ID &&
      f.id != ROVER_NODE_ID_ASSIGN_TOOL_FRAME_ID)
    return;
  rover_node_id_assign_rpi_t a;
  if (rover_node_id_assign_rpi_unpack(&a, f.data, f.dlc) != 0 || a.mac != s_mac) return;
  if (!valid_node_id(a.assigned_id)) {
    Serial.printf("Host offered invalid node ID %u, ignoring\n", a.assigned_id);
    return;
  }
  s_prefs.putUChar("node_id", a.assigned_id);
  Serial.printf("Assigned node ID %u by host %u; restarting as that node\n", a.assigned_id,
                (unsigned)ROVER_ID_SUB(f.id) + 1u);
  Serial.flush();
  // Restart rather than switch over live: one boot path, and the node starts with a clean state.
  ESP.restart();
}

// ---------------- running as a node ----------------
// Update replies go out right away rather than waiting for the next loop's drain.
static bool ota_send(const acc_can_frame_t& f) {
  const bool queued = anv_node_send(&s_node, ACC_CAN_PRIO_SERVICE, &f);
  (void)anv_node_drain_tx(&s_node);
  return queued;
}
static bool ota_armed() { return anv_node_state(&s_node) == ANV_STATE_ARMED; }

static void start_node(uint8_t node_id) {
  const uint32_t now = millis();
  anv_param_store_init(&s_params, nullptr, nullptr, 0, &s_hooks);
  anv_node_info_t info = {};
  info.git_build_id = ota_fw_build_id();
  info.mcu_unique_id = static_cast<uint32_t>(s_mac);
  anv_node_init(&s_node, node_id, kNodeType, kProtocolVersion, &s_can, s_txq_storage, kTxqCapPerPrio, &s_params,
                &info, &s_hooks, now);
  anv_node_boot_complete(&s_node);
  anv_node_set_hb_rate(&s_node, kHbRateHz, now);
  anv_rate_sched_init(&s_sensor_sched, kSensorRateHz, now);
  ota_init(node_id, OtaHooks{ota_send, ota_armed});
  s_mode = Mode::Running;
}

// Four live values for the dashboard tile: chip temperature (0.01 degC), free heap (KiB), and
// the low 16 bits of the frames received / sent. Replace with real sensors on a science board.
static void send_sensor_frame() {
  rover_fpga_sensors_a_t m;
  rover_fpga_sensors_a_init(&m);
  m.sensor_1 = static_cast<uint16_t>(constrain(temperatureRead() * 100.0f, 0.0f, 65535.0f));
  m.sensor_2 = static_cast<uint16_t>(ESP.getFreeHeap() / 1024u);
  m.sensor_3 = static_cast<uint16_t>(s_rx_frames);
  m.sensor_4 = static_cast<uint16_t>(s_can_ctx.tx_ok_count);
  acc_can_frame_t f;
  f.id = ROVER_CAN_ID(ROVER_CLASS_SENSOR, s_node_id, 0u);  // generic 4x u16 layout at this node's ID
  const int n = rover_fpga_sensors_a_pack(f.data, &m, sizeof(f.data));
  if (n < 0) return;
  f.dlc = static_cast<uint8_t>(n);
  (void)anv_node_send(&s_node, ACC_CAN_PRIO_TELEMETRY, &f);
}

// ---------------- bench text ----------------
static const char* host_name(uint8_t sub) {
  switch (sub) {
    case 2: return "Orin";
    case 3: return "RPi";
    default: return "tool";
  }
}

// seq = the host text being echoed back, or 0 for text typed on this board's serial console.
static bool send_text(uint8_t seq, const char* text, size_t len) {
  rover_text_from_node_t m;
  rover_text_from_node_init(&m);
  uint8_t c[kTextMaxLen] = {0};
  memcpy(c, text, min(len, kTextMaxLen));
  m.seq = seq;
  m.c0 = c[0]; m.c1 = c[1]; m.c2 = c[2]; m.c3 = c[3]; m.c4 = c[4]; m.c5 = c[5]; m.c6 = c[6];
  acc_can_frame_t f;
  f.id = ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, s_node_id, kTextSubFromNode);
  const int n = rover_text_from_node_pack(f.data, &m, sizeof(f.data));
  if (n < 0) return false;
  f.dlc = static_cast<uint8_t>(n);
  return anv_node_send(&s_node, ACC_CAN_PRIO_SERVICE, &f);
}

// A host's text for this node (or for everyone): print it and echo it back, so the screen shows the
// node really received it. Returns false for frames that aren't host text to us.
static bool on_text_frame(const acc_can_frame_t& f) {
  const uint8_t node = ROVER_ID_NODE(f.id), sub = ROVER_ID_SUB(f.id);
  if (ROVER_ID_CLASS(f.id) != ROVER_CLASS_BOOTLOADER || sub < kTextSubFromHostFirst || sub > kTextSubFromHostLast ||
      (node != s_node_id && node != ROVER_NODE_BROADCAST))
    return false;
  rover_text_to_node_rpi_t m;  // same layout from every host
  if (rover_text_to_node_rpi_unpack(&m, f.data, f.dlc) != 0) return true;
  const uint8_t c[kTextMaxLen] = {m.c0, m.c1, m.c2, m.c3, m.c4, m.c5, m.c6};
  char text[kTextMaxLen + 1] = {0};
  size_t len = 0;
  while (len < kTextMaxLen && c[len] != 0) {
    text[len] = (c[len] >= 0x20 && c[len] < 0x7F) ? static_cast<char>(c[len]) : '?';
    ++len;
  }
  Serial.printf("Message from %s%s: %s\n", host_name(sub), node == ROVER_NODE_BROADCAST ? " (to everyone)" : "", text);
  if (len > 0) send_text(m.seq, text, len);
  return true;
}

// ---------------- serial console ----------------
static void print_status() {
  twai_node_status_t st;
  twai_node_record_t rec;
  if (!twai_ard_get_info(&s_can_ctx, &st, &rec)) {
    Serial.println("TWAI not running");
    return;
  }
  const char* bus;
  switch (st.state) {
    case TWAI_ERROR_ACTIVE: bus = "ACTIVE"; break;
    case TWAI_ERROR_WARNING: bus = "WARNING"; break;
    case TWAI_ERROR_PASSIVE: bus = "PASSIVE"; break;
    case TWAI_ERROR_BUS_OFF: bus = "BUS-OFF"; break;
    default: bus = "UNKNOWN"; break;
  }
  Serial.printf("[%lu] ", (unsigned long)millis());
  if (s_mode == Mode::Running) {
    Serial.printf("node=%u %s master=%u", s_node_id, state_name(anv_node_state(&s_node)), s_node.master.master_node_id);
  } else {
    Serial.print("waiting for node ID");
  }
  Serial.printf("  bus=%s tec=%u rec=%u buserr=%lu  tx_ok=%lu rx=%lu rx_drop=%lu\n", bus, (unsigned)st.tx_error_count,
                (unsigned)st.rx_error_count, (unsigned long)rec.bus_err_num, (unsigned long)s_can_ctx.tx_ok_count,
                (unsigned long)s_rx_frames, (unsigned long)s_can_ctx.rx_dropped);
}

static void send_raw_text(const char* text) {
  acc_can_frame_t f;
  f.id = kRawTextId;
  f.dlc = static_cast<uint8_t>(min<size_t>(strlen(text), 8));
  memcpy(f.data, text, f.dlc);
  const int r = s_can.send(s_can.ctx, &f, ACC_CAN_PRIO_SERVICE);
  Serial.println(r == ACC_CAN_OK ? "Sent!" : "Send failed (queue full or bus-off)");
}

static void handle_command(char* line) {
  if (strcmp(line, "help") == 0) {
    Serial.println("commands: diag (toggle the 1 s status line), status, id, forget, msg <text>, raw <text>");
  } else if (strcmp(line, "diag") == 0) {
    s_diag = !s_diag;
    Serial.printf("periodic status %s\n", s_diag ? "on" : "off");
  } else if (strcmp(line, "status") == 0) {
    print_status();
  } else if (strcmp(line, "id") == 0) {
    Serial.print("MAC ");
    print_mac(s_mac);
    if (s_mode == Mode::Running) {
      Serial.printf("  node ID %u  build %04x\n", s_node_id, ota_fw_build_id());
    } else {
      Serial.printf("  no node ID yet  build %04x\n", ota_fw_build_id());
    }
  } else if (strcmp(line, "forget") == 0) {
    s_prefs.remove("node_id");
    Serial.println("Node ID erased; restarting to ask for a new one");
    Serial.flush();
    ESP.restart();
  } else if (strncmp(line, "msg ", 4) == 0) {
    if (s_mode != Mode::Running) {
      Serial.println("No node ID yet: messages go out once the RPi has given this board an ID");
    } else {
      const size_t len = strlen(line + 4);
      if (len > kTextMaxLen) Serial.printf("Only the first %u characters fit in one frame\n", (unsigned)kTextMaxLen);
      Serial.println(send_text(0, line + 4, len) ? "Message queued for the screen" : "Send queue full");
    }
  } else if (strncmp(line, "raw ", 4) == 0) {
    send_raw_text(line + 4);
  } else {
    Serial.printf("unknown command '%s' (try help)\n", line);
  }
}

// Non-blocking line reader: readStringUntil() would stall the CAN loop for up to a second.
static void poll_serial() {
  static char line[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        line[len] = '\0';
        handle_command(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
#if BENCH_TCAN_STB_GPIO >= 0
  pinMode(BENCH_TCAN_STB_GPIO, OUTPUT);
  digitalWrite(BENCH_TCAN_STB_GPIO, LOW);  // TCAN normal mode
#endif
  led_init();
  delay(500);  // let a freshly opened serial monitor catch the boot lines

  s_mac = read_mac();
  s_prefs.begin("anveshak", false);
  const uint8_t stored = s_prefs.getUChar("node_id", 0);

  Serial.println();
  Serial.print("=== Anveshak bench node, MAC ");
  print_mac(s_mac);
  Serial.printf(", build %04x, TX %d RX %d ===\n", ota_fw_build_id(), BENCH_CAN_TX_GPIO, BENCH_CAN_RX_GPIO);
  ota_check_last_update();

  s_can = twai_ard_make_driver(&s_can_ctx, BENCH_CAN_TX_GPIO, BENCH_CAN_RX_GPIO);
  if (s_can.init(s_can.ctx, kBitrate, nullptr, 0) != ACC_CAN_OK) {
    Serial.println("TWAI init FAILED: check the pins in board_pins.h");
    for (;;) {
      led_update(millis(), LedMode::BusOff, 0);
      delay(100);
    }
  }

  if (valid_node_id(stored)) {
    s_node_id = stored;
    Serial.printf("Node ID %u (saved). Type 'forget' to ask for a new one.\n", s_node_id);
    start_node(s_node_id);
  } else {
    randomSeed(static_cast<uint32_t>(s_mac ^ (s_mac >> 32)));  // boards powered together still spread out
    s_next_request_ms = millis() + kRequestFirstMinMs + random(kRequestFirstSpanMs);
    Serial.println("No node ID saved: asking the hosts for one");
  }
}

void loop() {
  const uint32_t now = millis();

  acc_can_frame_t f;
  while (s_can.receive(s_can.ctx, &f)) {
    ++s_rx_frames;
    if (f.id == kRawTextId) {
      Serial.printf("Received raw text: %.*s\n", f.dlc, reinterpret_cast<const char*>(f.data));
    } else if (s_mode == Mode::Running) {
      if (f.id == ROVER_HOST_HB_ORIN_FRAME_ID || f.id == ROVER_HOST_HB_RPI_FRAME_ID || f.id == ROVER_HOST_HB_TOOL_FRAME_ID)
        s_host_heard = true;
      if (!ota_on_frame(f, now) && !on_text_frame(f)) anv_node_on_rx_frame(&s_node, &f, now);
    } else {
      on_frame_while_waiting(f);
    }
  }

  if (s_mode == Mode::Running) {
    anv_node_tick(&s_node, now);  // heartbeat, command watchdog, bus-off recovery
    if (anv_rate_sched_due(&s_sensor_sched, now)) send_sensor_frame();
    ota_tick(now);
    ota_confirm(now, s_host_heard);
    if (ota_rolled_back() && s_host_heard && !s_rollback_reported) {
      s_rollback_reported = send_text(0, "rollbck", 7);  // shows on the screen: the last update failed
    }
    (void)anv_node_drain_tx(&s_node);
  } else {
    if (bus_off()) s_can.start_recovery(s_can.ctx);
    if ((int32_t)(now - s_next_request_ms) >= 0) {
      send_id_request();
      s_next_request_ms = now + kRequestEveryMs + random(kRequestSpanMs);
    }
  }

  const bool off = bus_off();
  led_update(now, off ? LedMode::BusOff : (s_mode == Mode::Running ? LedMode::Running : LedMode::WaitingForId),
             s_can_ctx.tx_ok_count);

  static uint32_t last_diag = 0;
  if (s_diag && now - last_diag >= 1000) {
    last_diag = now;
    print_status();
  }
  poll_serial();
  delay(1);
}
