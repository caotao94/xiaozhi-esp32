#pragma once

#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

// Entry point of the minimal factory rescue application (called from main.cc).
// Independent of Application/audio/MCP/cloud.
void rescue_main();

#endif  // CONFIG_METAOS_RESCUE_MODE
