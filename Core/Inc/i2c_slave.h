/*
 * i2c_slave.h
 *
 *  Created on: Feb 24, 2025
 *      Author: GeorgeVigelette
 */

#ifndef INC_I2C_SLAVE_H_
#define INC_I2C_SLAVE_H_

#include "main.h"
#include "i2c_protocol.h"
#include <stdio.h>

extern I2C_TX_Packet* data_available;

typedef enum {
    STATE_IDLE,
    STATE_SEND_DATA,
    STATE_BUSY,
	STATE_READ_DATA,
    STATE_ERROR
} I2C_Slave_State;

void i2c_print_info();
void I2C_Slave_Init(uint8_t addr);
void I2C_Slave_DeInit(void);
void I2C_BusRecovery(I2C_HandleTypeDef *hi2c);
void I2C_Process();
bool set_transmit_buffer(I2C_TX_Packet* packet);

#endif /* INC_I2C_SLAVE_H_ */
