/* Simulator: TJpgDec typedefs its own uint32_t etc. whenever _WIN32 is defined, which clashes
   with <stdint.h> on MinGW. Include it with _WIN32 hidden (its other headers come first). */
#pragma once
#include <stdint.h>
#include <string.h>
#pragma push_macro("_WIN32")
#undef _WIN32
#include "../../managed_components/espressif__esp_jpeg/tjpgd/tjpgd.h"
#pragma pop_macro("_WIN32")
