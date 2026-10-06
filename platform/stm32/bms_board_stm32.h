#ifndef BMS_BOARD_STM32_H_
#define BMS_BOARD_STM32_H_

#include <stdint.h>

int bms_board_stm32_init(void);
int bms_board_stm32_print_status(void);
uint32_t bms_board_stm32_alert_count(uint8_t monitor_index);

#endif
