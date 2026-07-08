/*
 * uart_comms.c
 *
 *  Created on: Mar 11, 2024
 *      Author: gvigelet
 */

#include "if_commands.h"
#include "main.h"
#include "module_manager.h"
#include "uart_comms.h"
#include "i2c_slave.h"
#include "trigger.h"
#include "utils.h"
#include "usbd_cdc_if.h"
#include "thermistor.h"

#include <string.h>
#include <stdbool.h>

#define ONEWIRE_TIMEOUT 500
#define TX_TIMEOUT 500

// Private variables
uint8_t rxBuffer[COMMAND_MAX_SIZE];
uint8_t txBuffer[COMMAND_MAX_SIZE];
uint8_t owRxBuffer[COMMAND_MAX_SIZE];
uint8_t owTxBuffer[COMMAND_MAX_SIZE];

volatile uint32_t ptrReceive;
volatile uint8_t rx_flag = 0;
volatile uint8_t tx_flag = 0;
volatile uint8_t rx_ow_callin_flag = 0;
volatile uint8_t tx_ow_callin_flag = 0;
volatile uint8_t rx_ow_callout_flag = 0;
volatile uint8_t tx_ow_callout_flag = 0;
volatile uint16_t ow_packetid = 0;

volatile bool async_enabled = false;

static uint16_t ow_packet_count;
static UartPacket ow_send_packet;
static UartPacket ow_receive_packet;
static UartPacket ow_data_packet;
static uint8_t owDataBuffer[256] = {0};

static uint16_t get_ow_next_packetID(){
	ow_packet_count++;
	if(ow_packet_count == 0) ow_packet_count = 1;
	return ow_packet_count;
}

static void buffer_to_packet(uint8_t* pBuffer, UartPacket* pPacket) {

    int bufferIndex = 0;
    uint16_t calculated_crc;

    if(pBuffer[bufferIndex++] != OW_START_BYTE) {
        // Send NACK doesn't have the correct start byte
    	pPacket->id = 0xFFFF;
    	pPacket->data_len = 0;
    	pPacket->packet_type = OW_NAK;
        return;
    }

    pPacket->id = (pBuffer[bufferIndex] << 8 | (pBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;
    pPacket->packet_type = pBuffer[bufferIndex++];
    pPacket->command = pBuffer[bufferIndex++];
    pPacket->addr = pBuffer[bufferIndex++];
    pPacket->reserved = pBuffer[bufferIndex++];

    // Extract payload length
    pPacket->data_len = (pBuffer[bufferIndex] << 8 | (pBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Check if data length is valid
    if (pPacket->data_len > DATA_MAX_SIZE) {
        // Send NACK response due to no end byte
    	pPacket->data_len = 0;
        pPacket->packet_type = OW_ERROR;
        return;
    }

    // Extract data pointer
    pPacket->data = &pBuffer[bufferIndex];
    bufferIndex += pPacket->data_len; // move pointer to end of data
    
    // Extract received CRC
    pPacket->crc = (pBuffer[bufferIndex] << 8 | (pBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Calculate CRC for received data

    calculated_crc = util_crc16(&pBuffer[1], pPacket->data_len + 8);
    
    // Check CRC
    if (pPacket->crc != calculated_crc) {
        // Send NACK response due to bad CRC
    	pPacket->reserved = OW_BAD_CRC;
    	pPacket->data_len = 0;
        pPacket->packet_type = OW_ERROR;
        return;
    }

    // Check end byte
    if (pBuffer[bufferIndex++] != OW_END_BYTE) {
    	pPacket->data_len = 0;
    	pPacket->reserved = 0;
    	pPacket->packet_type = OW_ERROR;
        return;
    }
}

static void comms_interface_send(const UartPacket* pResp)
{
    tx_flag = 0;  // Clear the flag before starting transmission

    memset(txBuffer, 0, sizeof(txBuffer));
    int bufferIndex = 0;

    // Build the packet header
    txBuffer[bufferIndex++] = OW_START_BYTE;
    txBuffer[bufferIndex++] = pResp->id >> 8;
    txBuffer[bufferIndex++] = pResp->id & 0xFF;
    txBuffer[bufferIndex++] = pResp->packet_type;
    txBuffer[bufferIndex++] = pResp->command;
    txBuffer[bufferIndex++] = pResp->addr;
    txBuffer[bufferIndex++] = pResp->reserved;
    txBuffer[bufferIndex++] = (pResp->data_len) >> 8;
    txBuffer[bufferIndex++] = (pResp->data_len) & 0xFF;

    // Check for possible buffer overflow (optional)
    if ( (bufferIndex + pResp->data_len + 4) > sizeof(txBuffer) ) {
        // Handle error: packet too large for txBuffer
        return;
    }

    // Add data payload if any
    if(pResp->data_len > 0)
    {
        memcpy(&txBuffer[bufferIndex], pResp->data, pResp->data_len);
        bufferIndex += pResp->data_len;
    }

    // Compute CRC over the packet from index 1 for (pResp->data_len + 8) bytes
    uint16_t crc = util_crc16(&txBuffer[1], pResp->data_len + 8);
    txBuffer[bufferIndex++] = crc >> 8;
    txBuffer[bufferIndex++] = crc & 0xFF;

    // Add the end byte
    txBuffer[bufferIndex++] = OW_END_BYTE;

    // Initiate transmission via USB CDC
    CDC_Transmit_FS(txBuffer, bufferIndex);

    // Wait for the transmit complete flag with a timeout to avoid infinite loop.
    uint32_t start_time = HAL_GetTick();

    while(!tx_flag)
    {
        if ((HAL_GetTick() - start_time) >= TX_TIMEOUT)
        {
            // Timeout handling: Log error and break out or reset the flag.
            break;
        }
    }
}

static bool comms_callout_onewire_send(UartPacket* pResp)
{
    uint32_t start_time = 0;
    int bufferIndex = 0;

    /* Clear the transmission buffer */
    memset(owTxBuffer, 0, sizeof(owTxBuffer));

    /* Ensure packet ID is set */
    if(pResp->id == 0)
        pResp->id = get_ow_next_packetID();

    /* Build the packet */
    owTxBuffer[bufferIndex++] = OW_START_BYTE;
    owTxBuffer[bufferIndex++] = pResp->id >> 8;
    owTxBuffer[bufferIndex++] = pResp->id & 0xFF;
    owTxBuffer[bufferIndex++] = pResp->packet_type;
    owTxBuffer[bufferIndex++] = pResp->command;
    owTxBuffer[bufferIndex++] = pResp->addr;
    owTxBuffer[bufferIndex++] = pResp->reserved;
    owTxBuffer[bufferIndex++] = (pResp->data_len) >> 8;
    owTxBuffer[bufferIndex++] = (pResp->data_len) & 0xFF;

    /* Check for data payload overflow */
    if(pResp->data_len > 0)
    {
        /* (Optionally, check that bufferIndex + pResp->data_len is within bounds) */
        memcpy(&owTxBuffer[bufferIndex], pResp->data, pResp->data_len);
        bufferIndex += pResp->data_len;
    }

    /* Calculate CRC over the packet (from index 1, covering 8 header bytes + payload) */
    uint16_t crc = util_crc16(&owTxBuffer[1], pResp->data_len + 8);
    owTxBuffer[bufferIndex++] = crc >> 8;
    owTxBuffer[bufferIndex++] = crc & 0xFF;

    /* Append the end byte */
    owTxBuffer[bufferIndex++] = OW_END_BYTE;

    tx_ow_callout_flag = 0;

    /* Optional: Print the buffer for debugging */
    // printBuffer(owTxBuffer, pResp->data_len + 12);

    /* Enable the transmitter in half-duplex mode */
    if(HAL_HalfDuplex_EnableTransmitter(&CALL_OUT_UART) != HAL_OK)
    {
        /* Setup Error */
        Error_Handler();
    }

    /* Start transmission in interrupt mode */
    if(HAL_UART_Transmit_IT(&CALL_OUT_UART, (uint8_t *)owTxBuffer, bufferIndex) != HAL_OK)
    {
        /* Transmission Error */
        Error_Handler();
    }

    start_time = HAL_GetTick();  // Record the start time

    /* Wait for transmission to complete (flag set in the callback) */
    while(!tx_ow_callout_flag)
    {
        if ((HAL_GetTick() - start_time) >= ONEWIRE_TIMEOUT)
        {;
            return false;  // Timeout occurred
        }
        // Optionally add a small delay here to yield CPU time.
    }

    return true;
}


static bool comms_callin_onewire_send(UartPacket* pResp)
{
    uint32_t start_time = 0;
    int bufferIndex = 0;

    /* Clear the transmission buffer */
    memset(owTxBuffer, 0, sizeof(owTxBuffer));

    /* Ensure packet ID is set */
    if(pResp->id == 0)
        pResp->id = get_ow_next_packetID();

    /* Build the packet */
    owTxBuffer[bufferIndex++] = OW_START_BYTE;
    owTxBuffer[bufferIndex++] = pResp->id >> 8;
    owTxBuffer[bufferIndex++] = pResp->id & 0xFF;
    owTxBuffer[bufferIndex++] = pResp->packet_type;
    owTxBuffer[bufferIndex++] = pResp->command;
    owTxBuffer[bufferIndex++] = pResp->addr;
    owTxBuffer[bufferIndex++] = pResp->reserved;
    owTxBuffer[bufferIndex++] = (pResp->data_len) >> 8;
    owTxBuffer[bufferIndex++] = (pResp->data_len) & 0xFF;

    /* Check for data payload overflow */
    if(pResp->data_len > 0)
    {
        /* (Optionally, check that bufferIndex + pResp->data_len is within bounds) */
        memcpy(&owTxBuffer[bufferIndex], pResp->data, pResp->data_len);
        bufferIndex += pResp->data_len;
    }

    /* Calculate CRC over the packet (from index 1, covering 8 header bytes + payload) */
    uint16_t crc = util_crc16(&owTxBuffer[1], pResp->data_len + 8);
    owTxBuffer[bufferIndex++] = crc >> 8;
    owTxBuffer[bufferIndex++] = crc & 0xFF;

    /* Append the end byte */
    owTxBuffer[bufferIndex++] = OW_END_BYTE;

    tx_ow_callin_flag = 0;

    /* Optional: Print the buffer for debugging */
    // printBuffer(owTxBuffer, pResp->data_len + 12);

    /* Enable the transmitter in half-duplex mode */
    if(HAL_HalfDuplex_EnableTransmitter(&CALL_IN_UART) != HAL_OK)
    {
        /* Setup Error */
        Error_Handler();
    }

    /* Start transmission in interrupt mode */
    if(HAL_UART_Transmit_IT(&CALL_IN_UART, (uint8_t *)owTxBuffer, bufferIndex) != HAL_OK)
    {
        /* Transmission Error */
        Error_Handler();
    }

    start_time = HAL_GetTick();  // Record the start time

    /* Wait for transmission to complete (flag set in the callback) */
    while(!tx_ow_callin_flag)
    {
        if ((HAL_GetTick() - start_time) >= ONEWIRE_TIMEOUT)
        {
            return false;  // Timeout occurred
        }
        // Optionally add a small delay here to yield CPU time.
    }

    return true;
}

static void comms_callout_onewire_receive(UartPacket* pRetPacket)
{
    uint32_t start_time = 0;

    // Clear the buffer
    memset(owRxBuffer, 0, sizeof(owRxBuffer));

    // Clear the flag before starting the reception
    rx_ow_callout_flag = 0;  // Ensure rx_ow_callout_flag is declared as volatile

    // Enable receiver in half-duplex mode
    if(HAL_HalfDuplex_EnableReceiver(&CALL_OUT_UART) != HAL_OK) {
        // Receive Error
        Error_Handler();
    }

    // Start reception until idle (non-blocking, interrupt-driven)
    if (HAL_UARTEx_ReceiveToIdle_IT(&CALL_OUT_UART, owRxBuffer, COMMAND_MAX_SIZE) != HAL_OK) {
        // Receive Error
        Error_Handler();
    }

    // Start the timeout counter
    start_time = HAL_GetTick();
    while(!rx_ow_callout_flag) {
        if ((HAL_GetTick() - start_time) >= ONEWIRE_TIMEOUT) {
            pRetPacket->packet_type = OW_TIMEOUT;
            pRetPacket->data_len = 0;
            return;  // Timeout occurred
        }
    }

    // Process the received data
    buffer_to_packet(owRxBuffer, pRetPacket);
}

void comms_host_start(void)
{

	memset(rxBuffer, 0, sizeof(rxBuffer));
	ptrReceive = 0;

	CDC_FlushRxBuffer_FS();

    rx_flag = 0;
    tx_flag = 0;

	CDC_ReceiveToIdle(rxBuffer, COMMAND_MAX_SIZE);

}

void comms_host_check_received(void)
{
	UartPacket cmd;
	UartPacket resp;
    uint16_t calculated_crc;

	if(!rx_flag) return;

    int bufferIndex = 0;

    if(rxBuffer[bufferIndex++] != OW_START_BYTE) {
        // Send NACK doesn't have the correct start byte
    	resp.id = 0xFFFF;
        resp.data_len = 0;
        resp.packet_type = OW_NAK;
        goto NextDataPacket;
    }

    cmd.id = (rxBuffer[bufferIndex] << 8 | (rxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;
    cmd.packet_type = rxBuffer[bufferIndex++];
    cmd.command = rxBuffer[bufferIndex++];
    cmd.addr = rxBuffer[bufferIndex++];
    cmd.reserved = rxBuffer[bufferIndex++];

    // Extract payload length
    cmd.data_len = (rxBuffer[bufferIndex] << 8 | (rxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Check if data length is valid
    if (cmd.data_len > DATA_MAX_SIZE) {
        // Send NACK response due to no end byte
    	// data can exceed buffersize but every buffer must have a start and end packet
    	// command that will send more data than one buffer will follow with data packets to complete the request
    	resp.id = cmd.id;
    	resp.addr = 0;
    	resp.reserved = 0;
        resp.data_len = 0;
        resp.packet_type = OW_ERROR;
        goto NextDataPacket;
    }

    // Extract data pointer
    cmd.data = &rxBuffer[bufferIndex];
    bufferIndex += cmd.data_len; // move pointer to end of data
    
    // Extract received CRC
    cmd.crc = (rxBuffer[bufferIndex] << 8 | (rxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Calculate CRC for received data
  	calculated_crc = util_crc16(&rxBuffer[1], cmd.data_len + 8);

    // Check CRC
    if (cmd.crc != calculated_crc) {
        // Send NACK response due to bad CRC
    	resp.id = cmd.id;
    	resp.addr = 0;
    	resp.reserved = OW_BAD_CRC;
        resp.data_len = 0;
        resp.packet_type = OW_ERROR;
        goto NextDataPacket;
    }

    // Check end byte
    if (rxBuffer[bufferIndex++] != OW_END_BYTE) {
    	resp.id = cmd.id;
        resp.data_len = 0;
    	resp.addr = 0;
    	resp.reserved = 0;
        resp.packet_type = OW_ERROR;
        goto NextDataPacket;
    }

	process_if_command(&cmd, &resp);

NextDataPacket:
	comms_interface_send(&resp);
	memset(rxBuffer, 0, sizeof(rxBuffer));
	ptrReceive=0;
	rx_flag = 0;
	CDC_ReceiveToIdle(rxBuffer, COMMAND_MAX_SIZE);
}

void comms_onewire_check_received()
{
    uint16_t calculated_crc;

	if(!rx_ow_callin_flag) return;

    memset((void*)&ow_send_packet, 0, sizeof(ow_send_packet));
    memset((void*)&ow_receive_packet, 0, sizeof(ow_receive_packet));

    int bufferIndex = 0;

    if(owRxBuffer[bufferIndex++] != OW_START_BYTE) {
        // Send NACK doesn't have the correct start byte
    	ow_send_packet.id = OW_CMD_NOP;
    	ow_send_packet.data_len = 0;
    	ow_send_packet.reserved = OW_INVALID_PACKET;
    	ow_send_packet.packet_type = OW_ERROR;
        goto NextOneWirePacket;
    }


    ow_receive_packet.id = (owRxBuffer[bufferIndex] << 8 | (owRxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;
    ow_receive_packet.packet_type = owRxBuffer[bufferIndex++];
    ow_receive_packet.command = owRxBuffer[bufferIndex++];
    ow_receive_packet.addr = owRxBuffer[bufferIndex++];
    ow_receive_packet.reserved = owRxBuffer[bufferIndex++];

    // Extract payload length
    ow_receive_packet.data_len = (owRxBuffer[bufferIndex] << 8 | (owRxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Check if data length is valid
    if (ow_receive_packet.data_len > DATA_MAX_SIZE) {
        // Send NACK response due to no end byte
    	// data can exceed buffersize but every buffer must have a start and end packet
    	// command that will send more data than one buffer will follow with data packets to complete the request
    	ow_send_packet.id = ow_receive_packet.id;
    	ow_send_packet.addr = 0;
    	ow_send_packet.reserved = OW_INVALID_PACKET;
    	ow_send_packet.data_len = 0;
    	ow_send_packet.packet_type = OW_ERROR;
        goto NextOneWirePacket;
    }

    // Extract data pointer
    ow_receive_packet.data = &owRxBuffer[bufferIndex];
    bufferIndex += ow_receive_packet.data_len; // move pointer to end of data
    
    // Extract received CRC
    ow_receive_packet.crc = (owRxBuffer[bufferIndex] << 8 | (owRxBuffer[bufferIndex+1] & 0xFF ));
    bufferIndex+=2;

    // Calculate CRC for received data

    calculated_crc = util_crc16(&owRxBuffer[1], ow_receive_packet.data_len + 8);
    
    // Check CRC
    if (ow_receive_packet.crc != calculated_crc) {
        // Send NACK response due to bad CRC
    	ow_send_packet.id = ow_receive_packet.id;
    	ow_send_packet.addr = 0;
    	ow_send_packet.reserved = OW_BAD_CRC;
    	ow_send_packet.data_len = 0;
    	ow_send_packet.packet_type = OW_ERROR;
        goto NextOneWirePacket;
    }

    // Relay one-wire packets to the next slave so the master's request propagates
    // hop-by-hop. A configured slave relays:
    //   - DISCOVERY for a DIFFERENT module_id (its own is handled locally as an
    //     idempotent re-claim below — this is what makes a master reboot recover
    //     cleanly without any teardown), and
    //   - CLEAR_CONFIG always (an explicit chain-wide address reset).
    bool _ow_is_disc  = (ow_receive_packet.command == OW_CMD_DISCOVERY);
    bool _ow_is_clear = (ow_receive_packet.command == OW_CMD_CLEAR_CONFIG);
    if (ow_receive_packet.packet_type == OW_ONE_WIRE &&
        get_configured() && get_module_ID() != 0 &&
        ((_ow_is_disc && ow_receive_packet.reserved != get_module_ID()) || _ow_is_clear))
    {
        memset((void*)&ow_data_packet, 0, sizeof(ow_data_packet));
        if (comms_callout_onewire_send(&ow_receive_packet)) {
            comms_callout_onewire_receive(&ow_data_packet);
            ow_send_packet = ow_data_packet;
        } else {
            ow_send_packet.id = ow_receive_packet.id;
            ow_send_packet.packet_type = OW_ERROR;
            ow_send_packet.data_len = 0;
            ow_send_packet.data = NULL;
        }

        // For clear-config, after propagating downstream, drop our OWN stale state.
        // set_configured(false) wipes the module storage; the outer boot loop then
        // re-enters slave configuration (which de-inits I2C) and waits for a fresh
        // discovery assignment. (Explicit command path only — not used at boot.)
        if (_ow_is_clear) {
            set_configured(false);
            set_module_ID(0);
            set_slave_address(0);
            ow_send_packet.id = ow_receive_packet.id;
            ow_send_packet.packet_type = OW_ONEWIRE_RESP;
            ow_send_packet.command = OW_CMD_CLEAR_CONFIG;
            ow_send_packet.data_len = 0;
            ow_send_packet.data = NULL;
        }
        goto NextOneWirePacket;
    }

	process_if_command(&ow_receive_packet, &ow_send_packet);

NextOneWirePacket:
	HAL_Delay(1);
	if(!comms_callin_onewire_send(&ow_send_packet))
	{
		// printf("failed to send onewire response\r\n");
	}
	memset(owRxBuffer, 0, sizeof(owRxBuffer));
    rx_ow_callin_flag = 0;
    if(HAL_HalfDuplex_EnableReceiver(&CALL_IN_UART) != HAL_OK) {
    	// Receive Error
		Error_Handler();
    }

	if (HAL_UARTEx_ReceiveToIdle_IT(&CALL_IN_UART, owRxBuffer, COMMAND_MAX_SIZE) != HAL_OK) {
		// Receive Error
		Error_Handler();
	}
}

bool comms_onewire_slave_start()
{
	ow_packet_count = 0;
	memset(owRxBuffer, 0, sizeof(owRxBuffer));
    rx_ow_callin_flag = 0;
    tx_ow_callin_flag = 0;
    HAL_UART_Abort(&CALL_IN_UART);
    if(HAL_HalfDuplex_EnableReceiver(&CALL_IN_UART) != HAL_OK) {
    	// Receive Error
		Error_Handler();
    }

	if (HAL_UARTEx_ReceiveToIdle_IT(&CALL_IN_UART, owRxBuffer, COMMAND_MAX_SIZE) != HAL_OK) {
		// Receive Error
		Error_Handler();
	}

	return true;
}

bool configure_master()
{
	ow_packet_count = 0;
	bool bRet = true;
	set_module_ID(0);
	// Recover the shared inter-board I2C bus in case it was left wedged (SDA/SCL
	// stuck low) by a master-only reboot that interrupted a forwarded transaction.
	// Harmless on a clean bus (SDA already high -> immediate return). Runs before
	// any forwarding so enumeration + slave reads start from a clean bus.
	I2C_BusRecovery(GLOBAL_I2C_DEVICE);
    HAL_UART_Abort(&CALL_OUT_UART);
    rx_ow_callin_flag = 0;
    tx_ow_callin_flag = 0;

	// configure master
    ModuleManager_Init();
    // Register the master module
    ModuleManager_RegisterMaster(0x00);

	return bRet;
}

bool configure_slave()
{
	bool bRet = false;
	CDC_Stop_ReceiveToIdle();
	ModuleManager_DeInit();
	// Drop any stale I2C slave address so a just-cleared node stops answering at
	// its old address until it is re-assigned one during discovery. Re-armed by
	// I2C_Slave_Init() once a fresh address is claimed.
	I2C_Slave_DeInit();
	set_device_role(ROLE_SLAVE);
	// configure slave
	comms_onewire_slave_start();
	return bRet;
}

#define BASE_I2C_ADDRESS 0x20   // Starting address for slaves
#define MAX_SLAVES       6      // Maximum number of slaves in the chain

bool enumerate_slaves()
{
    uint8_t next_address = BASE_I2C_ADDRESS;
    uint8_t slave_count = 1;
    bool bRet = true;

	// register found slaves

    while(slave_count < MAX_SLAVES)
    {
        // Clear the send packet structure before use.
        memset((void*)&ow_send_packet, 0, sizeof(ow_send_packet));

        // Prepare the discovery message.
        ow_send_packet.packet_type = OW_ONE_WIRE;
        ow_send_packet.command = OW_CMD_DISCOVERY;
        ow_send_packet.reserved = slave_count;
        ow_send_packet.addr = next_address;

        // Send the discovery packet to the current slave.
        if (comms_callout_onewire_send(&ow_send_packet))
        {
            // Wait for a response.
            comms_callout_onewire_receive(&ow_receive_packet);

			// Check for an error response.
			if (ow_receive_packet.packet_type != OW_ERROR && ow_receive_packet.packet_type != OW_TIMEOUT)
			{
				//printf("Slave found at I2C address 0x%02X\r\n", next_address);
				// The discovery response payload carries the node's operating mode
				// (app vs bootloader). Legacy firmware answers with no payload -> APP.
				uint8_t node_mode = (ow_receive_packet.data_len >= 1 && ow_receive_packet.data != NULL)
				                    ? ow_receive_packet.data[0]
				                    : (uint8_t)NODE_MODE_APP;
				// Record the slave if necessary, e.g. store the address.
				ModuleManager_AddSlave(next_address, node_mode);
				slave_count++;
				next_address++;  // Move on to the next address.
			}
			else if (ow_receive_packet.packet_type == OW_TIMEOUT)
			{
				// OW_TIMEOUT means the chain ended — no more slaves
				break;
			}
			else
			{
				//printf("Received error from slave at address 0x%02X\r\n", next_address);
				bRet = false;
	            break;
			}
        }
        else
        {
            //printf("Transmission failed for slave at address 0x%02X\r\n", next_address);
            bRet = false;
            break;
        }

        HAL_Delay(50);
    }

    return bRet;

}

// Phase 2: broadcast OW_CMD_CLEAR_CONFIG down the chain. Each *configured* node
// relays it downstream (in comms_onewire_check_received) and then drops its own
// enumeration state, returning to the "waiting for discovery" state. This gives a
// clean slate before an enumeration walk and fixes the master-reboot desync where
// stale slaves relay discovery instead of re-claiming an address.
void clear_chain_config(void)
{
    memset((void*)&ow_send_packet, 0, sizeof(ow_send_packet));
    ow_send_packet.packet_type = OW_ONE_WIRE;
    ow_send_packet.command = OW_CMD_CLEAR_CONFIG;
    ow_send_packet.reserved = 0;
    ow_send_packet.addr = 0;

    if (comms_callout_onewire_send(&ow_send_packet))
    {
        // Drain the propagated response so the half-duplex bus returns to idle.
        // The value is irrelevant (OW_TIMEOUT from the terminal node is expected);
        // clearing is idempotent and best-effort.
        comms_callout_onewire_receive(&ow_receive_packet);
    }
}

// Phase 2: master-only on-demand re-enumeration. Resets the local module table,
// clears stale state on every downstream node, waits for them to re-ready, then
// re-runs the discovery walk. Returns the resulting module count (incl. master).
uint8_t master_reenumerate(void)
{
    ModuleManager_RegisterMaster(0x00);   // collapse table back to just the master
    // Idempotent discovery walk: configured slaves re-confirm their module_id and
    // keep their I2C address (no teardown); fresh nodes (incl. bootloader-mode)
    // claim a new address. Robust against master-only reboots and repeated calls.
    enumerate_slaves();                   // (re)assignment (0x20, 0x21, ... + modes)
    return get_module_count();
}

// Callback functions
void comms_handle_ow_CallIn_RxEventCallback(const UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == CALL_IN_UART.Instance) {
        // Notify the task
    	rx_ow_callin_flag = 1;
    }
}

void comms_handle_ow_CallIn_TxCpltCallback(const UART_HandleTypeDef *huart)
{
    if (huart->Instance == CALL_IN_UART.Instance) {
        // Notify the task
    	tx_ow_callin_flag = 1;
    }
}


void comms_handle_ow_CallOut_RxEventCallback(const UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == CALL_OUT_UART.Instance) {
        // Notify the task
    	rx_ow_callout_flag = 1;
    }
}

void comms_handle_ow_CallOut_TxCpltCallback(const UART_HandleTypeDef *huart)
{
    if (huart->Instance == CALL_OUT_UART.Instance) {
        // Notify the task
    	tx_ow_callout_flag = 1;
    }
}

void CDC_handle_RxCpltCallback(uint16_t len) {
	rx_flag = 1;
}

void CDC_handle_TxCpltCallback() {
	tx_flag = 1;
}


// cppcheck-suppress constParameterPointer -- must match the HAL weak callback signature (non-const UART_HandleTypeDef *)
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {

    if (huart->Instance == CALL_OUT_UART.Instance) {

    }
}

void pulsetrain_complete_callback(uint32_t curr_count, uint32_t total_count) {
	// ========== AUTO-CYCLE CHECK ==========
	// If auto-cycle is active and this is the last train of the sequence,
	// request the next profile apply. Otherwise continue normally.
	extern bool auto_cycle_is_active(void);
	extern void auto_cycle_request_profile_apply(void);
	
	if (auto_cycle_is_active() && curr_count >= total_count) {
		// This trigger sequence is done. Request next profile apply.
		auto_cycle_request_profile_apply();
		// Early exit; don't send async status packet
		return;
	}
	
	if(async_enabled){
		int tx_temp_int = (int)(tx_temperature * 10);  // e.g. 32.6 → 326
		int amb_temp_int = (int)(ambient_temperature * 10);  // e.g. 32.6 → 326

        // int tx2_temp_int = (int)(tx2_temperature * 10);  // e.g. 32.6 → 326
        // int amb2_temp_int = (int)(ambient2_temperature * 10);  //

        // Format full status string
        int len = snprintf((char*)owDataBuffer, sizeof(owDataBuffer),
            "STATUS:RUNNING,"
            "MODE:%s,"
            "PULSE_TRAIN:[%lu/%lu],"
            "PULSE:[%lu/%lu],"
            "TEMP_TX:%d.%d,"
            "TEMP_AMBIENT:%d.%d",
			get_trigger_mode_str(),
            (unsigned long)curr_count,
            (unsigned long)total_count,
            (unsigned long)0,
            (unsigned long)0,
			tx_temp_int/10,
			abs(tx_temp_int % 10),
			amb_temp_int/10,
			abs(amb_temp_int % 10)
        );

        if (len < 0 || len >= sizeof(owDataBuffer)) {
            // Handle truncation or error
            len = 0;
        }

		// Called after pulse trains are done
		ow_data_packet.packet_type = OW_DATA;
		ow_data_packet.id = 0;
		ow_data_packet.addr = 0;
		ow_data_packet.reserved = 0;
		ow_data_packet.data_len = len;
		ow_data_packet.data = owDataBuffer;
		comms_interface_send(&ow_data_packet);
	}

}

// STATUS:RUNNING,MODE:SEQUENCE,PULSE_TRAIN:[2/5],PULSE:[3/10],TEMP_TX:32.6,TEMP_AMBIENT:29.1
void sequence_complete_callback(uint32_t total_count) {
	// ========== AUTO-CYCLE CHECK ==========
	// If auto-cycle is active, request next profile apply.
	extern bool auto_cycle_is_active(void);
	extern void auto_cycle_request_profile_apply(void);
	
	if (auto_cycle_is_active()) {
		auto_cycle_request_profile_apply();
		// Early exit; don't send async status packet
		return;
	}

	if(async_enabled){

		int tx_temp_int = (int)(tx_temperature * 10);  // e.g. 32.6 → 326
		int amb_temp_int = (int)(ambient_temperature * 10);  // e.g. 32.6 → 326

        // Format full status string
        int len = snprintf((char*)owDataBuffer, sizeof(owDataBuffer),
            "STATUS:STOPPED,"
            "MODE:%s,"
            "PULSE_TRAIN:[%lu/%lu],"
            "PULSE:[%lu/%lu],"
            "TEMP_TX:%d.%d,"
            "TEMP_AMBIENT:%d.%d",
			get_trigger_mode_str(),
            (unsigned long)total_count,
            (unsigned long)total_count,
            (unsigned long)0,
            (unsigned long)0,
			tx_temp_int/10,
			abs(tx_temp_int % 10),
			amb_temp_int/10,
			abs(amb_temp_int % 10)
        );

        if (len < 0 || len >= sizeof(owDataBuffer)) {
            // Handle truncation or error
            len = 0;
        }

		// Called after pulse trains are done
		ow_data_packet.packet_type = OW_DATA;
		ow_data_packet.id = 0;
		ow_data_packet.addr = 0;
		ow_data_packet.reserved = 0;
		ow_data_packet.data_len = len;
		ow_data_packet.data = owDataBuffer;
		comms_interface_send(&ow_data_packet);
	}
}

void pulse_complete_callback(uint32_t curr_count, uint32_t total_count) {
    // Called after pulse is complete not supported
}
