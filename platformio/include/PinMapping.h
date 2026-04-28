#pragma once
#include <Arduino.h>

// Pin definitions for the project. Adjust as needed for your specific board and wiring.
// Refers to the gpio pin numbers, not physical pin numbers. Check your board's documentation for details.

// static constexpr uint8_t PIN_FLAME_AO = A2;
// static constexpr uint8_t PIN_FLAME_DO = D12; // choose a digital pin (not all pins support interrupts, but that's optional for this sensor)



//static constexpr uint8_t PIN_DHT = 2;
//static constexpr uint8_t DHT_TYPE = DHT11; // or DHT22

// static constexpr uint8_t PIN_BUZZER = 5; // choose a PWM-capable pin (not required for tone())
//
// static constexpr uint8_t PIN_SERVO = 6; // choose a PWM-capable pin

static constexpr uint8_t PIN_SPS_RX = D12;
static constexpr uint8_t PIN_SPS_TX = D11;


static constexpr uint8_t PIN_WIND_RV = A1;
static constexpr uint8_t PIN_WIND_TMP = A0;
static constexpr uint8_t PIN_WIND_POWER = D2;

static constexpr uint8_t PIN_LORA_TX= TX;
static constexpr uint8_t PIN_LORA_RX= RX;

static constexpr uint8_t PIN_GPS_WAKE = A2;


// keypad setup
static const uint8_t KEYPAD_ROWS[4] = {D3, D4, D5, D6};
static const uint8_t KEYPAD_COLS[4] = {D7, D8, D9, D10};

static const char KEYPAD_MAP[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// keypad actions
static const char KEYPAD_PREV_PAGE = '1';
static const char KEYPAD_NEXT_PAGE= '2';
static const char KEYPAD_TOGGLE_SENSING = 'B';
static const char KEYPAD_HOMEPAGE = '#';
static const char KEYPAD_TOGGLE_SLEEP = 'A';
static const char KEYPAD_TOGGLE_CONT = 'C';


