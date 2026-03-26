#include <Arduino.h>

// Pin definitions for the project. Adjust as needed for your specific board and wiring.
// Refers to the gpio pin numbers, not physical pin numbers. Check your board's documentation for details.

static constexpr uint8_t PIN_FLAME_AO = A0;
static constexpr uint8_t PIN_FLAME_DO = D12; // choose a digital pin (not all pins support interrupts, but that's optional for this sensor)

//static constexpr uint8_t PIN_DHT = 2;
//static constexpr uint8_t DHT_TYPE = DHT11; // or DHT22

static constexpr uint8_t PIN_BUZZER = 5; // choose a PWM-capable pin (not required for tone())

static constexpr uint8_t PIN_SERVO = 6; // choose a PWM-capable pin

static constexpr uint8_t PIN_BUTTON = D11;

static constexpr uint8_t PIN_WIND_RV = A1;
static constexpr uint8_t PIN_WIND_TMP = A0;