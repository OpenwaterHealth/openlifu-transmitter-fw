/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * flash_eeprom.h
 *
 *  Created on: Mar 4, 2024
 *      Author: gvigelet
 */

#ifndef INC_FLASH_EEPROM_H_
#define INC_FLASH_EEPROM_H_

#include "stm32l4xx.h"
#include "memory_map.h"


/* Function prototypes */
HAL_StatusTypeDef Flash_Write(uint32_t address, const void *src, uint32_t size_bytes);
HAL_StatusTypeDef Flash_Read(uint32_t address, void *dst, uint32_t size_bytes);
HAL_StatusTypeDef Flash_Erase(uint32_t start_address, uint32_t end_address);


#endif /* INC_FLASH_EEPROM_H_ */
