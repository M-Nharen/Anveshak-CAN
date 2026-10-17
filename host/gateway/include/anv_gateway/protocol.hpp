// Protocol constants shared by the gateway modules. Message layouts are NOT here: those come
// from the generated code in firmware/generated (defs/rover.dbc is the single source).
// Only timing rules and enums from docs/architecture.md live in this file.
#pragma once

#include <cstdint>

#include "anv_gateway/time.hpp"

namespace anv::gw {

// --- Timing rules (docs/architecture.md "Hosts: choosing which one leads", "Host software") ---
constexpr int64_t kCyclePeriodNs = 20 * kNsPerMs;        // SYNC + DRIVE_CMDs, 50 Hz
constexpr int64_t kHostHbPeriodNs = 50 * kNsPerMs;       // HOST_HB, 20 Hz
constexpr int64_t kTargetStaleNs = 100 * kNsPerMs;       // older targets -> zero speed
constexpr int64_t kLeaderLostNs = 250 * kNsPerMs;        // 5 missed master heartbeats
constexpr int64_t kNodeMissingNs = 300 * kNsPerMs;       // node silent this long -> missing
constexpr int64_t kLateCycleNs = 2 * kNsPerMs;           // cycle started later than this is "late"
constexpr int64_t kLeadRequestDedupNs = 500 * kNsPerMs;  // identical (sender, counter) repeats
constexpr int64_t kSchedulerTickNs = 10 * kNsPerMs;      // base tick; cycle = 2 ticks, HB = 5

constexpr uint8_t kNumDrives = 6;

// --- Node IDs (generated ids.h has the same numbers; these are the C++-typed copies) ---
constexpr uint8_t kNodeOrin = 1;
constexpr uint8_t kNodeRpi = 2;
constexpr uint8_t kNodeTool = 3;
constexpr uint8_t kNodeFirstDrive = 4;  // drive n has node ID 3 + n
constexpr uint8_t kNodeFpga = 20;
constexpr uint8_t kNodePower = 21;          // reserved: power distribution / battery monitor
constexpr uint8_t kFirstAuxNode = 16;       // science / auxiliary nodes and spares
constexpr uint8_t kLastAuxNode = 30;
constexpr uint8_t kNodeUnconfigured = 31;  // a board with no ID yet

// Class 7 sub-types of the bench text frames (0 and 1 are the bootloader transfer).
constexpr uint8_t kTextSubFromHostBase = 1;  // + host node: 2 Orin, 3 RPi, 4 tool
constexpr uint8_t kTextSubFromNode = 5;

// HW_TARGET in NODE_ID_REQUEST
constexpr uint8_t kHwTargetEsp32 = 0;
constexpr uint8_t kHwTargetEsp32S3 = 1;
constexpr uint8_t kHwTargetUnknown = 0xFF;

constexpr uint8_t drive_node_id(uint8_t drive) { return static_cast<uint8_t>(3 + drive); }

// --- Enums on the wire ---
enum class Role : uint8_t { Booting = 0, Standby = 1, Master = 2, Observer = 3 };

enum class NodeState : uint8_t { Boot = 0, Disarmed = 1, Armed = 2, Fault = 3, Estop = 4, Bootloader = 5 };

enum class NmtCommand : uint8_t { Arm = 1, Disarm = 2, Reset = 3, EnterBootloader = 4, FaultReset = 5 };

enum class DriveMode : uint8_t { Coast = 0, Brake = 1, Velocity = 2, OpenLoopDuty = 3 };

// HOST_HB flags byte
constexpr uint8_t kHbFlagAutonomyReady = 1u << 0;
constexpr uint8_t kHbFlagOperatorLink = 1u << 1;
constexpr uint8_t kHbFlagEstopLatched = 1u << 2;
constexpr uint8_t kHbFlagArmed = 1u << 3;

constexpr uint8_t kEstopMagic = 0xE5;

// Wire scaling for DRIVE_CMD targets: 0.01 rad/s (or 0.01 % duty) per count.
constexpr double kTargetScale = 100.0;

// Serial-number comparison for u16 terms, so a wrap after 65535 handovers still orders correctly.
inline bool term_gt(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }
inline bool term_ge(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) >= 0; }
inline uint16_t term_max(uint16_t a, uint16_t b) { return term_gt(a, b) ? a : b; }

}  // namespace anv::gw
