/*
 * uart_comms.h
 *
 *  Created on: May 13, 2024
 *      Author: gvigelet
 */

#ifndef INC_UART_COMMS_H_
#define INC_UART_COMMS_H_

#include "main.h"  // This should contain your HAL includes and other basic includes.
#include "common.h"
#include <stdio.h>
#include <stdbool.h>

void comms_host_start(void);
void comms_host_check_received(void);
bool comms_onewire_slave_start(void);
void comms_onewire_check_received(void);
bool comms_onewire_master_sendreceive(UartPacket* pSendPacket, UartPacket* pRetPacket);

void comms_handle_ow_CallOut_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size);
void comms_handle_ow_CallOut_TxCpltCallback(UART_HandleTypeDef *huart);

void comms_handle_ow_CallIn_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size);
void comms_handle_ow_CallIn_TxCpltCallback(UART_HandleTypeDef *huart);

bool enumerate_slaves(void);

// Phase 2: broadcast OW_CMD_CLEAR_CONFIG down the chain so every node drops its
// stale enumeration state before a (re-)enumeration walk. Fixes master-reboot desync.
void clear_chain_config(void);

// Phase 2: master-only on-demand re-enumeration (clear-config + discovery walk).
// Returns the resulting module count (including the master).
uint8_t master_reenumerate(void);

void CDC_handle_TxCpltCallback();

bool configure_master();
bool configure_slave();

#endif /* INC_UART_COMMS_H_ */
