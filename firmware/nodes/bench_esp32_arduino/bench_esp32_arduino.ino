// Anveshak bench node for WROOM (ESP32) and ESP32-S3 boards, Arduino-ESP32 core 3.3+.
//
// Every board runs this same sketch, built on the v2 libraries in firmware/lib: anv_link (TWAI,
// 29-bit IDs), anv_proto and anv_node (the node runtime). The node type is `esp32_bench`
// (schema/types/esp32_bench.yaml): a `status` stream with four numbers about the board and a
// `blink` action. A board with no ID asks the hosts for one (id_request with its MAC), keeps the
// answer in NVS and then runs as a normal node: heartbeat, settings, stream rates, arm/disarm,
// E-stop and leader tracking all come from anv_node. See README.md.
//
// src/schema/ is a copy of the generated code made by sync_schema.sh: edit schema/, not it.
#include <Arduino.h>

#include "anv_link.h"
#include "anv_link_twai.h"
#include "anv_node.h"
#include "anv_node_esp32.h"
#include "anv_node_ota_esp32.h"

#include "board_pins.h"
#include "led.h"
#include "src/schema/anv_esp32_bench.h"
#include "user_app.h"

// ---------------- configuration ----------------
constexpr uint32_t kIdentifyMs = 5000;  // NMT identify: blink this long
constexpr size_t kTextMaxLen = 7;       // characters in one protocol `text` frame

// ---------------- state ----------------
static anv_twai_t s_twai;
static anv_link_t s_link;
static anv_node_t s_node;
static anv_node_nvs_t s_nvs;
static bool s_can_ok = false;
static bool s_diag = true;
static uint32_t s_blink_until = 0;
static uint8_t s_text_seq = 0;

// A build ID that changes with every compile, for the info page (the hosts show it).
static uint32_t build_id() {
  uint32_t h = 2166136261u;
  for (const char* p = __DATE__ " " __TIME__; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  return h;
}

static void print_mac(const uint8_t* m) {
  Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---------------- node hooks ----------------
// The bench board drives nothing, so there is nothing to switch off. A board with actuators
// stops them here: the runtime calls this at start and every time the node leaves ARMED.
static void hook_outputs_off(anv_node_t*, void*) {}

static void hook_setup(anv_node_t*, void*) { user_setup(); }

static void hook_loop(anv_node_t*, uint32_t now_ms, void*) { user_loop(now_ms); }

// The `status` stream, at whatever rate a host set (10 Hz by default).
static bool hook_fill_stream(anv_node_t*, const anv_msg_desc_t* msg, uint8_t out[8], void*) {
  if (msg->port != ANV_ESP32_BENCH_STATUS_PORT) return false;
  anv_esp32_bench_status_t m;
  m.chip_temp = anv_esp32_bench_status_chip_temp_from_phys(temperatureRead());
  m.free_heap = static_cast<uint16_t>(ESP.getFreeHeap() / 1024u);
  m.rx_frames = static_cast<uint16_t>(anv_link_stats(&s_link)->rx_frames);
  m.tx_frames = static_cast<uint16_t>(s_twai.tx_ok);
  anv_esp32_bench_status_pack(out, &m);
  return true;
}

static uint8_t hook_action(anv_node_t*, const anv_action_desc_t* action, anv_value_t arg, void*) {
  if (action->index != ANV_ESP32_BENCH_ACTION_BLINK) return ANV_SERVICE_STATUS_BAD_INDEX;
  if (arg.u < 1 || arg.u > 60) return ANV_SERVICE_STATUS_OUT_OF_RANGE;  // the schema's limits
  s_blink_until = millis() + arg.u * 1000u;
  Serial.printf("Blinking for %lu s\n", static_cast<unsigned long>(arg.u));
  return ANV_SERVICE_STATUS_OK;
}

static void hook_identify(anv_node_t*, void*) {
  s_blink_until = millis() + kIdentifyMs;
  Serial.println("Identify: blinking");
}

static void hook_state_changed(anv_node_t* node, uint8_t from, uint8_t to, void*) {
  Serial.printf("State %s -> %s", anv_node_state_name(from), anv_node_state_name(to));
  if (anv_node_id(node) != ANV_NODE_UNASSIGNED) Serial.printf(" (node %u)", anv_node_id(node));
  Serial.println();
}

static bool send_text(anv_node_id_t dst, uint8_t seq, const char* text, size_t len) {
  anv_text_t m = {};
  m.seq = seq;
  memcpy(m.chars, text, min(len, kTextMaxLen));
  uint8_t data[8];
  const anv_proto_addr_t addr = {ANV_TEXT_PRIORITY, dst, ANV_TEXT_PORT, anv_node_id(&s_node)};
  anv_link_frame_t f;
  return anv_proto_frame(&f, &addr, data, anv_text_pack(data, &m)) && anv_node_send_frame(&s_node, &f);
}

// The raw-frame hook: text from a host (the touch screen's Messages view) is printed and echoed
// back with the same seq, so the screen can tick it as received. The runtime has no use for
// `text` frames, which is why they arrive here.
static void hook_frame(anv_node_t* node, const anv_link_frame_t* f, void*) {
  if (anv_id_port(f->id) != ANV_TEXT_PORT) return;
  anv_text_t m;
  if (!anv_text_unpack(&m, f->data, f->len)) return;
  char text[kTextMaxLen + 1] = {0};
  size_t len = 0;
  while (len < kTextMaxLen && m.chars[len] != 0) {
    text[len] = (m.chars[len] >= 0x20 && m.chars[len] < 0x7F) ? static_cast<char>(m.chars[len]) : '?';
    ++len;
  }
  const bool to_all = anv_id_dst(f->id) == ANV_NODE_BROADCAST;
  Serial.printf("Message from %u%s: %s\n", anv_id_src(f->id), to_all ? " (to everyone)" : "", text);
  if (len > 0 && anv_node_id(node) != ANV_NODE_UNASSIGNED) send_text(anv_id_src(f->id), m.seq, text, len);
}

// ---------------- serial console ----------------
static void print_status() {
  twai_node_status_t st;
  twai_node_record_t rec;
  Serial.printf("[%lu] ", static_cast<unsigned long>(millis()));
  const anv_node_id_t id = anv_node_id(&s_node);
  if (id == ANV_NODE_UNASSIGNED) {
    Serial.print("waiting for node ID");
  } else {
    Serial.printf("node=%u %s", id, anv_node_state_name(anv_node_state(&s_node)));
  }
  if (anv_node_has_leader(&s_node)) Serial.printf(" leader=%u", anv_node_leader(&s_node));
  if (!s_can_ok || !anv_twai_get_info(&s_twai, &st, &rec)) {
    Serial.println("  TWAI not running");
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
  const anv_link_stats_t* ls = anv_link_stats(&s_link);
  Serial.printf("  bus=%s tec=%u rec=%u buserr=%lu  tx_ok=%lu rx=%lu rx_drop=%lu\n", bus,
                static_cast<unsigned>(st.tx_error_count), static_cast<unsigned>(st.rx_error_count),
                static_cast<unsigned long>(rec.bus_err_num), static_cast<unsigned long>(s_twai.tx_ok),
                static_cast<unsigned long>(ls->rx_frames), static_cast<unsigned long>(ls->rx_overflow));
}

static void handle_command(char* line) {
  unsigned id = 0;
  if (strcmp(line, "help") == 0) {
    Serial.println("commands: diag (toggle the 1 s status line), status, id, setid <1-254>, forget, "
                   "msg <text>, estop");
  } else if (strcmp(line, "diag") == 0) {
    s_diag = !s_diag;
    Serial.printf("periodic status %s\n", s_diag ? "on" : "off");
  } else if (strcmp(line, "status") == 0) {
    print_status();
  } else if (strcmp(line, "id") == 0) {
    Serial.print("MAC ");
    print_mac(s_node.cfg.mac);
    if (anv_node_id(&s_node) == ANV_NODE_UNASSIGNED) {
      Serial.print("  no node ID yet");
    } else {
      Serial.printf("  node ID %u%s", anv_node_id(&s_node), s_node.id_saved ? " (saved)" : "");
    }
    Serial.printf("  type %s  build %08lx\n", anv_esp32_bench_type.name, static_cast<unsigned long>(build_id()));
  } else if (sscanf(line, "setid %u", &id) == 1) {
    Serial.println(anv_node_save_id(&s_node, static_cast<anv_node_id_t>(id)) ? "Node ID saved"
                                                                               : "Refused (1-254, and not while armed)");
  } else if (strcmp(line, "forget") == 0) {
    Serial.println(anv_node_forget_id(&s_node) ? "Node ID erased; asking the hosts for a new one"
                                               : "Refused while armed");
  } else if (strncmp(line, "msg ", 4) == 0) {
    if (anv_node_id(&s_node) == ANV_NODE_UNASSIGNED) {
      Serial.println("No node ID yet: messages go out once a host has given this board an ID");
    } else {
      const size_t len = strlen(line + 4);
      if (len > kTextMaxLen) Serial.printf("Only the first %u characters fit in one frame\n", unsigned(kTextMaxLen));
      Serial.println(send_text(ANV_NODE_BROADCAST, ++s_text_seq, line + 4, len) ? "Message sent" : "Send queue full");
    }
  } else if (strcmp(line, "estop") == 0) {
    anv_node_estop(&s_node, 0);
    Serial.println("E-stop sent; every node is latched until the leader releases it");
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

  anv_node_config_t cfg;
  anv_node_config_default(&cfg);
  cfg.type = &anv_esp32_bench_type;
  cfg.link = &s_link;
  anv_node_nvs_init(&s_nvs, "anv_node");
  cfg.storage = anv_node_nvs_ops();
  cfg.storage_ctx = &s_nvs;
  anv_node_esp32_identity(&cfg, &s_nvs);
  cfg.info.fw_build = build_id();
  cfg.ota = anv_node_ota_esp32_backend();  // flashing over CAN: schema's `transfer` port, service `ota`
  cfg.save_assigned_id = true;  // a board keeps its ID across reboots; `forget` asks again
  cfg.outputs_off = hook_outputs_off;
  cfg.setup = hook_setup;
  cfg.loop = hook_loop;
  cfg.fill_stream = hook_fill_stream;
  cfg.on_action = hook_action;
  cfg.on_identify = hook_identify;
  cfg.on_reset = anv_node_esp32_restart;
  cfg.on_state_changed = hook_state_changed;
  cfg.on_frame = hook_frame;

  Serial.println();
  Serial.print("=== Anveshak bench node (anv_node), MAC ");
  print_mac(cfg.mac);
  Serial.printf(", build %08lx, TX %d RX %d ===\n", static_cast<unsigned long>(build_id()), BENCH_CAN_TX_GPIO,
                BENCH_CAN_RX_GPIO);

  anv_twai_setup(&s_twai, BENCH_CAN_TX_GPIO, BENCH_CAN_RX_GPIO);
  anv_link_init(&s_link, anv_twai_ops(), &s_twai);
  anv_link_config_t link_cfg;
  anv_link_config_default(&link_cfg);  // 500 kbit/s; anv_node sets the filters
  s_can_ok = anv_link_start(&s_link, &link_cfg) == ANV_LINK_OK;
  if (!s_can_ok) Serial.println("TWAI start FAILED: check the pins in board_pins.h");

  if (!anv_node_init(&s_node, &cfg)) {
    Serial.println("anv_node_init FAILED");  // only if the type outgrew the tables
  }
  anv_node_begin(&s_node, millis());
  if (anv_node_id(&s_node) == ANV_NODE_UNASSIGNED) {
    Serial.println("No node ID saved: asking the hosts for one");
  } else {
    Serial.printf("Node ID %u (saved). Type 'forget' to ask for a new one.\n", anv_node_id(&s_node));
  }
}

void loop() {
  const uint32_t now = millis();
  anv_node_poll(&s_node, now);

  if (!user_owns_led()) {
    LedMode mode;
    if (!s_can_ok || anv_link_stats(&s_link)->state == ANV_LINK_STATE_BUS_OFF) {
      mode = LedMode::BusOff;
    } else if ((int32_t)(now - s_blink_until) < 0) {
      mode = LedMode::Identify;
    } else if (anv_node_id(&s_node) == ANV_NODE_UNASSIGNED) {
      mode = LedMode::WaitingForId;
    } else {
      mode = LedMode::Running;
    }
    led_update(now, mode, s_twai.tx_ok);
  }

  static uint32_t last_diag = 0;
  if (s_diag && now - last_diag >= 1000) {
    last_diag = now;
    print_status();
  }
  poll_serial();
  delay(1);  // lets the idle task run; nothing in the node runtime waits
}
