/*
 * if_commands.c
 *
 *  Created on: Nov 15, 2024
 *      Author: GeorgeVigelette
 */

#include "main.h"
#include "if_commands.h"
#include "common.h"
#include "module_manager.h"
#include "uart_comms.h"
#include "i2c_master.h"
#include "i2c_slave.h"
#include "i2c_protocol.h"
#include "trigger.h"
#include "tx7332.h"
#include "demo.h"
#include "thermistor.h"
#include "lifu_config.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define COMMS_TIMEOUT 250

extern bool _enter_dfu;
extern bool _force_stm32_dfu; // for testing purposes, forces to enter STM32 system bootloader instead of custom DFU mode

extern TX7332 transmitters[2];
extern bool async_enabled;

static uint32_t id_words[3] = {0};
static char retTriggerJson[0xFF];
static uint8_t module_count = 0;
static uint8_t node_mode_byte = NODE_MODE_APP;   // this node runs application firmware
static uint8_t module_mode_byte = 0;             // scratch for OW_CTRL_GET_MODULE_MODE reply

uint8_t send_buff[I2C_BUFFER_SIZE] = {0};
uint8_t receive_buffer[I2C_BUFFER_SIZE] = {0};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))



// OW_CTRL_SET_PROFILE_CYCLE payload: [profile_count:1][n_chips:1][exec_order_len:1]
// header, then the execution order bytes, then one little-endian uint32
// apodization register per chip per profile.
#define PROFILE_CYCLE_HEADER_LEN 3

// Per-chip record of which delay/pattern profiles have been written, plus
// their popcounts. Doubles as the OW_TX7332_STATUS response payload, so the
// field order (counts first, then masks) is the wire format.
typedef struct {
	uint8_t delay_profile_count;
	uint8_t pattern_profile_count;
	uint16_t delay_profiles_mask;
	uint32_t pattern_profiles_mask;
} TxProfileCache;

// Stores execution_order and apodization data for multi-profile auto-cycling.
typedef struct {
	uint8_t profile_count;                 
	uint8_t exec_order_len;                
	uint8_t execution_order[MAX_PROFILES]; 
	uint8_t current_exec_index;             
	bool is_configured;                     
} ProfileCycleConfig;

static ProfileCycleConfig profile_cycle = {0};

static TxProfileCache tx_profile_cache[TX_PER_MODULE] = {0};
static uint8_t selected_profile_response = 0;

// Profiles are 1-based in the host API and mapped to 0-based selector fields on-chip.
static bool IsValidProfile(uint8_t profile)
{
	return (profile >= 1U) && (profile <= MAX_PROFILES);
}

static void cache_profiles_from_register_range(uint8_t tx_idx, uint16_t start_addr, uint8_t reg_count)
{
	if (tx_idx >= TX_PER_MODULE || reg_count == 0U) {
		return;
	}

	TxProfileCache *cache = &tx_profile_cache[tx_idx];
	for (uint16_t i = 0; i < (uint16_t)reg_count; i++) {
		uint16_t addr = (uint16_t)(start_addr + i);

		if (addr >= TX7332_DELAY_DATA_START && addr <= TX7332_DELAY_DATA_END) {
			uint8_t delay_profile = (uint8_t)(((addr - TX7332_DELAY_DATA_START) / TX7332_DELAY_PROFILE_OFFSET) + 1U);
			if (IsValidProfile(delay_profile)) {
				cache->delay_profiles_mask |= (uint16_t)(1U << (delay_profile - 1U));
			}
		} else if (addr >= TX7332_PATTERN_DATA_START && addr <= TX7332_PATTERN_DATA_END) {
			uint8_t pattern_profile = (uint8_t)(((addr - TX7332_PATTERN_DATA_START) / TX7332_PATTERN_PROFILE_OFFSET) + 1U);
			if (IsValidProfile(pattern_profile)) {
				cache->pattern_profiles_mask |= (uint32_t)(1UL << (pattern_profile - 1U));
			}
		}
	}

	cache->delay_profile_count = __builtin_popcount(cache->delay_profiles_mask);
	cache->pattern_profile_count = __builtin_popcount(cache->pattern_profiles_mask);
}

// Return the next 1-based profile index from execution_order, advancing
// current_exec_index and wrapping back to the start of the order.
static uint8_t get_next_profile_in_cycle(void)
{
	if (!profile_cycle.is_configured || profile_cycle.exec_order_len == 0) {
		return 1; // Default to profile 1 if no cycle configured
	}

	uint8_t next_index = profile_cycle.current_exec_index;
	profile_cycle.current_exec_index = (profile_cycle.current_exec_index + 1) % profile_cycle.exec_order_len;

	return profile_cycle.execution_order[next_index];
}

// Apply the next profile in the cycle (delay + pattern + apodization) to all
// configured TX chips and commit it. Called from ISR context (the LORES_TIMER
// compare interrupt in trigger.c, during inter-pulse dead time) and from the
// START_SWTRIG handler before the trigger starts. Returns true on success.
bool apply_next_profile_in_cycle(void)
{
	if (!profile_cycle.is_configured || profile_cycle.exec_order_len == 0) {
		return false;
	}

	uint8_t next_profile = get_next_profile_in_cycle();
	if (!IsValidProfile(next_profile)) {
		return false;
	}

	// Apply delay profile, pattern profile (0-based selector on-chip), and
	// apodization to all TX chips, then commit with the self-clearing LOAD_PROF bit.
	uint8_t tx_count = get_tx_chip_count();
	for (uint8_t txi = 0; txi < tx_count; txi++) {
		TX7332_SetActiveDelayProfile(next_profile, &transmitters[txi], txi);
		TX7332_WriteReg(&transmitters[txi], PATTERN_PROFILE_SELECT_REG_G1, (next_profile - 1U) & PATTERN_PROFILE_SELECT_MASK);
		TX7332_WriteReg(&transmitters[txi], PATTERN_PROFILE_SELECT_REG_G2, (next_profile - 1U) & PATTERN_PROFILE_SELECT_MASK);
		TX7332_LoadProfile(&transmitters[txi]);
	}

	return true;
}

// Reset the profile cycle to the beginning and apply the first profile. Called
// at pulse train boundaries so each train independently cycles all profiles.
void reset_profile_cycle_to_start(void)
{
	if (!profile_cycle.is_configured || profile_cycle.exec_order_len == 0) return;

	profile_cycle.current_exec_index = 0;
	apply_next_profile_in_cycle();
}

static void process_i2c_read_buffer(UartPacket *uartResp, UartPacket* cmd, uint8_t module_id);
static void process_i2c_forward(UartPacket *uartResp, UartPacket* cmd, uint8_t module_id);

static void print_uart_packet(const UartPacket* packet) {
    printf("ID: 0x%04X\r\n", packet->id);
    printf("Packet Type: 0x%02X\r\n", packet->packet_type);
    printf("Command: 0x%02X\r\n", packet->command);
    printf("Data Length: %d\r\n", packet->data_len);
    printf("CRC: 0x%04X\r\n", packet->crc);
    printf("Data: ");
    for (int i = 0; i < packet->data_len; i++) {
        printf("0x%02X ", packet->data[i]);
    }
    printf("\r\n");
}


static void process_i2c_read_buffer(UartPacket *uartResp, UartPacket* cmd, uint8_t module_id)
{
	uint16_t rx_len = 0;
	uint8_t slave_addr = ModuleManager_GetModule(module_id)->i2c_address;
	I2C_TX_Packet ret_i2c_packet;

	memset(receive_buffer, 0, I2C_BUFFER_SIZE);
	if(module_id == 0){
		printf("No Module found\r\n");
		uartResp->id = cmd->id;
		uartResp->packet_type = OW_ERROR;
		uartResp->command = cmd->command;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		return;
	}else{
		uartResp->id = cmd->id;
		uartResp->packet_type = cmd->packet_type;
		uartResp->command = cmd->command;
		if (cmd->packet_type == OW_TX7332){
			if (cmd->command == OW_TX7332_RREG) {
				uartResp->data_len = 4;
			} else {
				uartResp->data_len = 0;
			}
		} else {
			uartResp->data_len = cmd->data_len;
		}
	}

	rx_len = read_buffer_of_slave_global(slave_addr, receive_buffer, uartResp->data_len);
	printf("Received %d Bytes \r\n", rx_len);
	if(i2c_packet_fromBuffer(receive_buffer, &ret_i2c_packet)){
		uartResp->packet_type = ret_i2c_packet.reserved;
		uartResp->data_len = ret_i2c_packet.data_len;
		uartResp->data = (uint8_t *)ret_i2c_packet.pData;
	} else {
		uartResp->packet_type = OW_ERROR;
	}
}

static void process_i2c_forward(UartPacket *uartResp, UartPacket* cmd, uint8_t module_id)
{
	I2C_TX_Packet send_i2c_packet;
	uint16_t send_len = 0;
	uint8_t slave_addr = 0;
	int local_tx_idx = 0;

	if(module_id == 0){
		uartResp->id = cmd->id;
		uartResp->command = cmd->command;
		uartResp->packet_type = OW_ERROR;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		return;
	}

	memset(send_buff, 0, I2C_BUFFER_SIZE);

	slave_addr = ModuleManager_GetModule(module_id)->i2c_address;

	/* For TX7332 commands cmd->addr is the global TX chip index, so we compute
	 * the local chip index within the slave.  For all other packet types
	 * (HWID, PING, VERSION, USR_CFG, etc.) cmd->addr is the module index and
	 * local_tx_idx is unused / irrelevant on the slave, so default to 0. */
	if (cmd->packet_type == OW_TX7332) {
		local_tx_idx = cmd->addr - (module_id * TX_PER_MODULE);
	} else {
		local_tx_idx = 0;
	}

	if(cmd->packet_type == OW_TX7332 && (local_tx_idx<0 || local_tx_idx>1)){
		uartResp->packet_type = OW_ERROR;
		uartResp->command = cmd->command;
	}else {
		if(cmd->data_len > DATA_MAX_SIZE){
			uartResp->packet_type = OW_ERROR;
			uartResp->command = cmd->command;
			uartResp->data_len = 0;
			uartResp->data = NULL;
			return;
		}
		// relay to one of the slaves
		send_i2c_packet.id = cmd->id;
		send_i2c_packet.cmd = cmd->command;
		/* For TX7332 packets the reserved field carries the local chip index.
		 * For all other packet types (PING, VERSION, HWID, USR_CFG, etc.) pass
		 * the original reserved value through so read/write mode is preserved. */
		send_i2c_packet.reserved = (cmd->packet_type == OW_TX7332)
		                           ? (uint8_t)local_tx_idx
		                           : cmd->reserved;
		send_i2c_packet.data_len = cmd->data_len;
		send_i2c_packet.pData = cmd->data;

		send_len = i2c_packet_toBuffer(&send_i2c_packet, send_buff);  // rebuild buffer

		if(send_buffer_to_slave_global(slave_addr, send_buff, send_len) != 0) { // send buffer to slave
			uartResp->packet_type = OW_ERROR;
		}else{
			/* DFU and soft-reset commands cause the slave to reboot immediately
			 * after processing.  Attempting an I2C readback on a resetting slave
			 * would block in HAL_I2C_Mem_Read (COMMS_TIMEOUT) then call
			 * Error_Handler, disabling IRQs and spinning until the IWDG fires
			 * (~3.4 s) — which reboots the master.  Skip the readback and return
			 * success: the slave has already accepted the command. */
			if (cmd->command == OW_CMD_DFU || cmd->command == OW_CMD_RESET) {
				uartResp->command  = cmd->command;
				uartResp->addr     = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = 0;
			} else {
				/* USR_CFG write involves a flash erase+program cycle on the slave
				 * which can take up to ~300 ms; give it enough time to finish. */
				uint32_t wait_ms = (cmd->command == OW_CMD_USR_CFG && cmd->reserved == 1) ? 400U : 50U;
				uint32_t _t0 = HAL_GetTick();
				while ((HAL_GetTick() - _t0) < wait_ms) { /* wait for slave to process */ }
				process_i2c_read_buffer(uartResp, cmd, module_id);
			}
		}
	}	
}

static void ONE_WIRE_ProcessCommand(UartPacket *uartResp, UartPacket *cmd)
{
	uint8_t module_id = 0;

	switch (cmd->command)
	{
		case OW_CMD_PING:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			break;
		case OW_CMD_PONG:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			break;
		case OW_CMD_VERSION:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
            uartResp->data_len = strlen(FW_VERSION_STRING);
            uartResp->data = (uint8_t*)FW_VERSION_STRING;
			break;
		case OW_CMD_ECHO:
			// exact copy
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = cmd->data_len;
			uartResp->data = cmd->data;
			break;
		case OW_CMD_TOGGLE_LED:
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			HAL_GPIO_TogglePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin); //no led pins declared
			break;
		case OW_CMD_HWID:
			uartResp->command = OW_CMD_HWID;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			id_words[0] = HAL_GetUIDw0();
			id_words[1] = HAL_GetUIDw1();
			id_words[2] = HAL_GetUIDw2();
			uartResp->data_len = sizeof(id_words);
			uartResp->data = (uint8_t *)&id_words;
			break;
		case OW_CMD_GET_TEMP:
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			uartResp->data_len = 4;
			uartResp->data = (uint8_t *)&tx_temperature;
			break;
		case OW_CMD_GET_AMBIENT:
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			uartResp->data_len = 4;
			uartResp->data = (uint8_t *)&ambient_temperature;
			break;
		case OW_CMD_DISCOVERY:
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			if(cmd->reserved==0 || cmd->addr < 0x20 || cmd->addr > 0x25){
				printf("Error reserved or addr wrong\r\n");
				uartResp->packet_type = OW_ERROR;
				break;
			}

			// Idempotent (re)claim. Three cases:
			//   - unconfigured                         -> claim this address.
			//   - configured, discovery for OUR id     -> re-confirm (keep address,
			//        adopt the master's address if it somehow changed).
			//   - configured, discovery for another id -> we could not relay it
			//        (no downstream) -> tell the master the chain ended.
			// This lets a master reboot re-enumerate a still-configured chain with
			// no teardown: each slave just re-affirms its existing position.
			if(get_configured() && get_module_ID() != 0 && cmd->reserved != get_module_ID()) {
				uartResp->packet_type = OW_TIMEOUT;
				break;
			}

			if(get_configured() && get_slave_addres() != cmd->addr) {
				// Rare: re-claim with a changed address -> move the I2C listener now
				// (the boot inner-loop only (re)inits I2C while unconfigured).
				set_slave_address(cmd->addr);
				I2C_Slave_Init(cmd->addr);
			} else {
				set_slave_address(cmd->addr);
			}
			set_configured(true);
			set_module_ID(cmd->reserved);
			// Report our operating mode (application firmware) so the master can
			// tell app nodes from bootloader nodes during enumeration.
			uartResp->data_len = 1;
			uartResp->data = &node_mode_byte;
			break;
		case OW_CMD_CLEAR_CONFIG:
			// Drop any stale enumeration state so the master can re-assign a fresh
			// I2C address. Configured nodes relay + clear in comms_onewire_check_received;
			// reaching here means we are an unconfigured (or terminal) node -> just ACK.
			uartResp->id = cmd->id;
			uartResp->command = cmd->command;
			set_configured(false);
			set_module_ID(0);
			set_slave_address(0);
			uartResp->data_len = 0;
			break;
        case OW_CMD_USR_CFG:
            // reserved == 0: READ
            // reserved == 1: WRITE (cmd->data is JSON text)
			uartResp->id = cmd->id;
			uartResp->command = OW_CMD_USR_CFG;
            if (cmd->reserved == 0) {
                const uint8_t *wire_buf = NULL;
                uint16_t wire_len = 0;
                const uint16_t max_payload = (uint16_t)(DATA_MAX_SIZE); 
                if (lifu_cfg_wire_read(&wire_buf, &wire_len, max_payload) != HAL_OK || wire_buf == NULL) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                uartResp->data_len = wire_len;
                uartResp->data = (uint8_t *)wire_buf;
            }
            else if (cmd->reserved == 1) {
                if (cmd->data == NULL || cmd->data_len == 0) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                if (lifu_cfg_wire_write(cmd->data, cmd->data_len) != HAL_OK) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                // Return the updated header as an ACK payload.
                const uint8_t *wire_buf = NULL;
                uint16_t wire_len = 0;
                const uint16_t max_payload = (uint16_t)(DATA_MAX_SIZE);
                if (lifu_cfg_wire_read(&wire_buf, &wire_len, max_payload) != HAL_OK || wire_buf == NULL) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }
                uartResp->data_len = (uint16_t)sizeof(lifu_cfg_wire_hdr_t);
                uartResp->data = (uint8_t *)wire_buf;
            }
            else {
                uartResp->packet_type = OW_ERROR;
                uartResp->data_len = 0;
                uartResp->data = NULL;
            }
            break;
		case OW_CMD_RESET:
			uartResp->id = cmd->id;
			uartResp->command = OW_CMD_RESET;
			module_id = ModuleManager_GetModuleIndex(cmd->addr);
			if (module_id == 0x00){
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = 0;

		    __HAL_LPTIM_CLEAR_FLAG(&RESET_TIMER, LPTIM_FLAG_ARRM | LPTIM_FLAG_CMPM |
		                                          LPTIM_FLAG_EXTTRIG | LPTIM_FLAG_DOWN |
		                                          LPTIM_FLAG_UP    | LPTIM_FLAG_ARROK);
			if( HAL_LPTIM_Counter_Start_IT(&RESET_TIMER, 1500000) != HAL_OK){
					uartResp->packet_type = OW_ERROR;
				}
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_DFU:
			uartResp->id = cmd->id;
			uartResp->command = OW_CMD_DFU;
			module_id = ModuleManager_GetModuleIndex(cmd->addr);
			if (module_id == 0x00){
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = 0;

				_enter_dfu = true;

				__HAL_LPTIM_CLEAR_FLAG(&RESET_TIMER, LPTIM_FLAG_ARRM | LPTIM_FLAG_CMPM |
													LPTIM_FLAG_EXTTRIG | LPTIM_FLAG_DOWN |
													LPTIM_FLAG_UP    | LPTIM_FLAG_ARROK);
				if(HAL_LPTIM_Counter_Start_IT(&RESET_TIMER, 1500000) != HAL_OK){
					uartResp->packet_type = OW_ERROR;
				}
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		default:
			uartResp->addr = 0;
			uartResp->reserved = 0;
			uartResp->data_len = 0;
			uartResp->reserved = OW_INVALID_PACKET;
			uartResp->packet_type = OW_ERROR;
			break;
	}
}

static void CONTROLLER_ProcessCommand(UartPacket *uartResp, UartPacket* cmd)
{
	uint8_t module_id = 0;
	if (cmd->addr >= get_module_count()) {
		uartResp->packet_type = OW_ERROR;
		return;
	}else{
		module_id = (uint8_t)cmd->addr;
	}

	switch (cmd->command)
	{
		case OW_CMD_PING:
			if (module_id == 0x00){
				uartResp->command = cmd->command;
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_VERSION:
			cmd->data_len = strlen(FW_VERSION_STRING); //passing amount to read if forwarding to slave
			if (module_id == 0x00){
				uartResp->command = cmd->command;
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = strlen(FW_VERSION_STRING);
				uartResp->data = (uint8_t*)FW_VERSION_STRING;
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_ECHO:
			if (module_id == 0x00){
				// exact copy
				uartResp->id = cmd->id;
				uartResp->command = cmd->command;
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = cmd->data_len;
				uartResp->data = cmd->data;
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_TOGGLE_LED:
			if (module_id == 0x00)
			{
				uartResp->data_len = 0;
				uartResp->id = cmd->id;
				uartResp->command = cmd->command;
				HAL_GPIO_TogglePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin); 
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_HWID:
			cmd->data_len = HW_ID_DATA_LENGTH; //passing amount to read if forwarding to slave
			if (module_id == 0x00)
			{
				uartResp->id = cmd->id;
				uartResp->command = cmd->command;
				uartResp->reserved = cmd->reserved;
				uartResp->addr = cmd->addr;
				id_words[0] = HAL_GetUIDw0();
				id_words[1] = HAL_GetUIDw1();
				id_words[2] = HAL_GetUIDw2();
				uartResp->data_len = HW_ID_DATA_LENGTH;
				uartResp->data = (uint8_t *)&id_words;
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_GET_TEMP:
			cmd->data_len = TEMPERATURE_DATA_LENGTH; //passing amount to read if forwarding to slave
			if (module_id == 0){
				uartResp->id = cmd->id;
				uartResp->command = cmd->command;
				uartResp->data_len = TEMPERATURE_DATA_LENGTH;
				uartResp->data = (uint8_t *)&tx_temperature;
			}else{
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_GET_AMBIENT:
			cmd->data_len = TEMPERATURE_DATA_LENGTH; //passing amount to read if forwarding to slave
			if (module_id == 0){
				uartResp->id = cmd->id;
				uartResp->command = cmd->command;
				uartResp->data_len = TEMPERATURE_DATA_LENGTH;
				uartResp->data = (uint8_t *)&ambient_temperature;
			}else{
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CTRL_START_SWTRIG:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;
			
			// Check if auto-cycle should be enabled
			if (profile_cycle.is_configured && profile_cycle.exec_order_len > 0) {
				// Validate: pulse_count must be divisible by number of profiles
				uint32_t pulse_count = get_trigger_pulse_count();
				uint8_t n_profiles = profile_cycle.exec_order_len;

				if (pulse_count == 0 || (pulse_count % n_profiles) != 0) {
					printf("[AUTO_CYCLE] ERROR: pulse_count %lu not divisible by %u profiles\r\n",
					       pulse_count, n_profiles);
					uartResp->packet_type = OW_ERROR;
					break;
				}

				uint32_t pulses_per_profile = pulse_count / n_profiles;

				// Apply the first profile immediately before starting
				profile_cycle.current_exec_index = 0;
				if (!apply_next_profile_in_cycle()) {
					uartResp->packet_type = OW_ERROR;
					break;
				}

				// Start pulse-level auto-cycle
				if (!tx_overheat_flag) {
					auto_cycle_start(pulses_per_profile);
					if (start_trigger_pulse() != TRIGGER_STATUS_RUNNING) {
						uartResp->packet_type = OW_ERROR;
						auto_cycle_stop();
					}
				}
			} else {
				// Normal mode: single trigger sequence
				if(!tx_overheat_flag && start_trigger_pulse() != TRIGGER_STATUS_RUNNING) {
					uartResp->packet_type = OW_ERROR;
				}
			}
			break;
		case OW_CTRL_STOP_SWTRIG:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;
			if(stop_trigger_pulse() != TRIGGER_STATUS_READY)
			{
				uartResp->packet_type = OW_ERROR;
			}
			
			// Stop auto-cycle if active
			if (auto_cycle_is_active()) {
				auto_cycle_stop();
			}
			break;
		case OW_CTRL_SET_SWTRIG:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			if (module_id != 0){
				// trigger is only on master
				uartResp->packet_type = OW_ERROR;
				uartResp->data = NULL;
				break;
			}

			// Clear any current trigger configs. Multi-profile solutions send
			// OW_CTRL_SET_PROFILE_CYCLE again after this command.
			auto_cycle_stop();
			profile_cycle.is_configured = false;
			profile_cycle.exec_order_len = 0;
			profile_cycle.current_exec_index = 0;

			if(!set_trigger_data((char *)cmd->data, cmd->data_len))
			{
				uartResp->packet_type = OW_ERROR;
			}else{
				// refresh state
				if(!get_trigger_data(retTriggerJson, 0xFF))
				{
					uartResp->packet_type = OW_ERROR;
				}else{
					uartResp->data_len = strlen(retTriggerJson);
					uartResp->data = (uint8_t *)retTriggerJson;
				}
			}

			break;
		case OW_CTRL_GET_SWTRIG:
			// refresh state
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			
			if (module_id != 0){
				// trigger is only on master
				uartResp->packet_type = OW_ERROR;
				uartResp->data = NULL;
				break;
			}

			if(!get_trigger_data(retTriggerJson, 0xFF))
			{
				uartResp->packet_type = OW_ERROR;
				break;
			}

			uartResp->data_len = strlen(retTriggerJson);
			uartResp->data = (uint8_t *)retTriggerJson;
			break;
		case OW_CTRL_GET_MODULE_COUNT:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;

			if (module_id == 0){
				module_count = get_module_count();
				uartResp->data_len = 1;
				uartResp->data = (uint8_t *)&module_count;
			}else{
                uartResp->packet_type = OW_ERROR;
                uartResp->data_len = 0;
                uartResp->data = NULL;
			}
			break;
		case OW_CTRL_GET_MODULE_MODE:
			// addr = module index (validated < module_count at function entry).
			// Returns the NodeMode (app vs bootloader) recorded during enumeration.
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			module_mode_byte = ModuleManager_GetModuleMode(module_id);
			uartResp->data_len = 1;
			uartResp->data = &module_mode_byte;
			break;
		case OW_CTRL_ENUMERATE:
			// Master-only: re-run the robust enumeration (clear-config broadcast +
			// discovery walk) on demand, then return the new module count.
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			if (module_id == 0){
				module_count = master_reenumerate();
				uartResp->data_len = 1;
				uartResp->data = (uint8_t *)&module_count;
			}else{
				uartResp->packet_type = OW_ERROR;
				uartResp->data_len = 0;
				uartResp->data = NULL;
			}
			break;
        case OW_CMD_USR_CFG:
			if (module_id != 0x00)
			{
				process_i2c_forward(uartResp, cmd, module_id);
				return;
			}
            // reserved == 0: READ
            // reserved == 1: WRITE (cmd->data is JSON text)
            if (cmd->reserved == 0) {
                const uint8_t *wire_buf = NULL;
                uint16_t wire_len = 0;
                const uint16_t max_payload = (uint16_t)(DATA_MAX_SIZE);
                if (lifu_cfg_wire_read(&wire_buf, &wire_len, max_payload) != HAL_OK || wire_buf == NULL) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                uartResp->data_len = wire_len;
                uartResp->data = (uint8_t *)wire_buf;
            }
            else if (cmd->reserved == 1) {
                if (cmd->data == NULL || cmd->data_len == 0) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                if (lifu_cfg_wire_write(cmd->data, cmd->data_len) != HAL_OK) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }

                // Return the updated header as an ACK payload.
                const uint8_t *wire_buf = NULL;
                uint16_t wire_len = 0;
                const uint16_t max_payload = (uint16_t)(DATA_MAX_SIZE);
                if (lifu_cfg_wire_read(&wire_buf, &wire_len, max_payload) != HAL_OK || wire_buf == NULL) {
                    uartResp->packet_type = OW_ERROR;
                    uartResp->data_len = 0;
                    uartResp->data = NULL;
                    break;
                }
                uartResp->data_len = (uint16_t)sizeof(lifu_cfg_wire_hdr_t);
                uartResp->data = (uint8_t *)wire_buf;
            }
            else {
                uartResp->packet_type = OW_ERROR;
                uartResp->data_len = 0;
                uartResp->data = NULL;
            }
            break;
		case OW_CMD_RESET:
			if (module_id == 0x00){
				uartResp->command = cmd->command;
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = 0;

				__HAL_LPTIM_CLEAR_FLAG(&RESET_TIMER, LPTIM_FLAG_ARRM | LPTIM_FLAG_CMPM |
													  LPTIM_FLAG_EXTTRIG | LPTIM_FLAG_DOWN |
													  LPTIM_FLAG_UP    | LPTIM_FLAG_ARROK);
				if( HAL_LPTIM_Counter_Start_IT(&RESET_TIMER, 1500000) != HAL_OK){
						uartResp->packet_type = OW_ERROR;
				}
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_DFU:
			if (module_id == 0x00){
				uartResp->command = cmd->command;
				uartResp->addr = cmd->addr;
				uartResp->reserved = cmd->reserved;
				uartResp->data_len = 0;

				_enter_dfu = true;
				_force_stm32_dfu = cmd->reserved == 0x77? true : false; // force to enter STM32 DFU bootloader instead of custom DFU mode for testing purposes

				__HAL_LPTIM_CLEAR_FLAG(&RESET_TIMER, LPTIM_FLAG_ARRM | LPTIM_FLAG_CMPM |
													LPTIM_FLAG_EXTTRIG | LPTIM_FLAG_DOWN |
													LPTIM_FLAG_UP    | LPTIM_FLAG_ARROK);
				if(HAL_LPTIM_Counter_Start_IT(&RESET_TIMER, 1500000) != HAL_OK){
					uartResp->packet_type = OW_ERROR;
				}
			} else {
				process_i2c_forward(uartResp, cmd, module_id);
			}
			break;
		case OW_CMD_ASYNC:
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			
			if (module_id != 0){
				// trigger is only on master
				uartResp->packet_type = OW_ERROR;
				uartResp->data = NULL;
				break;
			}
			
			if(cmd->data_len == 1){
				async_enabled = cmd->data[0] == 1? true: false;
			}
			uartResp->reserved = async_enabled?1:0;
			uartResp->data_len = 0;
			break;
		case OW_CTRL_SET_PATTERN_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			if (cmd->data_len < 1U || cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint8_t profile = cmd->data[0];
			if (!IsValidProfile(profile)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// Pattern profile selector is 0-based in the TX7332 registers.
			TX7332_WriteReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G1, (profile - 1U) & PATTERN_PROFILE_SELECT_MASK);
			TX7332_WriteReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G2, (profile - 1U) & PATTERN_PROFILE_SELECT_MASK);

			// Commit selector changes on-chip (self-clearing LOAD_PROF bit).
			TX7332_LoadProfile(&transmitters[cmd->addr]);

			break;
		}
		case OW_CTRL_GET_PATTERN_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			if (cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint32_t pattern_sel_g1 = TX7332_ReadReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G1);
			uint32_t pattern_sel_g2 = TX7332_ReadReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G2);

			uint8_t pattern_g1 = (uint8_t)(pattern_sel_g1 & PATTERN_PROFILE_SELECT_MASK);
			uint8_t pattern_g2 = (uint8_t)(pattern_sel_g2 & PATTERN_PROFILE_SELECT_MASK);

			// Pattern selector is 0-based in hardware; convert to 1-based for host.
			if ((pattern_g1 != pattern_g2) || !IsValidProfile(pattern_g1 + 1U)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			selected_profile_response = pattern_g1 + 1U;
			uartResp->data = &selected_profile_response;
			uartResp->data_len = 1;
			break;
		}
		case OW_CTRL_SET_DELAY_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			if (cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			if (cmd->data_len < 1U) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint8_t profile = *((uint8_t *)cmd->data);
			if (!IsValidProfile(profile)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			for (uint8_t i = 0; i < get_tx_chip_count(); i++) {
				TX7332_SetActiveDelayProfile(profile, &transmitters[i], i);
			}

			break;
		}
		case OW_CTRL_GET_DELAY_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			if (cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint8_t profile = 0U;
			if (!TX7332_GetActiveDelayProfile(&transmitters[cmd->addr], &profile) ||
				!IsValidProfile(profile)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			selected_profile_response = profile;
			uartResp->data = &selected_profile_response;
			uartResp->data_len = 1;
			break;
		}
		case OW_CTRL_SET_PROFILE_CYCLE:
		{
			// Receives execution_order and pre-computed apodization registers
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			// Gating: Prevent cycle config changes while auto-cycle is running
			if (auto_cycle_is_active()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			if (cmd->data_len < PROFILE_CYCLE_HEADER_LEN) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint8_t *payload = (uint8_t *)cmd->data;
			uint8_t n_profiles = payload[0];
			uint8_t n_chips = payload[1];
			uint8_t exec_order_len = payload[2];

			// Validate ranges
			if (n_profiles < 1 || n_profiles > MAX_PROFILES ||
				n_chips < 1 || n_chips > TX_PER_MODULE ||
				exec_order_len < 1 || exec_order_len > MAX_PROFILES) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint16_t expected_size = PROFILE_CYCLE_HEADER_LEN + (uint16_t)exec_order_len +
				((uint16_t)n_profiles * (uint16_t)n_chips * (uint16_t)sizeof(uint32_t));
			if (cmd->data_len < expected_size) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			profile_cycle.profile_count = n_profiles;
			profile_cycle.exec_order_len = exec_order_len;
			profile_cycle.current_exec_index = 0;

			// Extract execution_order indices (1-based)
			uint8_t *exec_order_ptr = &payload[PROFILE_CYCLE_HEADER_LEN];
			for (uint8_t i = 0; i < exec_order_len; i++) {
				uint8_t profile_idx = exec_order_ptr[i];
				if (profile_idx < 1 || profile_idx > n_profiles) {
					uartResp->packet_type = OW_ERROR;
					return;
				}
				profile_cycle.execution_order[i] = profile_idx;
			}

			// Extract pre-computed apodization registers per profile per chip (little-endian uint32)
			uint8_t *apod_data_ptr = &payload[PROFILE_CYCLE_HEADER_LEN + exec_order_len];
			for (uint8_t p = 0; p < n_profiles; p++) {
				for (uint8_t c = 0; c < n_chips; c++) {
					uint8_t *reg_ptr = &apod_data_ptr[(p * n_chips + c) * sizeof(uint32_t)];
					apod_registers[p][c] = (uint32_t)reg_ptr[0]
						| ((uint32_t)reg_ptr[1] << 8)
						| ((uint32_t)reg_ptr[2] << 16)
						| ((uint32_t)reg_ptr[3] << 24);
				}
			}

			profile_cycle.is_configured = true;

			// Success response
			break;
		}
		default:
			uartResp->addr = 0;
			uartResp->reserved = OW_INVALID_PACKET;
			uartResp->data_len = 0;
			uartResp->packet_type = OW_ERROR;
			break;
	}

}

static void TX7332_ProcessCommand(UartPacket *uartResp, UartPacket* cmd)
{
	uint8_t module_id = 0;
	uint16_t reg_address = 0;
	uint32_t reg_value = 0;
	static uint32_t reg_data_buff[REG_DATA_LEN];
	int reg_count = 0;

	uartResp->id = cmd->id;
	uartResp->command = cmd->command;

	switch (cmd->command)
	{
	case OW_TX7332_ENUM:
		// send array of tx chip counts 0,1,2,3,4,...
		uartResp->command = OW_TX7332_ENUM;
		uartResp->addr = cmd->addr;
		// Here we will have the array for all tx chips with 0,1 on the controller
		// and 2,3 on the first slave in the chain and so on
		uartResp->reserved = (uint8_t)get_tx_chip_count();
		uartResp->data_len = 0;
		uartResp->data = NULL;
		break;
	case OW_TX7332_DEMO:
		uartResp->command = OW_TX7332_DEMO;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		uartResp->addr = cmd->addr;
		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			write_demo_registers(&transmitters[cmd->addr]);
	    }else{
			process_i2c_forward(uartResp, cmd, module_id);
		}

		uartResp->reserved = (uint8_t)get_tx_chip_count();
		break;
	case OW_TX7332_WREG:
		uartResp->command = OW_TX7332_WREG;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if(cmd->data_len != 6 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			// Unpack 32-bit value (next 4 bytes, little-endian)
			reg_value = cmd->data[2] | (cmd->data[3] << 8) | (cmd->data[4] << 16) | (cmd->data[5] << 24);

			TX7332_WriteReg(&transmitters[cmd->addr], reg_address, reg_value);
			cache_profiles_from_register_range(cmd->addr, reg_address, 1U);
		}
		else
		{
			process_i2c_forward(uartResp, cmd, module_id);
		}

		break;
	case OW_TX7332_RREG:
		uartResp->command = OW_TX7332_RREG;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if(cmd->data_len != 2 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			reg_value = 0;

			reg_value = TX7332_ReadReg(&transmitters[cmd->addr], reg_address);
			memset(reg_data_buff,0,REG_DATA_LEN*sizeof(reg_value));

			// Package response
			reg_data_buff[0] = reg_value;

			uartResp->data_len = sizeof(reg_value);
			uartResp->data = (uint8_t*)reg_data_buff;
		}else{
			process_i2c_forward(uartResp, cmd, module_id);
		}
		break;
	case OW_TX7332_WBLOCK:
		uartResp->command = OW_TX7332_WBLOCK;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if(cmd->data_len <= 6 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_count = cmd->data[2];
			// byte [3] dummy byte
			// Check if the actual data length matches expected length
			if(cmd->data_len != (4 + (4 * reg_count)))
			{
				// printf("Invalid data size does not match \r\n");
				uartResp->packet_type = OW_ERROR;
				break;
			}


			memset(reg_data_buff,0,REG_DATA_LEN*sizeof(reg_value));

			memcpy((uint8_t*)reg_data_buff, &cmd->data[4], sizeof(uint32_t) * reg_count);
			if(!TX7332_WriteBulk(&transmitters[cmd->addr], reg_address, reg_data_buff, reg_count)){
				uartResp->packet_type = OW_ERROR;
				break;
			}
			cache_profiles_from_register_range(cmd->addr, reg_address, (uint8_t)reg_count);
		}else{
			process_i2c_forward(uartResp, cmd, module_id);
		}

		break;
	case OW_TX7332_VWREG:
		uartResp->command = OW_TX7332_VWREG;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if(cmd->data_len != 6 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			// Unpack 32-bit value (next 4 bytes, little-endian)
			reg_value = cmd->data[2] | (cmd->data[3] << 8) | (cmd->data[4] << 16) | (cmd->data[5] << 24);

			if(!TX7332_WriteVerify(&transmitters[cmd->addr], reg_address, reg_value))
			{
				uartResp->packet_type = OW_ERROR;
			} else {
				cache_profiles_from_register_range(cmd->addr, reg_address, 1U);
			}
		}else{
			process_i2c_forward(uartResp, cmd, module_id);
		}

		break;
	case OW_TX7332_VWBLOCK:
		uartResp->command = OW_TX7332_VWBLOCK;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if(cmd->data_len <= 6 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			// Unpack 16-bit address (first 2 bytes, little-endian)
			reg_count = cmd->data[2];
			// byte [3] dummy byte
			// Check if the actual data length matches expected length
			if(cmd->data_len != (4 + (4 * reg_count)))
			{
				// printf("Invalid data size does not match \r\n");
				uartResp->packet_type = OW_ERROR;
				break;
			}


			memset(reg_data_buff,0,REG_DATA_LEN*sizeof(reg_value));

			memcpy((uint8_t*)reg_data_buff, &cmd->data[4], sizeof(uint32_t) * reg_count);
			if(!TX7332_WriteBulkVerify(&transmitters[cmd->addr], reg_address, reg_data_buff, reg_count)){
				uartResp->packet_type = OW_ERROR;
				break;
			}
			cache_profiles_from_register_range(cmd->addr, reg_address, (uint8_t)reg_count);
		}else{
			process_i2c_forward(uartResp, cmd, module_id);
		}
		break;
	case OW_TX7332_STATUS:
		uartResp->command = OW_TX7332_STATUS;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		if (cmd->addr >= get_tx_chip_count()) {
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);
		if (module_id == 0x00) {
			uartResp->data_len = (uint16_t)sizeof(tx_profile_cache[cmd->addr]);
			uartResp->data = (uint8_t *)&tx_profile_cache[cmd->addr];
		} else {
			process_i2c_forward(uartResp, cmd, module_id);
		}
		break;
	case OW_TX7332_RBLOCK:
		uartResp->command = OW_TX7332_RBLOCK;
		uartResp->addr = cmd->addr;
		uartResp->reserved = 0;
		uartResp->data_len = 0;
		uartResp->data = NULL;
		/* Request payload: uint16_t start_addr, uint8_t count, uint8_t reserved */
		if(cmd->data_len != 4 || cmd->addr >= get_tx_chip_count()){
			uartResp->packet_type = OW_ERROR;
			break;
		}

		module_id = ModuleManager_GetModuleIndex(cmd->addr);

		if(module_id == 0x00) // local
		{
			reg_address = cmd->data[0] | (cmd->data[1] << 8);
			reg_count   = cmd->data[2];
			if(reg_count == 0 || reg_count > REG_DATA_LEN){
				uartResp->packet_type = OW_ERROR;
				break;
			}

			memset(reg_data_buff, 0, REG_DATA_LEN * sizeof(uint32_t));
			for(int i = 0; i < reg_count; i++){
				reg_data_buff[i] = TX7332_ReadReg(&transmitters[cmd->addr], reg_address + i);
			}

			uartResp->data_len = (uint16_t)(reg_count * sizeof(uint32_t));
			uartResp->data = (uint8_t*)reg_data_buff;
		}else{
			process_i2c_forward(uartResp, cmd, module_id);
		}
		break;
	case OW_TX7332_DEVICE_COUNT:
	{
		static uint8_t temp_module_count;
		temp_module_count = get_module_count();
		uartResp->command = OW_TX7332_DEVICE_COUNT;
		uartResp->addr = 0;
		uartResp->reserved = 0;
		uartResp->data = &temp_module_count;
		uartResp->data_len = 1;
	}
		break;

	case OW_TX7332_RESET:
		uartResp->command = OW_TX7332_RESET;
		uartResp->addr = 0;
		uartResp->reserved = 0;
		uartResp->data = NULL;
		uartResp->data_len = 0;
		break;
	default:
		uartResp->data_len = 0;
		uartResp->reserved = OW_INVALID_PACKET;
		uartResp->packet_type = OW_ERROR;
		break;
	}
}

bool process_if_command(UartPacket *cmd, UartPacket *resp)
{
	// I2C_TX_Packet i2c_packet;
	(void)print_uart_packet;

	resp->id = cmd->id;
	if(cmd->packet_type == OW_ONE_WIRE){
		resp->packet_type = OW_ONEWIRE_RESP;
	}else{
		resp->packet_type = OW_RESP;
	}
	resp->addr = 0;
	resp->reserved = 0;
	resp->data_len = 0;
	resp->data = 0;
	switch (cmd->packet_type)
	{
	case OW_ONE_WIRE:
		ONE_WIRE_ProcessCommand(resp, cmd);
		break;
	case OW_CMD:
	case OW_CONTROLLER:
		//process by the USTX Controller
		CONTROLLER_ProcessCommand(resp, cmd);
		break;
	case OW_TX7332:
		TX7332_ProcessCommand(resp, cmd);
		break;
	case OW_I2C_PASSTHRU:
	{
		/* Raw I2C passthrough: forward bytes to an I2C slave (typically the
		 * DFU bootloader at 0x72) and optionally read back bytes.
		 *
		 * cmd->addr     = 7-bit I2C slave address
		 * cmd->command  = 0x00  write-only
		 *                 0x01  write then delay 5ms then read
		 * cmd->reserved = number of bytes to read back (command=0x01 only, max 255)
		 * cmd->data     = bytes to write (may be empty for a read-only op)
		 */
		static uint8_t i2c_passthru_rx[256];
		uint8_t i2c_addr = cmd->addr;
		uint8_t do_read  = (cmd->command == 0x01);
		uint8_t rx_count = cmd->reserved;

		resp->command   = cmd->command;
		resp->addr      = cmd->addr;
		resp->reserved  = 0;
		resp->data_len  = 0;
		resp->data      = NULL;

		/* Write phase (skip if no data) */
		if (cmd->data_len > 0) {
			if (send_buffer_to_slave_global(i2c_addr, cmd->data, cmd->data_len) != 0) {
				resp->packet_type = OW_ERROR;
				break;
			}
		}

		/* Optional read phase */
		if (do_read && rx_count > 0) {
			HAL_Delay(5); /* brief gap between write and read */
			memset(i2c_passthru_rx, 0, sizeof(i2c_passthru_rx));
			if (read_raw_from_slave_global(i2c_addr, i2c_passthru_rx, rx_count) != 0) {
				resp->packet_type = OW_ERROR;
				break;
			}
			resp->data_len = rx_count;
			resp->data     = i2c_passthru_rx;
		}
		break;
	}
	default:
		resp->data_len = 0;
		resp->reserved = OW_UNKNOWN_ERROR;
		resp->packet_type = OW_ERROR;
		break;
	}

	return true;

}
