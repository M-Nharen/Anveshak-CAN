// A temperature-probe node on anv_node.
#include <Arduino.h>

#include "anv_board_esp32.h"
#include "src/schema/anv_temp_sensor.h"

constexpr int PROBE_PIN = 34;   // analog input (WROOM; on an S3 use e.g. 1)

static anv_board_t s_board;
static float s_temp_c = 0;
static bool s_alarm = false;

// Replace with your real sensor. Here: an LM35-style probe, 10 mV per degC.
static float read_probe_c() { return analogReadMilliVolts(PROBE_PIN) / 10.0f; }

// Nothing to switch off on a sensor, but the hook is required.
static void hook_outputs_off(anv_node_t*, void*) {}

// The `reading` stream: called at whatever rate a host set (5 Hz by default).
static bool hook_fill_stream(anv_node_t*, const anv_msg_desc_t* msg, uint8_t out[8], void*) {
  if (msg->port != ANV_TEMP_SENSOR_READING_PORT) return false;
  anv_temp_sensor_reading_t r = {};
  r.temp = anv_temp_sensor_reading_temp_from_phys(s_temp_c);
  r.alarm = s_alarm;
  anv_temp_sensor_reading_pack(out, &r);
  return true;
}

static uint8_t hook_action(anv_node_t*, const anv_action_desc_t* a, anv_value_t, void*) {
  if (a->index != ANV_TEMP_SENSOR_ACTION_SELF_TEST) return ANV_SERVICE_STATUS_BAD_INDEX;
  const float t = read_probe_c();
  return (t > -40 && t < 150) ? ANV_SERVICE_STATUS_OK : ANV_SERVICE_STATUS_FAILED;
}

// Runs every poll. Read the probe at 10 Hz, apply the settings, raise or clear the alarm.
static void hook_loop(anv_node_t* node, uint32_t now_ms, void*) {
  static uint32_t next_ms = 0;
  if ((int32_t)(now_ms - next_ms) < 0) return;  // never block: just come back later
  next_ms = now_ms + 100;

  anv_value_t offset, alarm_c;
  anv_node_get(node, ANV_TEMP_SENSOR_PARAM_OFFSET_C, &offset);   // live settings
  anv_node_get(node, ANV_TEMP_SENSOR_PARAM_ALARM_C, &alarm_c);
  s_temp_c = read_probe_c() + offset.f;

  const bool alarm = s_temp_c > alarm_c.f;
  if (alarm && !s_alarm) anv_node_event(node, ANV_TEMP_SENSOR_EVENT_OVERHEAT, (uint16_t)s_temp_c);
  if (!alarm && s_alarm) anv_node_event_clear(node, ANV_TEMP_SENSOR_EVENT_OVERHEAT, (uint16_t)s_temp_c);
  s_alarm = alarm;
}

void setup() {
  Serial.begin(115200);

  anv_node_config_t cfg;
  anv_node_config_default(&cfg);
  cfg.type = &anv_temp_sensor_type;
  cfg.outputs_off = hook_outputs_off;
  cfg.fill_stream = hook_fill_stream;
  cfg.on_action = hook_action;
  cfg.loop = hook_loop;

  if (!anv_board_begin(&s_board, &cfg, NULL)) {
    Serial.printf("temp_sensor node: %s\n", anv_board_error(&s_board));
  }
}

void loop() {
  anv_board_poll(&s_board);
  delay(1);
}
