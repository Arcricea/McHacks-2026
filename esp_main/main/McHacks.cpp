#include "esp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "graphics.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern "C" int app_main() {
  graphics_init();
  xTaskCreate(graphics_main, "graphics", 4096, NULL, 10, NULL);
  server_app_main();
  return 0;
}
