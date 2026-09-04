// ---
// description: Reset-cause + hang-zone diagnostics. Harvests PM->RCAUSE and a
//   .noinit RAM breadcrumb across warm resets so a watchdog reboot can be
//   attributed to the blocking region (radio TX / I2C sensor / SPS30 UART) the
//   firmware hung in, and reports both through the AWAKEN payload.
// role: interface
// ---
#pragma once

#include <stdint.h>

namespace ResetDiagnostics {

// Blocking regions worth distinguishing when the watchdog fires. Ordering is
// wire-visible (carried in AwakenPayload.hang_zone) — append, never renumber.
enum HangZone : uint8_t {
  ZONE_UNKNOWN = 0,   // breadcrumb invalid, or WDT fired outside any marked region
  ZONE_BOOT,          // setup()/begin() phase, before loop() is entered
  ZONE_RADIO_TX,      // RadioHeadTdmaDriver send / waitPacketSent / ACK-wait paths
  ZONE_I2C_SHT31,     // AdafruitSht31Driver bus transactions
  ZONE_I2C_GPS,       // AdafruitGpsDriver (PA1010D) bus transactions
  ZONE_I2C_IMU,       // SparkfunIcm20948Driver (ICM-20948 / DMP) bus transactions
  ZONE_UART_SPS30,    // SensirionUartSps30Driver read/write waits
  ZONE_LOOP_IDLE,     // steady-state loop(), outside any marked blocking region
};

// Boot-time harvest: reads the .noinit breadcrumb left by the previous run,
// derives hang_zone (only trusted on a WDT reset with an intact breadcrumb),
// then re-initializes the breadcrumb for this run (zone = ZONE_BOOT,
// boot_count++). Call once, as early in setup() as possible — right after
// reading PM->RCAUSE, before anything can hang.
void harvest(uint8_t reset_cause);

// Write the current-zone breadcrumb (a single byte store plus its inverse for a
// cheap integrity check). Safe and cheap enough to call on hot paths.
void markZone(HangZone zone);
HangZone currentZone();

// Values captured by harvest(), for reporting through the AWAKEN payload.
uint8_t  resetCause();        // raw PM->RCAUSE from this boot
uint8_t  hangZone();          // HangZone at the prior WDT hang, else ZONE_UNKNOWN
uint16_t bootCount();         // increments across warm resets (0 on cold boot -> 1)
bool     wasWatchdogReset();  // RCAUSE had the WDT bit set

// RAII helper: mark `zone` on entry, restore the enclosing zone on exit.
// Restoring the previous zone (rather than clearing to a fixed value) keeps
// ZONE_BOOT during setup() and ZONE_LOOP_IDLE during loop() as the "both prime
// suspects innocent" fallbacks when a hang happens in unmarked code.
struct ZoneScope {
  HangZone prev;
  explicit ZoneScope(HangZone zone) : prev(currentZone()) { markZone(zone); }
  ~ZoneScope() { markZone(prev); }
  ZoneScope(const ZoneScope&) = delete;
  ZoneScope& operator=(const ZoneScope&) = delete;
};

} // namespace ResetDiagnostics
