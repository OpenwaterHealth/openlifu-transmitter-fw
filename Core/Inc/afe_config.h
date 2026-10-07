/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * afe_config.h
 *
 *  Created on: Jan 8, 2024
 *      Author: gvigelet
 */

#ifndef INC_AFE_CONFIG_H_
#define INC_AFE_CONFIG_H_

#include "main.h"
#include "common.h"
#include <stdint.h>

// Configuration structure
typedef struct {
    uint8_t tx7332_id;
    uint8_t i2c_address;
    // Add more configuration parameters if needed
} DeviceConfig_t;


// Functions for configuration
void loadDeviceConfig(DeviceConfig_t *config);
void writeDeviceConfig(DeviceConfig_t *config);
void eraseDeviceConfig(void);

#endif /* INC_AFE_CONFIG_H_ */
