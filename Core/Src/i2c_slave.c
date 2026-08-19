/*
 * i2c_slave.c
 *
 *  Created on: Feb 24, 2025
 *      Author: GeorgeVigelette
 */
/*
 * i2c_slave.c
 *
 *  Created on: Mar 30, 2024
 *      Author: gvigelet
 */

#include "main.h"
#include "common.h"
#include "if_commands.h"
#include "i2c_protocol.h"
#include "i2c_slave.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

#define DATA_BUFFER_SIZE 2048

uint8_t rx_buffer[I2C_BUFFER_SIZE];
uint8_t tx_buffer[I2C_BUFFER_SIZE];
uint8_t return_buffer[I2C_BUFFER_SIZE];

uint8_t tx_position = 0;  // 0 - status, 8 - data packet
size_t tx_bytes = 0;
static uint8_t* send_buffer = 0;
I2C_TX_Packet* data_available;

I2C_TX_Packet ret_data;
uint8_t rec_data_buffer[DATA_BUFFER_SIZE] = {0};

I2C_TX_Packet tx_packet;
I2C_TX_Packet rx_packet;
I2C_TX_Packet packet_to_send_to_master;

__IO uint16_t rx_count = 0;
__IO uint16_t tx_packet_count = 0;
__IO int is_first_byte_received = 0;
__IO int countAddr = 0;
__IO int countrxCplt = 0;
__IO int countError = 0;

void I2C_Slave_Init(uint8_t addr) {

  // DeInit/Init cycle so the new OwnAddress1 is programmed from a known-clean
  // state. Without this, re-arming after a clear-config could leave the old
  // address active (two nodes answering the same address) or a half-configured
  // peripheral, which manifested as forwarded reads failing after re-enumeration.
  HAL_I2C_DeInit(GLOBAL_I2C_DEVICE);

  if(addr == 0x00 || addr > 0x7F){
	  GLOBAL_I2C_DEVICE->Init.OwnAddress1  = 0x32 << 1;  // default to 32
  }else{
	  GLOBAL_I2C_DEVICE->Init.OwnAddress1  = addr << 1;
  }

  data_available = NULL;
  // Reinitialize the I2C peripheral with the updated configuration
  if (HAL_I2C_Init(GLOBAL_I2C_DEVICE) != HAL_OK) {
	  // Handle the error if reinitialization fails
	  printf("Error Handler");
	  Error_Handler();
  }

  // clear header
  memset(tx_buffer, 0, I2C_BUFFER_SIZE);
  memset(return_buffer, 0, I2C_BUFFER_SIZE);


  packet_to_send_to_master.id = 00;
  packet_to_send_to_master.cmd = 0x00;
  packet_to_send_to_master.reserved = 0;

  if(HAL_I2C_EnableListen_IT(GLOBAL_I2C_DEVICE) != HAL_OK) {
	  // Handle the error if reinitialization fails
	  printf("Error Handler");
	  Error_Handler();
  }

}

// Stop answering on the I2C slave bus. Called when a slave drops its enumeration
// state (clear-config) so a stale OwnAddress1 does not linger and collide with
// another node before this slave is re-assigned an address.
void I2C_Slave_DeInit(void) {
  if (GLOBAL_I2C_DEVICE != NULL && GLOBAL_I2C_DEVICE->Instance != NULL) {
    HAL_I2C_DisableListen_IT(GLOBAL_I2C_DEVICE);
    HAL_I2C_DeInit(GLOBAL_I2C_DEVICE);
  }
}

// Short (~few us) bit-bang delay for bus recovery. Precise timing is not needed;
// a slow recovery clock is fine — the stuck slave only needs clock edges.
static void i2c_recov_delay(void) {
  for (volatile int i = 0; i < 300; i++) { __NOP(); }
}

// I2C bus recovery: if a slave (or a master reset mid-transaction) left SDA/SCL
// stuck low, drive up to 9 manual SCL clock pulses to let the stuck device finish
// its byte and release SDA, then issue a STOP. This is the standard remedy for a
// wedged I2C bus after a master-only reboot (the shared inter-board bus otherwise
// stays held low and every forwarded transaction fails). Restores I2C AF + re-init.
void I2C_BusRecovery(I2C_HandleTypeDef *hi2c) {
  if (hi2c == NULL || hi2c->Instance == NULL) { return; }

  GPIO_TypeDef *port;
  uint16_t scl_pin, sda_pin;
  if (hi2c->Instance == I2C1) {
    port = GLOBAL_SCL_GPIO_Port; scl_pin = GLOBAL_SCL_Pin; sda_pin = GLOBAL_SDA_Pin;   /* PB6/PB7 */
  } else if (hi2c->Instance == I2C2) {
    port = LOCAL_SCL_GPIO_Port;  scl_pin = LOCAL_SCL_Pin;  sda_pin = LOCAL_SDA_Pin;    /* PB10/PB11 */
  } else {
    return;
  }

  HAL_I2C_DeInit(hi2c);   /* release AF control of the pins */

  GPIO_InitTypeDef g = {0};
  g.Mode  = GPIO_MODE_OUTPUT_OD;
  g.Pull  = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin   = scl_pin | sda_pin;
  HAL_GPIO_Init(port, &g);

  HAL_GPIO_WritePin(port, scl_pin | sda_pin, GPIO_PIN_SET);  /* idle high */
  i2c_recov_delay();

  /* Clock out a stuck slave: up to 9 SCL pulses until SDA is released high. */
  for (int i = 0; i < 9; i++) {
    if (HAL_GPIO_ReadPin(port, sda_pin) == GPIO_PIN_SET) { break; }
    HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_RESET); i2c_recov_delay();
    HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_SET);   i2c_recov_delay();
  }

  /* Generate a STOP: SDA low while SCL high, then SDA high. */
  HAL_GPIO_WritePin(port, sda_pin, GPIO_PIN_RESET); i2c_recov_delay();
  HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_SET);   i2c_recov_delay();
  HAL_GPIO_WritePin(port, sda_pin, GPIO_PIN_SET);   i2c_recov_delay();

  /* Restore the peripheral (HAL_I2C_Init re-runs MspInit -> pins back to I2C AF). */
  HAL_I2C_Init(hi2c);
}

void i2c_print_info() {
    //uint32_t timing = GLOBAL_I2C_DEVICE.Init.Timing;
    //uint32_t pclk = HAL_RCC_GetPCLK1Freq(); // Get the peripheral clock frequency

    // Calculate the I2C speed in Hz
    //uint32_t i2c_speed = pclk / ((timing & 0xFFFF) + 1);

    printf("I2C Speed: %d kHz\r\n", 400); // Print the I2C speed in kHz
    printf("I2C Slave Addr: 0x%02x\r\n\r\n", (uint8_t)(GLOBAL_I2C_DEVICE->Init.OwnAddress1 >> 1));
}

// Command opcode -> packet type. The 0x20 / 0x00 nibbles map straight onto
// TX7332 / global commands, but the controller commands the master forwards to
// us do not live in one nibble (0x1D-0x1F alongside 0x28-0x2A), so they are
// listed explicitly. Anything unrecognised is rejected rather than guessed.
static uint8_t slave_packet_type_for(uint8_t command)
{
	switch (command) {
	case OW_CTRL_SET_SWTRIG:
	case OW_CTRL_SET_DELAY_PROFILE:
	case OW_CTRL_GET_DELAY_PROFILE:
	case OW_CTRL_SET_PROFILE_CYCLE:
	case OW_CTRL_SET_PATTERN_PROFILE:
	case OW_CTRL_GET_PATTERN_PROFILE:
	case OW_CTRL_ARM_PROFILE_CYCLE:
		return OW_CONTROLLER;
	default:
		break;
	}

	if ((command & 0xF0) == 0x20) return OW_TX7332;
	if ((command & 0xF0) == 0x00) return OW_CMD;
	return OW_ERROR;
}

void I2C_Process() {
	if (!data_available) return;

	UartPacket new_cmd;
	UartPacket resp;

	memset(rec_data_buffer, 0, DATA_BUFFER_SIZE);

	// convert command
	new_cmd.id = data_available->id;

	new_cmd.command = data_available->cmd;
	new_cmd.packet_type = slave_packet_type_for(new_cmd.command);
	/* For TX7332 commands data_available->reserved carries the local chip index
	 * which CONTROLLER/TX7332_ProcessCommand expects in cmd->addr.
	 * For all other commands the slave always processes for itself
	 * (module 0), so force addr=0 and only put the original reserved value in
	 * new_cmd.reserved (e.g. 0=READ / 1=WRITE for USR_CFG, arm/disarm for
	 * OW_CTRL_ARM_PROFILE_CYCLE). */
	if (new_cmd.packet_type == OW_TX7332) {
		new_cmd.addr = data_available->reserved;  // local TX chip index
	} else {
		new_cmd.addr = 0;                          // always self on slave
	}
	new_cmd.reserved = data_available->reserved;  // needed by ONE_WIRE handlers (e.g. USR_CFG read/write)
	new_cmd.data_len = data_available->data_len;
	new_cmd.data = rec_data_buffer;
	if(data_available->data_len>0){
		memcpy(new_cmd.data, data_available->pData, data_available->data_len);
	}
	packet_to_send_to_master.id = data_available->id;
	packet_to_send_to_master.cmd = data_available->cmd;

	// clear data available buffer
	data_available = NULL;

	process_if_command(&new_cmd, &resp);

	// convert response to i2c return
	if(resp.packet_type != OW_ERROR)
	{
		packet_to_send_to_master.id = resp.id;
		packet_to_send_to_master.cmd = resp.command;
		packet_to_send_to_master.reserved = resp.packet_type;
		packet_to_send_to_master.data_len = resp.data_len;
		packet_to_send_to_master.pData = resp.data;
		set_transmit_buffer(&packet_to_send_to_master);
	}
	else
	{
		set_transmit_buffer(NULL);
	}
	

}

bool set_transmit_buffer(I2C_TX_Packet* packet)
{
	bool ret = false;

	memset(tx_buffer, 0, I2C_BUFFER_SIZE);
	if(packet)
	{
		if(i2c_packet_toBuffer(packet, tx_buffer)>0)
		{
			// update tx_packet from this buffer
			ret = i2c_packet_fromBuffer(tx_buffer, packet);
		}
	}else{
		packet_to_send_to_master.reserved = OW_INVALID_PACKET;
		packet_to_send_to_master.data_len = 0;
		packet_to_send_to_master.pData = NULL;
		if(i2c_packet_toBuffer(&packet_to_send_to_master, tx_buffer)>0) {
			i2c_packet_fromBuffer(tx_buffer, &packet_to_send_to_master);
			ret = true;
		}
	}
	if(!ret && packet != NULL){
		packet->reserved = OW_INVALID_PACKET;
	}
	return ret;
}

/**
  * @brief  Listen Complete callback.
  * @param  hi2c Pointer to a I2C_HandleTypeDef structure that contains
  *                the configuration information for the specified I2C.
  * @retval None
  */
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
	HAL_I2C_EnableListen_IT(hi2c);
}

/**
  * @brief  Slave Address Match callback.
  * @param  hi2c Pointer to a I2C_HandleTypeDef structure that contains
  *                the configuration information for the specified I2C.
  * @param  TransferDirection: Master request Transfer Direction (Write/Read), value of @ref I2C_XferOptions_definition
  * @param  AddrMatchCode: Address Match Code
  * @retval None
  */
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{

	if(TransferDirection == I2C_DIRECTION_TRANSMIT)
	{
		if(is_first_byte_received == 0)
		{
			rx_count = 0;
			countAddr++;
			HAL_I2C_Slave_Sequential_Receive_IT(hi2c, rx_buffer + rx_count, 2, I2C_FIRST_FRAME);
		}
	}
	else
	{
		tx_packet_count = 0;
		tx_position = rx_buffer[0];
		tx_bytes = 0;
		if(tx_position == 0)
		{
			tx_bytes = i2c_packet_toBuffer(&packet_to_send_to_master, return_buffer);
			send_buffer = return_buffer;
		}else{
			// read buffer
			tx_bytes = tx_packet.pkt_len;
			// printf("Read Data %d\r\n", tx_bytes);
			send_buffer = tx_buffer;
		}

		HAL_I2C_Slave_Sequential_Transmit_IT(hi2c, send_buffer, tx_bytes, I2C_FIRST_AND_LAST_FRAME);
	}
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *I2cHandle)
{
	if(I2cHandle->Instance == GLOBAL_I2C_DEVICE->Instance) {
		if(is_first_byte_received == 1)
		{
			is_first_byte_received = 0;
		}
		else
		{
			// printf("send NAK\r\n");
			__HAL_I2C_GENERATE_NACK(I2cHandle);
		}
#if 0
		// can use this in case we don't know how much the master wants to read and generate a nak at the end of the buffer.
		tx_packet_count++;
		HAL_I2C_Slave_Seq_Transmit_IT(I2cHandle, send_buffer+tx_packet_count, 1, I2C_NEXT_FRAME);
#endif
	}
}

/**
  * @brief  Rx Transfer completed callback.
  * @param  I2cHandle: I2C handle
  * @note   This example shows a simple way to report end of IT Rx transfer, and
  *         you can add your own implementation.
  * @retval None
  */

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *I2cHandle)
{
	/* Reset address match code event */
	if(I2cHandle->Instance == GLOBAL_I2C_DEVICE->Instance) {
		if(is_first_byte_received == 0)
		{
			rx_count += 2;
			is_first_byte_received = 1;
			uint16_t pkt_len16 = (uint16_t)(rx_buffer[0] | ((uint16_t)rx_buffer[1] << 8));
			uint16_t bytes_left = pkt_len16 - 2;

			HAL_I2C_Slave_Seq_Receive_IT(I2cHandle, rx_buffer + rx_count, bytes_left, I2C_LAST_FRAME);
		}
		else
		{
			rx_count = (uint16_t)(rx_buffer[0] | ((uint16_t)rx_buffer[1] << 8));
			is_first_byte_received=0;
			// process data
			if (!i2c_packet_fromBuffer(rx_buffer, &rx_packet))
			{
				Error_Handler();
			}
			packet_to_send_to_master.id = rx_packet.id;
			packet_to_send_to_master.cmd = rx_packet.cmd;
			// printBuffer(rx_buffer, rx_count);
			// process or send for processing
			data_available = &rx_packet;
		}
	}
	else
	{
		// printf("UNHANDLED I2C Instance\r\n");
	}
}

/**
  * @brief  I2C error callbacks.
  * @param  I2cHandle: I2C handle
  * @note   This example shows a simple way to report transfer error, and you can
  *         add your own implementation.
  * @retval None
  */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *I2cHandle)
{
  countError++;
  uint32_t errorcode = HAL_I2C_GetError(I2cHandle);
  if (errorcode == 4)  // AF error
  {
	__HAL_I2C_CLEAR_FLAG(I2cHandle, I2C_FLAG_AF); //clear AF flag
	if(tx_packet_count == 0) //error is while slave is receiving
	{
		//process_data();
		rx_count = 0;
	}
	else // error while slave is transmitting
	{
		tx_packet_count = 0;
	}
  }
  else if (errorcode == 1)  // BERR Error
  {
	  //printf("HAL_I2C_ErrorCallback ERR: 0x%08lX Resetting devide\r\n", errorcode);
	  HAL_I2C_DeInit(I2cHandle);
	  HAL_I2C_Init(I2cHandle);

	  //enable_receive_header();
  }else{
	  //printf("HAL_I2C_ErrorCallback ERR: 0x%08lX\r\n", errorcode);
	  Error_Handler();
  }
  HAL_I2C_EnableListen_IT(I2cHandle);
}

