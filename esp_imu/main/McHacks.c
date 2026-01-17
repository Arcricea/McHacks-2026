#include "esp_client.h"
#include "freertos/task.h"
#include "mp6050.h"
#include "portmacro.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

void imu_main();
extern "C" int app_main() {
  client_app_main();
  imu_main();
  return 0;
}

void imu_main() {
  imu_init();
  while (1) {
    custom_queue_add(imu_read());
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}
