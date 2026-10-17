#include "ota_can.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"

#include "board_pins.h"
#include "src/anv/ids.h"

// Arduino's core marks a new image valid at start-up unless this returns true; the sketch confirms
// it later instead, once the image has shown it can run (ota_confirm).
extern "C" bool verifyRollbackLater() { return true; }

namespace {

// --- Update commands carried over ISO-TP (first payload byte; a reply echoes it | 0x40) ---
enum Cmd : uint8_t { INFO = 0x01, BEGIN = 0x02, WRITE = 0x03, VERIFY = 0x04, COMMIT = 0x05, BOOT = 0x06, ABORT = 0x07 };
enum Status : uint8_t {
  OK = 0,
  BAD_STATE = 1,     // e.g. WRITE without BEGIN
  BAD_ARG = 2,       // wrong length or offset
  WRONG_TARGET = 3,  // image for the other chip
  TOO_BIG = 4,
  FLASH_ERROR = 5,
  CRC_MISMATCH = 6,
  ARMED = 7,  // never while the node is armed
  BAD_IMAGE = 8,
  UNKNOWN_CMD = 9,
};
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kSubHostToNode = 0, kSubNodeToHost = 1;

// --- ISO-TP (ISO 15765-2), receive side; replies always fit one single frame ---
constexpr size_t kMaxMessage = 2048 + 16;  // a WRITE block plus its header
constexpr uint8_t kFcBlockSize = 0;        // all consecutive frames after one flow control
constexpr uint8_t kFcStMin = 0xF5;         // >= 500 us between frames: the loop keeps up and the bus keeps room
constexpr uint32_t kCfTimeoutMs = 1000;    // N_Cr
constexpr uint32_t kSessionTimeoutMs = 30000;

uint8_t s_node_id = 0;
OtaHooks s_hooks{};

uint8_t s_rx[kMaxMessage];
size_t s_rx_len = 0, s_rx_got = 0;
uint8_t s_rx_next_sn = 0;
bool s_rx_active = false;
uint32_t s_rx_last_ms = 0;

// --- Update session ---
const esp_partition_t* s_target = nullptr;
esp_ota_handle_t s_handle = 0;
bool s_session = false;
uint32_t s_size = 0, s_expected_crc = 0, s_written = 0, s_crc = 0;
uint32_t s_last_cmd_ms = 0;
uint32_t s_restart_at = 0;
bool s_verified = false;

bool s_rolled_back = false;
bool s_confirmed = false;

// Plain CRC-32 (zlib / IEEE), so the host's zlib.crc32() and this agree by construction.
uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint32_t get_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
void put_u32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

void send_raw(const uint8_t* data, uint8_t len) {
  acc_can_frame_t f;
  f.id = ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, s_node_id, kSubNodeToHost);
  f.dlc = 8;
  memset(f.data, 0xAA, sizeof(f.data));  // ISO-TP padding
  memcpy(f.data, data, len);
  s_hooks.send(f);
}

// A reply: one ISO-TP single frame of up to 7 bytes.
void reply(uint8_t cmd, uint8_t status, const uint8_t* extra = nullptr, uint8_t extra_len = 0) {
  uint8_t d[8];
  const uint8_t len = 2 + extra_len;
  d[0] = len;  // single-frame PCI: type 0, length
  d[1] = cmd | 0x40;
  d[2] = status;
  if (extra_len) memcpy(d + 3, extra, extra_len);
  send_raw(d, 1 + len);
}

void end_session() {
  if (s_session && !s_verified) esp_ota_abort(s_handle);
  s_session = false;
  s_verified = false;
  s_handle = 0;
}

void handle_message(const uint8_t* m, size_t len, uint32_t now_ms) {
  if (len == 0) return;
  const uint8_t cmd = m[0];
  s_last_cmd_ms = now_ms;
  switch (cmd) {
    case INFO: {
      const esp_partition_t* running = esp_ota_get_running_partition();
      const esp_partition_t* spare = esp_ota_get_next_update_partition(nullptr);
      const uint16_t slot_kb = spare ? spare->size / 1024 : 0;
      const uint8_t extra[5] = {BENCH_HW_TARGET, kProtocolVersion,
                                uint8_t(running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? 1 : 0),
                                uint8_t(slot_kb & 0xFF), uint8_t(slot_kb >> 8)};
      reply(cmd, OK, extra, sizeof extra);
      return;
    }
    case BEGIN: {
      if (len != 10) return reply(cmd, BAD_ARG);
      if (s_hooks.armed()) return reply(cmd, ARMED);
      end_session();
      const uint32_t size = get_u32(m + 1);
      if (m[9] != BENCH_HW_TARGET) return reply(cmd, WRONG_TARGET);
      s_target = esp_ota_get_next_update_partition(nullptr);
      if (!s_target) return reply(cmd, FLASH_ERROR);
      if (size == 0 || size > s_target->size) return reply(cmd, TOO_BIG);
      // Sequential writes erase sector by sector as data arrives, instead of a multi-second erase now.
      if (esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK) return reply(cmd, FLASH_ERROR);
      s_session = true;
      s_size = size;
      s_expected_crc = get_u32(m + 5);
      s_written = 0;
      s_crc = 0;
      Serial.printf("Update: receiving %lu bytes into %s\n", (unsigned long)size, s_target->label);
      return reply(cmd, OK);
    }
    case WRITE: {
      if (!s_session || s_verified) return reply(cmd, BAD_STATE);
      if (s_hooks.armed()) {
        end_session();
        return reply(cmd, ARMED);
      }
      if (len < 6) return reply(cmd, BAD_ARG);
      const uint32_t offset = get_u32(m + 1);
      const uint8_t* data = m + 5;
      const size_t n = len - 5;
      uint8_t next[4];
      if (offset != s_written || s_written + n > s_size) {
        put_u32(next, s_written);  // tells the host where to resume
        return reply(cmd, BAD_ARG, next, 4);
      }
      if (offset == 0) {
        // The first block carries the image header: refuse anything that isn't an app for this chip.
        const esp_image_header_t* h = reinterpret_cast<const esp_image_header_t*>(data);
        if (n < sizeof(esp_image_header_t) || h->magic != ESP_IMAGE_HEADER_MAGIC) {
          end_session();
          return reply(cmd, BAD_IMAGE);
        }
        if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
          end_session();
          return reply(cmd, WRONG_TARGET);
        }
      }
      if (esp_ota_write(s_handle, data, n) != ESP_OK) {
        end_session();
        return reply(cmd, FLASH_ERROR);
      }
      s_crc = crc32_update(s_crc, data, n);
      s_written += n;
      put_u32(next, s_written);
      return reply(cmd, OK, next, 4);
    }
    case VERIFY: {
      if (!s_session || s_verified) return reply(cmd, BAD_STATE);
      if (s_written != s_size || s_crc != s_expected_crc) {
        end_session();
        return reply(cmd, CRC_MISMATCH);
      }
      const esp_err_t err = esp_ota_end(s_handle);  // also checks the image's own structure and hash
      s_verified = true;
      if (err != ESP_OK) {
        s_session = false;
        s_verified = false;
        return reply(cmd, err == ESP_ERR_OTA_VALIDATE_FAILED ? BAD_IMAGE : FLASH_ERROR);
      }
      return reply(cmd, OK);
    }
    case COMMIT: {
      if (!s_session || !s_verified) return reply(cmd, BAD_STATE);
      if (esp_ota_set_boot_partition(s_target) != ESP_OK) return reply(cmd, FLASH_ERROR);
      // Remember which slot should come up, to tell a good boot from a rollback afterwards.
      Preferences p;
      p.begin("anveshak_ota", false);
      p.putString("slot", s_target->label);
      p.end();
      Serial.printf("Update: %s will boot next\n", s_target->label);
      return reply(cmd, OK);
    }
    case BOOT:
      reply(cmd, OK);
      s_restart_at = now_ms + 200;  // let the reply leave first
      return;
    case ABORT:
      end_session();
      Serial.println("Update: aborted by the host");
      return reply(cmd, OK);
    default:
      return reply(cmd, UNKNOWN_CMD);
  }
}

}  // namespace

void ota_init(uint8_t node_id, const OtaHooks& hooks) {
  s_node_id = node_id;
  s_hooks = hooks;
}

bool ota_on_frame(const acc_can_frame_t& f, uint32_t now_ms) {
  if (f.id != ROVER_CAN_ID(ROVER_CLASS_BOOTLOADER, s_node_id, kSubHostToNode) || f.dlc < 1) return false;
  const uint8_t pci = f.data[0] >> 4;
  switch (pci) {
    case 0: {  // single frame
      const uint8_t len = f.data[0] & 0x0F;
      if (len >= 1 && len <= 7 && len < f.dlc) handle_message(f.data + 1, len, now_ms);
      s_rx_active = false;
      break;
    }
    case 1: {  // first frame: answer with flow control, then collect the consecutive frames
      const size_t len = ((f.data[0] & 0x0F) << 8) | f.data[1];
      if (f.dlc < 8 || len <= 7) break;
      if (len > sizeof s_rx) {
        const uint8_t overflow[3] = {0x32, 0, 0};
        send_raw(overflow, 3);
        s_rx_active = false;
        break;
      }
      memcpy(s_rx, f.data + 2, 6);
      s_rx_len = len;
      s_rx_got = 6;
      s_rx_next_sn = 1;
      s_rx_active = true;
      s_rx_last_ms = now_ms;
      const uint8_t cts[3] = {0x30, kFcBlockSize, kFcStMin};
      send_raw(cts, 3);
      break;
    }
    case 2: {  // consecutive frame
      if (!s_rx_active) break;
      if ((f.data[0] & 0x0F) != s_rx_next_sn) {
        s_rx_active = false;  // lost a frame: the host times out and resends the block
        break;
      }
      const size_t n = min<size_t>(7, s_rx_len - s_rx_got);
      memcpy(s_rx + s_rx_got, f.data + 1, n);
      s_rx_got += n;
      s_rx_next_sn = (s_rx_next_sn + 1) & 0x0F;
      s_rx_last_ms = now_ms;
      if (s_rx_got >= s_rx_len) {
        s_rx_active = false;
        handle_message(s_rx, s_rx_len, now_ms);
      }
      break;
    }
    default:
      break;
  }
  return true;
}

void ota_tick(uint32_t now_ms) {
  if (s_rx_active && now_ms - s_rx_last_ms > kCfTimeoutMs) s_rx_active = false;
  if (s_session && now_ms - s_last_cmd_ms > kSessionTimeoutMs) {
    Serial.println("Update: host went quiet, abandoning the transfer");
    end_session();
  }
  if (s_restart_at && (int32_t)(now_ms - s_restart_at) >= 0) {
    Serial.println("Update: restarting into the new image");
    Serial.flush();
    ESP.restart();
  }
}

bool ota_busy() { return s_session || s_rx_active; }

void ota_check_last_update() {
  Preferences p;
  p.begin("anveshak_ota", false);
  const String expected = p.getString("slot", "");
  if (expected.length()) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && expected == running->label) {
      Serial.printf("Update: now running the new image from %s\n", running->label);
    } else {
      s_rolled_back = true;
      Serial.printf("Update: the new image in %s failed; rolled back to %s\n", expected.c_str(),
                    running ? running->label : "?");
    }
    p.remove("slot");
  }
  p.end();
}

bool ota_rolled_back() { return s_rolled_back; }

void ota_confirm(uint32_t now_ms, bool host_heard) {
  if (s_confirmed || !host_heard || now_ms < 5000) return;
  s_confirmed = true;
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("Update: new image confirmed (no rollback)");
  }
}

uint16_t ota_fw_build_id() {
  const uint8_t* sha = esp_app_get_description()->app_elf_sha256;
  return static_cast<uint16_t>(sha[0] | (sha[1] << 8));
}
