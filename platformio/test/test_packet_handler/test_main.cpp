// ---
// description: Unit tests for PacketHandler's sensor_flags gating (hold-last-good substitution of invalid-sensor placeholders) and bundle encoding.
// role: test
// ---
#include <unity.h>

#include "radio/PacketHandler.h"
#include "telemetry/BinaryPacket.h"
#include "telemetry/SensorSnapshot.h"

#include <string.h>

// Bundles in these tests use maxDeltas=2 (ref + 2 deltas = 3 pushes per bundle)
// to keep scenarios short. Wire layout under test:
//   [PktHeader:5][FullStatePayload:20][n_deltas:1][DeltaPayload x n:12][crc8:1]

namespace {

constexpr uint8_t kMaxDeltas = 2;

SensorSnapshot makeSnap(uint32_t sessionMs, uint16_t flags,
                        float tempC, float humidityPct,
                        float windMps = 1.0f, float pm = 2.0f) {
  SensorSnapshot s;  // placeholders default to -1.0f
  s.sessionTimeMs = sessionMs;
  s.sensorFlags   = flags;
  if (flags & PacketHandler::SHT31_FLAG) {
    s.tempC       = tempC;
    s.humidityPct = humidityPct;
  }
  if (flags & PacketHandler::WIND_FLAG) {
    s.windMps = windMps;
  }
  if (flags & PacketHandler::SPS30_FLAG) {
    s.pm1_0 = pm;
    s.pm2_5 = pm;
    s.pm4_0 = pm;
    s.pm10  = pm;
  }
  return s;
}

struct DecodedBundle {
  BinaryPacket::FullStatePayload ref;
  uint8_t deltaCount;
  BinaryPacket::DeltaPayload deltas[BinaryPacket::kBundleMaxDeltas];
};

DecodedBundle decodeBundle(const uint8_t *buf) {
  DecodedBundle d = {};
  size_t off = sizeof(BinaryPacket::PktHeader);
  memcpy(&d.ref, buf + off, sizeof(d.ref));
  off += sizeof(d.ref);
  d.deltaCount = buf[off++];
  for (uint8_t i = 0; i < d.deltaCount; ++i) {
    memcpy(&d.deltas[i], buf + off, sizeof(d.deltas[i]));
    off += sizeof(d.deltas[i]);
  }
  return d;
}

// Pushes snaps until a bundle is ready, then takes and decodes it.
DecodedBundle pushBundle(PacketHandler &ph, const SensorSnapshot *snaps, size_t n) {
  uint8_t buf[BinaryPacket::kMaxBundleLoRaSize] = {};
  for (size_t i = 0; i < n; ++i) {
    ph.push(snaps[i]);
  }
  TEST_ASSERT_TRUE(ph.bundleReady());
  TEST_ASSERT_GREATER_THAN_UINT8(0, ph.takeBundle(buf, sizeof(buf)));
  return decodeBundle(buf);
}

// Takes the ready bundle and returns its PktHeader::flags byte, checking the
// frame's CRC still covers the header (flushWindow() may have edited it in
// place after encoding).
uint8_t takeBundleFlags(PacketHandler &ph) {
  uint8_t buf[BinaryPacket::kMaxBundleLoRaSize] = {};
  const uint8_t len = ph.takeBundle(buf, sizeof(buf));
  TEST_ASSERT_GREATER_THAN_UINT8(0, len);
  TEST_ASSERT_EQUAL_UINT8(buf[len - 1], BinaryPacket::crc8(buf, len - 1));

  BinaryPacket::PktHeader hdr = {};
  memcpy(&hdr, buf, sizeof(hdr));
  return hdr.flags;
}

// Pushes `n` identical samples, taking any bundle that completes.
void pushSamples(PacketHandler &ph, uint32_t startMs, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    ph.push(makeSnap(startMs + 1000u * static_cast<uint32_t>(i),
                     PacketHandler::SHT31_FLAG, 25.0f, 45.0f));
  }
}

constexpr uint16_t kAllFlags =
    PacketHandler::WIND_FLAG | PacketHandler::SHT31_FLAG | PacketHandler::SPS30_FLAG;

} // namespace

// -----------------------------------------------------------------------------
// Placeholder gating — reference sample
// -----------------------------------------------------------------------------

void test_invalid_sht31_on_first_ever_sample_encodes_zero_not_placeholder(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  SensorSnapshot snaps[3] = {
      makeSnap(1000, kAllFlags & ~PacketHandler::SHT31_FLAG, 0, 0),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  const DecodedBundle b = pushBundle(ph, snaps, 3);

  // No last-good yet: encode 0, never the -1.0f placeholder
  // (-100 centi-degC / 65436 centi-%RH after the unsigned wrap).
  TEST_ASSERT_EQUAL_INT16(0, b.ref.temp_cdegc);
  TEST_ASSERT_EQUAL_UINT16(0, b.ref.humidity_cpct);
}

void test_invalid_reference_holds_last_good_and_deltas_do_not_clamp(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  // Bundle 1: all valid at 21.50 C — establishes last-good.
  SensorSnapshot b1[3] = {
      makeSnap(1000, kAllFlags, 21.50f, 50.0f),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  pushBundle(ph, b1, 3);

  // Bundle 2: SHT31 fails exactly on the reference, then recovers.
  // Pre-fix this produced ref=-1.0 C and deltas clamped at +12.7 (ghost 11.7 C).
  SensorSnapshot b2[3] = {
      makeSnap(3250, kAllFlags & ~PacketHandler::SHT31_FLAG, 0, 0),
      makeSnap(4000, kAllFlags, 21.25f, 50.0f),
      makeSnap(4750, kAllFlags, 21.75f, 50.0f),
  };
  const DecodedBundle b = pushBundle(ph, b2, 3);

  TEST_ASSERT_EQUAL_INT16(2150, b.ref.temp_cdegc);  // held last-good
  TEST_ASSERT_EQUAL_INT8(-3, b.deltas[0].temp_delta_deci_c);  // 21.25 vs 21.50
  TEST_ASSERT_EQUAL_INT8(3, b.deltas[1].temp_delta_deci_c);   // 21.75 vs 21.50
  TEST_ASSERT_EQUAL_UINT8(
      0, b.deltas[0].flags & BinaryPacket::DELTA_FLAG_TEMP_CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(
      0, b.deltas[1].flags & BinaryPacket::DELTA_FLAG_TEMP_CLAMPED);
}

// -----------------------------------------------------------------------------
// Placeholder gating — mid-batch delta sample
// -----------------------------------------------------------------------------

void test_invalid_midbatch_sample_holds_last_good_delta_zero(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  SensorSnapshot snaps[3] = {
      makeSnap(1000, kAllFlags, 21.50f, 50.0f),
      makeSnap(1750, kAllFlags & ~PacketHandler::SHT31_FLAG, 0, 0),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  const DecodedBundle b = pushBundle(ph, snaps, 3);

  TEST_ASSERT_EQUAL_INT16(2150, b.ref.temp_cdegc);
  TEST_ASSERT_EQUAL_INT8(0, b.deltas[0].temp_delta_deci_c);
  TEST_ASSERT_EQUAL_INT8(0, b.deltas[0].humidity_delta_0p2pct);
  TEST_ASSERT_EQUAL_UINT8(
      0, b.deltas[0].flags & (BinaryPacket::DELTA_FLAG_TEMP_CLAMPED |
                              BinaryPacket::DELTA_FLAG_HUMID_CLAMPED));
}

// -----------------------------------------------------------------------------
// Gating covers wind and SPS30 groups too
// -----------------------------------------------------------------------------

void test_invalid_wind_and_sps30_hold_last_good(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  SensorSnapshot b1[3] = {
      makeSnap(1000, kAllFlags, 21.50f, 50.0f, 3.0f, 8.0f),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f, 3.0f, 8.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f, 3.0f, 8.0f),
  };
  pushBundle(ph, b1, 3);

  SensorSnapshot b2[3] = {
      makeSnap(3250, PacketHandler::SHT31_FLAG, 21.50f, 50.0f),  // wind+PM invalid
      makeSnap(4000, kAllFlags, 21.50f, 50.0f, 3.0f, 8.0f),
      makeSnap(4750, kAllFlags, 21.50f, 50.0f, 3.0f, 8.0f),
  };
  const DecodedBundle b = pushBundle(ph, b2, 3);

  TEST_ASSERT_EQUAL_UINT16(300, b.ref.wind_cms);   // held 3.0 m/s
  TEST_ASSERT_EQUAL_UINT16(80, b.ref.pm2_5_ug10);  // held 8.0 ug/m3
}

// -----------------------------------------------------------------------------
// Gating must not rewrite the wire validity flags
// -----------------------------------------------------------------------------

void test_sensor_flags_on_wire_preserve_invalidity(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  const uint16_t refFlags = kAllFlags & ~PacketHandler::SHT31_FLAG;
  SensorSnapshot snaps[3] = {
      makeSnap(1000, refFlags, 0, 0),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  const DecodedBundle b = pushBundle(ph, snaps, 3);

  // Consumers must still see that the reference's SHT31 reading was invalid.
  TEST_ASSERT_EQUAL_UINT16(refFlags, b.ref.sensor_flags);
}

// -----------------------------------------------------------------------------
// reset() clears the last-good cache
// -----------------------------------------------------------------------------

void test_reset_clears_last_good(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  SensorSnapshot b1[3] = {
      makeSnap(1000, kAllFlags, 21.50f, 50.0f),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  pushBundle(ph, b1, 3);

  ph.reset();

  SensorSnapshot b2[3] = {
      makeSnap(1000, kAllFlags & ~PacketHandler::SHT31_FLAG, 0, 0),
      makeSnap(1750, kAllFlags, 21.50f, 50.0f),
      makeSnap(2500, kAllFlags, 21.50f, 50.0f),
  };
  const DecodedBundle b = pushBundle(ph, b2, 3);

  TEST_ASSERT_EQUAL_INT16(0, b.ref.temp_cdegc);  // stale last-good discarded
}

// -----------------------------------------------------------------------------
// Valid readings pass through unchanged
// -----------------------------------------------------------------------------

void test_valid_readings_encode_unchanged(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  SensorSnapshot snaps[3] = {
      makeSnap(1000, kAllFlags, 21.50f, 57.25f, 0.0f, 1.5f),
      makeSnap(1750, kAllFlags, 21.75f, 57.75f, 0.0f, 1.5f),
      makeSnap(2500, kAllFlags, 21.25f, 56.75f, 0.0f, 1.5f),
  };
  const DecodedBundle b = pushBundle(ph, snaps, 3);

  TEST_ASSERT_EQUAL_INT16(2150, b.ref.temp_cdegc);
  TEST_ASSERT_EQUAL_UINT16(5725, b.ref.humidity_cpct);
  TEST_ASSERT_EQUAL_UINT16(15, b.ref.pm2_5_ug10);
  TEST_ASSERT_EQUAL_INT8(3, b.deltas[0].temp_delta_deci_c);
  TEST_ASSERT_EQUAL_INT8(-3, b.deltas[1].temp_delta_deci_c);
}

// -----------------------------------------------------------------------------
// Timed duty-cycle window markers (PktHeader::flags)
// -----------------------------------------------------------------------------

void test_window_first_marks_only_the_first_bundle_of_the_window(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();

  pushSamples(ph, 1000, 3);  // ref + 2 deltas completes bundle 1
  TEST_ASSERT_TRUE(ph.bundleReady());
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_WINDOW_FIRST, takeBundleFlags(ph));

  pushSamples(ph, 5000, 3);  // bundle 2 of the same window
  TEST_ASSERT_TRUE(ph.bundleReady());
  TEST_ASSERT_EQUAL_UINT8(0, takeBundleFlags(ph));
}

void test_flush_window_emits_partial_bundle_marked_last(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();
  pushSamples(ph, 1000, 3);
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_WINDOW_FIRST, takeBundleFlags(ph));

  // Two samples accumulated — short of a full bundle, so without the flush they
  // would sit in the accumulator across the MCU standby.
  pushSamples(ph, 5000, 2);
  TEST_ASSERT_FALSE(ph.bundleReady());

  TEST_ASSERT_TRUE(ph.flushWindow());
  TEST_ASSERT_TRUE(ph.bundleReady());
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_WINDOW_LAST, takeBundleFlags(ph));
}

void test_single_bundle_window_carries_both_markers(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();
  pushSamples(ph, 1000, 2);  // ref + 1 delta — no bundle yet

  TEST_ASSERT_TRUE(ph.flushWindow());
  TEST_ASSERT_EQUAL_UINT8(
      BinaryPacket::PKT_FLAG_WINDOW_FIRST | BinaryPacket::PKT_FLAG_WINDOW_LAST,
      takeBundleFlags(ph));
}

void test_flush_stamps_a_completed_but_untaken_bundle_in_place(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();
  pushSamples(ph, 1000, 3);  // bundle completed, deliberately not taken
  TEST_ASSERT_TRUE(ph.bundleReady());

  TEST_ASSERT_TRUE(ph.flushWindow());

  // Marked in place rather than overwritten — takeBundleFlags() also asserts the
  // CRC was recomputed, since a stale CRC would be dropped by the base.
  TEST_ASSERT_EQUAL_UINT8(
      BinaryPacket::PKT_FLAG_WINDOW_FIRST | BinaryPacket::PKT_FLAG_WINDOW_LAST,
      takeBundleFlags(ph));
}

void test_flush_with_nothing_accumulated_emits_no_bundle(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();
  pushSamples(ph, 1000, 3);
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_WINDOW_FIRST, takeBundleFlags(ph));

  // The window's samples landed exactly on a bundle boundary.
  TEST_ASSERT_FALSE(ph.flushWindow());
  TEST_ASSERT_FALSE(ph.bundleReady());
}

void test_window_first_survives_until_a_bundle_actually_carries_it(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();

  // Nothing pushed yet, so the marker is still pending; the next window opening
  // must not double-count it into a second bundle.
  TEST_ASSERT_FALSE(ph.flushWindow());

  pushSamples(ph, 1000, 3);
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_WINDOW_FIRST, takeBundleFlags(ph));

  pushSamples(ph, 5000, 3);
  TEST_ASSERT_EQUAL_UINT8(0, takeBundleFlags(ph));
}

void test_reset_clears_pending_window_marker(void) {
  PacketHandler ph(PacketHandler::Config::make(2, kMaxDeltas));

  ph.beginWindow();
  ph.reset();

  pushSamples(ph, 1000, 3);
  TEST_ASSERT_EQUAL_UINT8(0, takeBundleFlags(ph));
}

void runPacketHandlerTests(void) {
  UNITY_BEGIN();

  RUN_TEST(test_invalid_sht31_on_first_ever_sample_encodes_zero_not_placeholder);
  RUN_TEST(test_invalid_reference_holds_last_good_and_deltas_do_not_clamp);
  RUN_TEST(test_invalid_midbatch_sample_holds_last_good_delta_zero);
  RUN_TEST(test_invalid_wind_and_sps30_hold_last_good);
  RUN_TEST(test_sensor_flags_on_wire_preserve_invalidity);
  RUN_TEST(test_reset_clears_last_good);
  RUN_TEST(test_valid_readings_encode_unchanged);

  RUN_TEST(test_window_first_marks_only_the_first_bundle_of_the_window);
  RUN_TEST(test_flush_window_emits_partial_bundle_marked_last);
  RUN_TEST(test_single_bundle_window_carries_both_markers);
  RUN_TEST(test_flush_stamps_a_completed_but_untaken_bundle_in_place);
  RUN_TEST(test_flush_with_nothing_accumulated_emits_no_bundle);
  RUN_TEST(test_window_first_survives_until_a_bundle_actually_carries_it);
  RUN_TEST(test_reset_clears_pending_window_marker);

  UNITY_END();
}

#ifdef ARDUINO

void setup() {
  delay(2000);
  runPacketHandlerTests();
}

void loop() {}

#else

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runPacketHandlerTests();

  return 0;
}

#endif
