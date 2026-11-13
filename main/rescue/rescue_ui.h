#pragma once

#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

class RescueBoard;

// Builds the rescue UI (status / repository / switch) and starts the install
// progress timer. LVGL runs in the esp_lvgl_port task; this returns once the UI
// is up.
void rescue_ui_run(RescueBoard* board);

#endif  // CONFIG_METAOS_RESCUE_MODE
