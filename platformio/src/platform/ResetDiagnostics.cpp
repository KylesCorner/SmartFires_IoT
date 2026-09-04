// ---
// description: Implements ResetDiagnostics — the .noinit breadcrumb, boot-time
//   harvest of RCAUSE + prior hang zone, and the markZone/accessor API.
// role: implementation
// ---
#include "platform/ResetDiagnostics.h"

namespace {

// Validity guard for the breadcrumb — RAM is garbage on a cold power-up, so a
// matching magic (plus the inverted-zone check) is what tells harvest() the
// breadcrumb actually survived a warm reset rather than being random SRAM.
constexpr uint32_t kBreadcrumbMagic = 0x5A1F0DE5u;

// PM->RCAUSE watchdog bit on the SAMD21 (PM_RCAUSE_WDT). Hard-coded so this TU
// stays free of the CMSIS device header and compiles unchanged for native tests.
constexpr uint8_t kRCauseWdtMask = 0x20u;

struct ResetBreadcrumb {
  uint32_t magic;
  uint8_t  zone;
  uint8_t  zone_inv;   // ~zone, cheap corruption check
  uint16_t boot_count;
};

// Must survive a warm (WDT/system) reset but be treated as garbage after a cold
// power-up. On the node build this lives in a NOLOAD .noinit section (see
// ldscripts/flash_with_bootloader_noinit.ld) that the C runtime does not zero.
// On the base build and native tests there is no such section, so it is an
// ordinary zero-initialized global — which simply makes every harvest report
// ZONE_UNKNOWN (the base never sends AWAKEN, so nothing depends on retention).
// volatile so each markZone() store is committed to RAM promptly — the WDT
// reset is asynchronous, and the breadcrumb is only useful if it reflects the
// last region entered before the hang.
#if defined(SMARTFIRES_RESET_DIAG)
__attribute__((section(".noinit"))) volatile ResetBreadcrumb g_breadcrumb;
#else
volatile ResetBreadcrumb g_breadcrumb;
#endif

uint8_t  g_resetCause = 0;
uint8_t  g_hangZone   = ResetDiagnostics::ZONE_UNKNOWN;
uint16_t g_bootCount  = 0;
bool     g_wdtReset   = false;

inline bool breadcrumbValid() {
  return g_breadcrumb.magic == kBreadcrumbMagic &&
         static_cast<uint8_t>(~g_breadcrumb.zone) == g_breadcrumb.zone_inv;
}

} // namespace

namespace ResetDiagnostics {

void harvest(uint8_t reset_cause) {
  g_resetCause = reset_cause;
  g_wdtReset   = (reset_cause & kRCauseWdtMask) != 0u;

  const bool valid = breadcrumbValid();

  // A hang zone is only meaningful on a WDT reset with an intact breadcrumb. On
  // a brownout/power-on the retained zone is deliberately discarded — a reset
  // caught mid-I2C-read is not a hang.
  g_hangZone = (g_wdtReset && valid) ? static_cast<uint8_t>(g_breadcrumb.zone)
                                     : static_cast<uint8_t>(ZONE_UNKNOWN);

  const uint16_t priorCount = valid ? g_breadcrumb.boot_count : 0u;
  g_bootCount = static_cast<uint16_t>(priorCount + 1u);

  // Re-arm the breadcrumb for this run: valid magic, this boot's count, and
  // ZONE_BOOT until the first instrumented region overwrites it.
  g_breadcrumb.magic      = kBreadcrumbMagic;
  g_breadcrumb.boot_count = g_bootCount;
  markZone(ZONE_BOOT);
}

void markZone(HangZone zone) {
  g_breadcrumb.zone     = static_cast<uint8_t>(zone);
  g_breadcrumb.zone_inv = static_cast<uint8_t>(~static_cast<uint8_t>(zone));
}

HangZone currentZone() {
  return static_cast<HangZone>(g_breadcrumb.zone);
}

uint8_t  resetCause()       { return g_resetCause; }
uint8_t  hangZone()         { return g_hangZone; }
uint16_t bootCount()        { return g_bootCount; }
bool     wasWatchdogReset() { return g_wdtReset; }

} // namespace ResetDiagnostics
