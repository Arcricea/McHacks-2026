//
// Created by Muhammad Ali Ullah on 2026-01-17.
//

#ifndef MCHAX_SD_CARD_H
#define MCHAX_SD_CARD_H

#include "esp_err.h"

void spi_main();
esp_err_t sd_card_init(void);
void sd_card_deinit(void);

#endif //MCHAX_SD_CARD_H