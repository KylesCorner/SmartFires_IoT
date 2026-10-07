#include <unity.h>

#include "config/NetworkProfiles.h"
#include "radio/LoRaAirtime.h"
#include "telemetry/BinaryPacket.h"

void setUp() {}
void tearDown() {}

namespace {

void test_all_profiles_are_valid() {
  TEST_ASSERT_TRUE(NetworkProfiles::isValid(NetworkProfiles::kSf7));
  TEST_ASSERT_TRUE(NetworkProfiles::isValid(NetworkProfiles::kSf9));
  TEST_ASSERT_TRUE(NetworkProfiles::isValid(NetworkProfiles::kSf10));
  TEST_ASSERT_TRUE(NetworkProfiles::isValid(NetworkProfiles::kSf12));
}

void test_max_bundle_airtime_golden_cases() {
  TEST_ASSERT_EQUAL_UINT32(318, NetworkProfiles::maxBundleAirtimeMs(
                                    NetworkProfiles::kSf7));
  TEST_ASSERT_EQUAL_UINT32(1005, NetworkProfiles::maxBundleAirtimeMs(
                                     NetworkProfiles::kSf9));
  TEST_ASSERT_EQUAL_UINT32(1805, NetworkProfiles::maxBundleAirtimeMs(
                                     NetworkProfiles::kSf10));
  TEST_ASSERT_EQUAL_UINT32(2216, NetworkProfiles::maxBundleAirtimeMs(
                                     NetworkProfiles::kSf12));
}

void assertPacketClassAirtimes(
    const NetworkProfiles::NetworkProfile &profile,
    const uint32_t expected[9]) {
  const uint16_t lengths[9] = {
      BinaryPacket::kAwakenLoRaSize,
      BinaryPacket::kTimeSyncLoRaSize,
      BinaryPacket::kAckSummaryLoRaSize,
      BinaryPacket::kWindowMarkerLoRaSize,
      BinaryPacket::kStatusLoRaSize,
      BinaryPacket::kCmdResetLoRaSize,
      BinaryPacket::kCmdSetTxPowerLoRaSize,
      BinaryPacket::kCmdAckLoRaSize,
      BinaryPacket::kFullStateLoRaSize,
  };
  for (uint8_t i = 0; i < 9; ++i) {
    TEST_ASSERT_EQUAL_UINT32(
        expected[i], NetworkProfiles::applicationAirtimeMs(profile, lengths[i]));
  }
}

void test_packet_class_airtime_golden_cases() {
  const uint32_t sf7[9] = {52, 52, 47, 57, 72, 42, 47, 52, 72};
  const uint32_t sf9[9] = {165, 186, 165, 186, 247, 145, 165, 165, 227};
  const uint32_t sf10[9] = {330, 330, 289, 371, 453, 289, 289, 330, 453};
  const uint32_t sf12[9] = {660, 660, 578, 742, 906,
                            578, 578, 660, 824};
  assertPacketClassAirtimes(NetworkProfiles::kSf7, sf7);
  assertPacketClassAirtimes(NetworkProfiles::kSf9, sf9);
  assertPacketClassAirtimes(NetworkProfiles::kSf10, sf10);
  assertPacketClassAirtimes(NetworkProfiles::kSf12, sf12);
}

void test_wire_ceiling_and_operating_cap_are_separate() {
  TEST_ASSERT_EQUAL_UINT8(14, NetworkProfiles::kSf7.maxBundleDeltas);
  TEST_ASSERT_EQUAL_UINT8(7, NetworkProfiles::kSf12.maxBundleDeltas);
  TEST_ASSERT_EQUAL_UINT16(195,
                           NetworkProfiles::kSf7.maxOperationalApplicationBytes);
  TEST_ASSERT_EQUAL_UINT16(
      111, NetworkProfiles::kSf12.maxOperationalApplicationBytes);

  // A wire-maximum frame remains calculable/decodable at SF12. The smaller
  // operational cap remains an intentional conservative trial setting rather
  // than a second wire format.
  const uint32_t wireMaxSf12 = NetworkProfiles::applicationAirtimeMs(
      NetworkProfiles::kSf12, 195);
  TEST_ASSERT_EQUAL_UINT32(3609, wireMaxSf12);
  TEST_ASSERT_TRUE(wireMaxSf12 + NetworkProfiles::kSf12.txCompletionMarginMs <
                   NetworkProfiles::kSf12.slotWidthMs -
                       2u * NetworkProfiles::kSf12.guardMs);
}

void test_profile_modem_registers_follow_bandwidth() {
  TEST_ASSERT_EQUAL_UINT32(125000, NetworkProfiles::kSf7.bandwidthHz);
  TEST_ASSERT_EQUAL_UINT32(250000, NetworkProfiles::kSf12.bandwidthHz);
  TEST_ASSERT_EQUAL_UINT8(5, NetworkProfiles::kSf12.codingRateDenominator);
  TEST_ASSERT_EQUAL_HEX8(0x72, NetworkProfiles::sx127xModemConfig1(
                                    NetworkProfiles::kSf7));
  TEST_ASSERT_EQUAL_HEX8(0x82, NetworkProfiles::sx127xModemConfig1(
                                    NetworkProfiles::kSf12));
}

void test_low_data_rate_optimization_selection() {
  TEST_ASSERT_FALSE(NetworkProfiles::kSf7.lowDataRateOptimization);
  TEST_ASSERT_FALSE(NetworkProfiles::kSf9.lowDataRateOptimization);
  TEST_ASSERT_FALSE(NetworkProfiles::kSf10.lowDataRateOptimization);
  TEST_ASSERT_TRUE(NetworkProfiles::kSf12.lowDataRateOptimization);
}

void test_offered_load_keeps_control_headroom() {
  TEST_ASSERT_TRUE(
      NetworkProfiles::offeredLoadPermille(NetworkProfiles::kSf7) <= 900);
  TEST_ASSERT_TRUE(
      NetworkProfiles::offeredLoadPermille(NetworkProfiles::kSf9) <= 900);
  TEST_ASSERT_TRUE(
      NetworkProfiles::offeredLoadPermille(NetworkProfiles::kSf10) <= 900);
  TEST_ASSERT_TRUE(
      NetworkProfiles::offeredLoadPermille(NetworkProfiles::kSf12) <= 900);
}

void test_network_profile_announcement_serialization() {
  BinaryPacket::NetworkProfilePayload profile = {};
  profile.schema_version = 1;
  profile.profile_id = 12;
  profile.spreading_factor = 12;
  profile.coding_rate_denominator = 5;
  profile.bandwidth_hz = 250000;
  profile.num_slots = 5;
  profile.slot_width_ms = 5000;
  profile.guard_ms = 100;
  profile.max_bundle_deltas = 7;
  profile.continuous_sample_period_ms = 4000;
  profile.timed_sample_period_ms = 4000;
  profile.status_interval_ms = 300000;
  profile.fingerprint = 0x1234ABCD;

  uint8_t bytes[BinaryPacket::kNetworkProfileUartPayloadSize] = {};
  TEST_ASSERT_EQUAL_UINT8(
      BinaryPacket::kNetworkProfileUartPayloadSize,
      BinaryPacket::encodeNetworkProfilePayload(1, 9, profile, bytes,
                                                sizeof(bytes)));
  BinaryPacket::PktHeader header = {};
  memcpy(&header, bytes, sizeof(header));
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_MAGIC, header.magic);
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_NETWORK_PROFILE, header.pkt_type);
  TEST_ASSERT_EQUAL_UINT8(1, header.node_id);
  TEST_ASSERT_EQUAL_UINT8(9, header.seq);
  TEST_ASSERT_EQUAL_UINT8(12, bytes[sizeof(BinaryPacket::PktHeader) + 1]);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_all_profiles_are_valid);
  RUN_TEST(test_max_bundle_airtime_golden_cases);
  RUN_TEST(test_packet_class_airtime_golden_cases);
  RUN_TEST(test_wire_ceiling_and_operating_cap_are_separate);
  RUN_TEST(test_profile_modem_registers_follow_bandwidth);
  RUN_TEST(test_low_data_rate_optimization_selection);
  RUN_TEST(test_offered_load_keeps_control_headroom);
  RUN_TEST(test_network_profile_announcement_serialization);
  return UNITY_END();
}
