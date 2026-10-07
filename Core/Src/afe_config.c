/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * afe_config.c
 *
 *  Created on: Jan 8, 2024
 *      Author: gvigelet
 */

#include "afe_config.h"




void loadDeviceConfig(DeviceConfig_t *config)
{
	// F072 doesn't have an eeprom need to rework this and use flash
	// config->i2c_address = *(__IO uint32_t*)EEPROM_I2C_ADDR;
	config->i2c_address = 0x32;
}

void writeDeviceConfig(DeviceConfig_t *config)
{
	// F072 doesn't have an eeprom need to rework this and use flash
}

void eraseDeviceConfig(void)
{
	// F072 doesn't have an eeprom need to rework this and use flash
}
