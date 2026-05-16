#include "Arduino.h"

extern "C" void app_main() {
  initArduino();
  setup();
  while (true) {
    loop();
    delay(1);
  }
}
