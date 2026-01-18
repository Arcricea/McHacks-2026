#include "esp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "graphics.h"
#include "sd_card.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern void speaker_main();

extern "C" int app_main() {
  graphics_init();
  xTaskCreate(graphics_main, "graphics", 4096, NULL, 10, NULL);
  server_app_main();
  // spi_main(); // Commented out to skip text file tests
  speaker_main(); // Run the speaker/audio test
  return 0;
}
