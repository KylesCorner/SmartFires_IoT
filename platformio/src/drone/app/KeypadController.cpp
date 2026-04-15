#include "KeypadController.h"
#include "PinMapping.h"

void KeypadController::update() {
  if (!_ctx.keypad.keyAvailable()) return;

  const char key = _ctx.keypad.consumeKey();
  if (key == '\0') return;

  Serial.print("[KEYPAD] pressed: ");
  Serial.println(key);

  switch (key) {
    case KEYPAD_PREV_PAGE: {
      const uint8_t current = static_cast<uint8_t>(_state.ui.currentPage);
      const uint8_t count = static_cast<uint8_t>(OledPage::Count);
      _state.ui.currentPage = static_cast<OledPage>((current == 0) ? (count - 1) : (current - 1));
      _state.ui.oledNeedsRefresh = true;
      break;
    }

    case KEYPAD_NEXT_PAGE: {
      const uint8_t current = static_cast<uint8_t>(_state.ui.currentPage);
      const uint8_t count = static_cast<uint8_t>(OledPage::Count);
      _state.ui.currentPage = static_cast<OledPage>((current + 1) % count);
      _state.ui.oledNeedsRefresh = true;
      break;
    }

    case KEYPAD_TOGGLE_SENSING:
      _state.sensingEnabled = !_state.sensingEnabled;
      Serial.print("[KEYPAD] sensing ");
      Serial.println(_state.sensingEnabled ? "ENABLED" : "DISABLED");
      _state.ui.oledNeedsRefresh = true;
      break;

    case KEYPAD_HOMEPAGE:
      _state.ui.currentPage = OledPage::Env;
      _state.ui.oledNeedsRefresh = true;
      break;

    case KEYPAD_TOGGLE_SLEEP:
      _state.sensorsSleeping = !_state.sensorsSleeping;
      _state.sensingEnabled = !_state.sensingEnabled;
      _state.ui.oledNeedsRefresh = true;
      break;

    default:
      Serial.printf("[KEYPAD] key: %c reserved\n", key);
      break;
  }
}
