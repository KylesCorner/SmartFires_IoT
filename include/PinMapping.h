#include <Arduino.h>
#include "DHT.h"
static constexpr uint8_t PIN_FLAME_AO = A0;
static constexpr uint8_t PIN_FLAME_DO = 3;

static constexpr uint8_t PIN_DHT = 2;
static constexpr uint8_t DHT_TYPE = DHT11; // or DHT22

static constexpr uint8_t PIN_BUZZER = 5; // choose a PWM-capable pin (not required for tone())

