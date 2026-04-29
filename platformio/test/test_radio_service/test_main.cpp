#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeRadio.h"
#include "radio/RadioService.h"
#include "telemetry/TelemetryFrame.h"

static void assertState(RadioService &service, RadioServiceState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(service.state()));
}

static void assertError(RadioService &service, RadioServiceError expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(service.error()));
}

static TelemetryFrame makeFrame(const char *payload) {
  TelemetryFrame frame;
  frame.clear();

  strncpy(frame.payload, payload, TelemetryFrame::MaxLen);
  frame.payload[TelemetryFrame::MaxLen - 1] = '\0';
  frame.len = strlen(frame.payload);

  return frame;
}

void test_radio_begin_success() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  RadioService service(cfg, radio, clock);

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(radio.beginCalled);
  assertState(service, RadioServiceState::Ready);
  assertError(service, RadioServiceError::None);
}

void test_radio_begin_failure_enters_error() {
  FakeClock clock;
  FakeRadio radio;
  radio.beginOk = false;

  RadioService::Config cfg;
  RadioService service(cfg, radio, clock);

  TEST_ASSERT_FALSE(service.begin());
  assertState(service, RadioServiceState::Error);
  assertError(service, RadioServiceError::BeginFailed);
}

void test_send_telemetry_without_ack_returns_ready() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  cfg.nodeId = 4;
  cfg.requireAck = false;

  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("battery,v=4.00");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.sendTelemetry(frame));

  assertState(service, RadioServiceState::Ready);
  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
  TEST_ASSERT_NOT_NULL(strstr(radio.lastSent(), "T,node=4,seq=1"));
  TEST_ASSERT_NOT_NULL(strstr(radio.lastSent(), "battery,v=4.00"));
}

void test_send_telemetry_waits_for_ack() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  cfg.nodeId = 2;
  cfg.requireAck = true;

  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("sht31,temp_c=22.0");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.sendTelemetry(frame));

  assertState(service, RadioServiceState::WaitingForAck);
  TEST_ASSERT_EQUAL_UINT16(1, service.lastSeq());

  radio.queueRx("ACK,1");
  service.update();

  assertState(service, RadioServiceState::Ready);
  assertError(service, RadioServiceError::None);
}

void test_wrong_ack_does_not_clear_waiting_state() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  cfg.requireAck = true;

  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("wind,wind_mps=1.2");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.sendTelemetry(frame));

  radio.queueRx("ACK,999");
  service.update();

  assertState(service, RadioServiceState::WaitingForAck);
}

void test_ack_timeout_retries_then_succeeds() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  cfg.requireAck = true;
  cfg.ackTimeoutMs = 100;
  cfg.maxRetries = 3;

  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("gps,fix=1");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.sendTelemetry(frame));
  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);

  clock.advance(100);
  service.update();

  TEST_ASSERT_EQUAL_UINT32(2, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, service.retryCount());
  assertState(service, RadioServiceState::WaitingForAck);

  radio.queueRx("ACK,1");
  service.update();

  assertState(service, RadioServiceState::Ready);
}

void test_ack_timeout_enters_error_after_max_retries() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  cfg.requireAck = true;
  cfg.ackTimeoutMs = 100;
  cfg.maxRetries = 1;

  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("pm,pm25=12.0");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.sendTelemetry(frame));

  clock.advance(100);
  service.update();

  TEST_ASSERT_EQUAL_UINT32(2, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, service.retryCount());

  clock.advance(100);
  service.update();

  assertState(service, RadioServiceState::Error);
  assertError(service, RadioServiceError::AckTimeout);
}

void test_receive_command() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  RadioService service(cfg, radio, clock);

  TEST_ASSERT_TRUE(service.begin());

  radio.queueRx("CMD,set_interval=30000");
  service.update();

  TEST_ASSERT_TRUE(service.hasCommand());

  char cmd[64];
  const size_t n = service.readCommand(cmd, sizeof(cmd));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_STRING("set_interval=30000", cmd);
  TEST_ASSERT_FALSE(service.hasCommand());
}

void test_send_failure_enters_error() {
  FakeClock clock;
  FakeRadio radio;
  radio.sendOk = false;

  RadioService::Config cfg;
  RadioService service(cfg, radio, clock);
  TelemetryFrame frame = makeFrame("battery,v=4.0");

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_FALSE(service.sendTelemetry(frame));

  assertState(service, RadioServiceState::Error);
  assertError(service, RadioServiceError::SendFailed);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_radio_begin_success);
  RUN_TEST(test_radio_begin_failure_enters_error);
  RUN_TEST(test_send_telemetry_without_ack_returns_ready);
  RUN_TEST(test_send_telemetry_waits_for_ack);
  RUN_TEST(test_wrong_ack_does_not_clear_waiting_state);
  RUN_TEST(test_ack_timeout_retries_then_succeeds);
  RUN_TEST(test_ack_timeout_enters_error_after_max_retries);
  RUN_TEST(test_receive_command);
  RUN_TEST(test_send_failure_enters_error);

  return UNITY_END();
}
