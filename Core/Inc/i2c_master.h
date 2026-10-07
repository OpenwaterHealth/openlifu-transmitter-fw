/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * i2c_master.h
 *
 *  Created on: Jan 17, 2024
 *      Author: gvigelet
 */

#ifndef INC_I2C_MASTER_H_
#define INC_I2C_MASTER_H_

#include "main.h"
#include <stdio.h>
#include <stdbool.h>

void I2C_scan_local(void);
void I2C_scan_global(void);
uint8_t send_buffer_to_slave_global(uint8_t slave_addr, uint8_t* pBuffer, uint16_t buf_len);
uint8_t send_buffer_to_slave_local(uint8_t slave_addr, uint8_t* pBuffer, uint16_t buf_len);
uint8_t read_status_register_of_slave_local(uint8_t slave_addr, uint8_t* pBuffer, uint16_t max_len);
uint16_t read_buffer_of_slave_global(uint8_t slave_addr, uint8_t* pBuffer, uint16_t max_len);
uint8_t read_data_register_of_slave_local(uint8_t slave_addr, uint8_t* pBuffer, size_t rx_len);
uint8_t read_data_register_of_slave_global(uint8_t slave_addr, uint8_t* pBuffer, size_t rx_len);
uint8_t read_raw_from_slave_global(uint8_t slave_addr, uint8_t* pBuffer, size_t rx_len);
uint16_t I2C_read_CDCE6214_reg(uint8_t i2c_addr, uint16_t reg_addr);
bool I2C_write_CDCE6214_reg(uint8_t i2c_addr, uint16_t reg_addr, uint16_t reg_val);

float MAX31875_ReadTemperature(void);

#endif /* INC_I2C_MASTER_H_ */
