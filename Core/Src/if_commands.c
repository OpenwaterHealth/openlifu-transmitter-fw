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

#define MAX_PROFILES 16
#define NUM_CHANNELS 64
#define TX_APOD_CHANNELS_PER_CHIP 32U
#define TX7332_APODIZATION_REGISTER 0x1BU

static const uint8_t apodization_channel_order_reversed[TX_APOD_CHANNELS_PER_CHIP] = {
	16U, 14U, 12U, 10U, 8U, 6U, 4U, 2U,
	15U, 13U, 11U, 9U, 7U, 5U, 3U, 1U,
	32U, 30U, 28U, 26U, 24U, 22U, 20U, 18U,
	31U, 29U, 27U, 25U, 23U, 21U, 19U, 17U
};

// Match SDK packing: lsb is the index of channel number in APODIZATION_CHANNEL_ORDER_REVERSED.
static uint8_t apodization_lsb_for_channel(uint8_t channel_1based)
{
	for (uint8_t lsb = 0U; lsb < TX_APOD_CHANNELS_PER_CHIP; lsb++) {
		if (apodization_channel_order_reversed[lsb] == channel_1based) {
			return lsb;
		}
	}

	return 0U;
}

// Delay profile RAM starts at 0x20 (16 regs/profile), pattern RAM starts at 0x120 (4 regs/profile).
#define TX7332_DELAY_DATA_START       0x20U
#define TX7332_DELAY_DATA_END         0x11FU
#define TX7332_PATTERN_DATA_START     0x120U
#define TX7332_PATTERN_DATA_END       0x19FU
#define TX7332_DELAY_PROFILE_OFFSET   16U
#define TX7332_PATTERN_PROFILE_OFFSET 4U

typedef struct {
	uint16_t delay_profiles_mask;
	uint32_t pattern_profiles_mask;
	uint8_t delay_profile_count;
	uint8_t pattern_profile_count;
} TxProfileCache;

typedef struct {
	uint8_t delay_profile_count;
	uint8_t pattern_profile_count;
	uint16_t delay_profiles_mask;
	uint32_t pattern_profiles_mask;
} TxProfileStatus;

// ========== GROUPED PROFILE PACKAGE SYSTEM ==========
// Stores execution_order and apodization data for multi-profile auto-cycling.
typedef struct {
	uint8_t profile_count;              // Number of configured profiles (1-16)
	uint8_t apod_channels;              // Apodization channels per profile (e.g., 64)
	uint8_t exec_order_len;             // Length of execution_order array
	uint8_t execution_order[MAX_PROFILES];         // Profile indices to cycle through (1-based)
	uint8_t current_exec_index;          // Current position in execution_order (for auto-cycling)MAX_PROFILES
	bool is_configured;                  // Flag: true if cycle data has been received
} ProfileCycleConfig;

// Cache the apodization rows in MCU RAM so the selected profile can restore its table.
static uint8_t apodization_table[MAX_PROFILES][NUM_CHANNELS];
static uint8_t active_apodization[NUM_CHANNELS];

static ProfileCycleConfig profile_cycle = {
	.profile_count = 0,
	.apod_channels = 0,
	.exec_order_len = 0,
	.current_exec_index = 0,
	.is_configured = false
};

static TxProfileCache tx_profile_cache[TX_PER_MODULE] = {0};
static TxProfileStatus tx_profile_status = {0};
static uint8_t selected_profile_response = 0;

// Pattern profiles are 1 based in the datasheet
static bool IsValidPatternProfile(uint8_t profile)
{
	return (profile >= 0) && (profile <= MAX_NUMBER_OF_PROFILES - 1);
}

// Delay Profiles are 0 based in the datasheet
static bool IsValidDelayProfile(uint8_t profile)
{
	return (profile >= 0) && (profile <= MAX_NUMBER_OF_PROFILES - 1);
}

static uint32_t BuildDelayProfileSelectValue(uint32_t current_reg, uint8_t profile)
{
	uint32_t delay_profile_field = (uint32_t)(profile - 1U) & BF_PROF_SEL_FIELD_MASK;
	uint32_t clear_mask =
		((uint32_t)BF_PROF_SEL_FIELD_MASK << BF_PROF_SEL_G1_SHIFT) |
		((uint32_t)BF_PROF_SEL_FIELD_MASK << BF_PROF_SEL_G2_SHIFT);

	// Preserve all non-profile fields (including TR_SW_DEL fields), update only BF_PROF_SEL bits.
	uint32_t next_reg = current_reg & ~clear_mask;
	next_reg |= (delay_profile_field << BF_PROF_SEL_G1_SHIFT);
	next_reg |= (delay_profile_field << BF_PROF_SEL_G2_SHIFT);
	return next_reg;
}

// currently only doing pattern profiles
static bool ExtractUnifiedProfile(uint32_t delay_select_reg,
								  uint32_t pattern_sel_g1,
								  uint32_t pattern_sel_g2,
								  uint8_t *profile)
{
	uint8_t delay_g1 = (uint8_t)((delay_select_reg >> BF_PROF_SEL_G1_SHIFT) & BF_PROF_SEL_FIELD_MASK);
	uint8_t delay_g2 = (uint8_t)((delay_select_reg >> BF_PROF_SEL_G2_SHIFT) & BF_PROF_SEL_FIELD_MASK);
	uint8_t pattern_g1 = (uint8_t)(pattern_sel_g1 & PATTERN_PROFILE_SELECT_MASK);
	uint8_t pattern_g2 = (uint8_t)(pattern_sel_g2 & PATTERN_PROFILE_SELECT_MASK);

	if (delay_g1 != delay_g2 || pattern_g1 != pattern_g2) {
		return false;
	}

	// Delay select fields are 0-based; pattern fields are 1-based.
	uint8_t delay_profile = (uint8_t)(delay_g1 + 1U);
	if (delay_profile != pattern_g1 || !IsValidDelayProfile(delay_profile)) {
		return false;
	}

	*profile = delay_profile;
	return true;
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
			if (delay_profile >= 1U && delay_profile <= MAX_NUMBER_OF_PROFILES) {
				cache->delay_profiles_mask |= (uint16_t)(1U << (delay_profile - 1U));
			}
		} else if (addr >= TX7332_PATTERN_DATA_START && addr <= TX7332_PATTERN_DATA_END) {
			uint8_t pattern_profile = (uint8_t)(((addr - TX7332_PATTERN_DATA_START) / TX7332_PATTERN_PROFILE_OFFSET) + 1U);
			if (IsValidPatternProfile(pattern_profile)) {
				cache->pattern_profiles_mask |= (uint32_t)(1UL << (pattern_profile - 1U));
			}
		}
	}

	cache->delay_profile_count = __builtin_popcount(cache->delay_profiles_mask);
	cache->pattern_profile_count = __builtin_popcount(cache->pattern_profiles_mask);
}

// ========== GROUPED PROFILE CYCLE HELPERS ==========

/**
 * Initialize the profile cycle configuration.
 * Called once at startup to prepare the auto-cycling infrastructure.
 */
static void init_profile_cycle_config(void)
{
	profile_cycle.profile_count = 0;
	profile_cycle.apod_channels = 0;
	profile_cycle.exec_order_len = 0;
	profile_cycle.current_exec_index = 0;
	profile_cycle.is_configured = false;
	memset(profile_cycle.execution_order, 0, sizeof(profile_cycle.execution_order));
	memset(apodization_table, 0, sizeof(apodization_table));
	memset(active_apodization, 0, sizeof(active_apodization));
}

/**
 * Get the next profile index from the execution_order.
 * Advances current_exec_index and wraps around to 0 when reaching the end.
 * Returns the 1-based profile index to activate next.
 */
static uint8_t get_next_profile_in_cycle(void)
{
	if (!profile_cycle.is_configured || profile_cycle.exec_order_len == 0) {
		return 1; // Default to profile 1 if no cycle configured
	}
	
	uint8_t next_index = profile_cycle.current_exec_index;
	profile_cycle.current_exec_index = (profile_cycle.current_exec_index + 1) % profile_cycle.exec_order_len;
	
	return profile_cycle.execution_order[next_index];
}

/**
 * Apply apodization for the selected profile.
 * Copies the cached row and writes the per-chip apodization register.
 */
static void apply_profile_apodization(uint8_t tx_index, uint8_t profile_index)
{
	if (tx_index >= TX_PER_MODULE || profile_index < 1U || profile_index > MAX_PROFILES) {
		return;
	}

	uint8_t apod_profile = (uint8_t)(profile_index - 1U);
	// Match SDK chip ordering used for 64->(32,32) split: TX index 0 maps to upper half,
	// TX index 1 maps to lower half.
	uint8_t apod_tx_index = tx_index;
	if (TX_PER_MODULE == 2U) {
		apod_tx_index = (uint8_t)((tx_index + 1U) % 2U);
	}
	uint8_t channel_offset = (uint8_t)(apod_tx_index * TX_APOD_CHANNELS_PER_CHIP);
	uint32_t apod_register = 0U;

	memcpy(active_apodization, apodization_table[apod_profile], NUM_CHANNELS);

	// TX7332 apodization is a 32-bit active-low channel mask.
	for (uint8_t channel = 0; channel < TX_APOD_CHANNELS_PER_CHIP; channel++) {
		uint8_t apod_value = apodization_table[apod_profile][channel_offset + channel];
		if (apod_value == 0U) {
			uint8_t lsb = apodization_lsb_for_channel((uint8_t)(channel + 1U));
			apod_register |= (1UL << lsb);
		}
	}

	TX7332_WriteReg(&transmitters[tx_index], TX7332_APODIZATION_REGISTER, apod_register);
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
			if(!tx_overheat_flag && start_trigger_pulse() != TRIGGER_STATUS_RUNNING)
			{
				uartResp->packet_type = OW_ERROR;
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




		////////////////////////////////////////////////////////////////////////////////////////
		////////////////////////////////////////////////////////////////////////////////////////
		////////////////////////////////////////////////////////////////////////////////////////
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
			if (!IsValidPatternProfile(profile)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// Keep TR_SW_DEL timing fields intact while updating delay profile selectors.
			// uint32_t delay_select_reg = TX7332_ReadReg(&transmitters[cmd->addr], DELAY_PROFILE_SELECT_REGISTER); // why do we need to read this first?
			// delay_select_reg = BuildDelayProfileSelectValue(delay_select_reg, profile);
			// TX7332_WriteReg(&transmitters[cmd->addr], DELAY_PROFILE_SELECT_REGISTER, delay_select_reg);

			// Pattern profile fields are direct (1-based) selections for each group.
			TX7332_WriteReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G1, profile & PATTERN_PROFILE_SELECT_MASK);
			TX7332_WriteReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G2, profile & PATTERN_PROFILE_SELECT_MASK);

			// Commit selector changes on-chip (self-clearing LOAD_PROF bit).
			TX7332_LoadProfile(&transmitters[cmd->addr]);

			// Keep the active apodization row aligned with the selected delay profile.
			// apply_profile_apodization(cmd->addr, profile); //need to move to the set delay portion
			break;
		}
		case OW_CTRL_GET_PATTERN_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 1;

			if (cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// uint32_t delay_select_reg = TX7332_ReadReg(&transmitters[cmd->addr], DELAY_PROFILE_SELECT_REGISTER);
			uint32_t pattern_sel_g1 = TX7332_ReadReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G1);
			uint32_t pattern_sel_g2 = TX7332_ReadReg(&transmitters[cmd->addr], PATTERN_PROFILE_SELECT_REG_G2);

			uint8_t pattern_g1 = (uint8_t)(pattern_sel_g1 & PATTERN_PROFILE_SELECT_MASK);
			uint8_t pattern_g2 = (uint8_t)(pattern_sel_g2 & PATTERN_PROFILE_SELECT_MASK);


			if ((pattern_g1 != pattern_g2) || !IsValidPatternProfile(pattern_g1)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			selected_profile_response = pattern_g1;

			uartResp->data = &selected_profile_response;

			// if (!ExtractUnifiedProfile(DELAY_PROFILE_SELECT_REGISTER, pattern_sel_g1, pattern_sel_g2, &selected_profile_response)) {
			// 	uartResp->packet_type = OW_ERROR;
			// 	return;
			// }

			// uartResp->data = &selected_profile_response;
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
			if (!IsValidDelayProfile(profile)) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// Set the delay profile on-chip
			TX7332_WriteReg(&transmitters[cmd->addr], DELAY_PROFILE_SELECT_REGISTER, profile);

			// Commit selector changes on-chip (self-clearing LOAD_PROF bit).
			TX7332_LoadProfile(&transmitters[cmd->addr]);

			// Apply the corresponding apodization for the selected delay profile.
			apply_profile_apodization(cmd->addr, profile);
			break;
		}

		case OW_CTRL_GET_DELAY_PROFILE:
		{
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 1;

			if (cmd->addr >= get_tx_chip_count()) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint32_t pattern_sel = TX7332_ReadReg(&transmitters[cmd->addr], DELAY_PROFILE_SELECT_REGISTER);
			uint8_t profile = (uint8_t)(pattern_sel & PATTERN_PROFILE_SELECT_MASK);
			selected_profile_response = profile;
			uartResp->data = &selected_profile_response;
			break;
		}


		////////////////////////////////////////////////////////////////////////////////////////
		////////////////////////////////////////////////////////////////////////////////////////
		////////////////////////////////////////////////////////////////////////////////////////




		case OW_CTRL_SET_PROFILE_CYCLE:
		{
			/**
			 * Grouped Profile Cycle Command Handler
			 *
			 * Receives execution_order list and apodization data from host.
			 * Stores this configuration in MCU RAM for runtime profile cycling.
			 *
			 * Packet Format:
			 *   [profile_count:1] [apod_channels:1] [exec_order_len:1]
			 *   [execution_order[0..exec_order_len-1]:N bytes]
			 *   [apod_profile_1[0..63]:64 bytes] ... [apod_profile_N[0..63]:64 bytes]
			 */
			uartResp->command = cmd->command;
			uartResp->addr = cmd->addr;
			uartResp->reserved = cmd->reserved;
			uartResp->data_len = 0;

			// Minimum payload: 3 bytes header
			if (cmd->data_len < 3U) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			uint8_t *payload = (uint8_t *)cmd->data;
			uint8_t n_profiles = payload[0];
			uint8_t n_apod_channels = payload[1];
			uint8_t exec_order_len = payload[2];

			// Validate ranges
			if (n_profiles < 1 || n_profiles > 16 || 
				n_apod_channels != 64 || 
				exec_order_len < 1 || exec_order_len > 16) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// Expected payload size: 3 (header) + exec_order_len + (n_profiles * 64)
			uint16_t expected_size = 3U + (uint16_t)exec_order_len + ((uint16_t)n_profiles * 64U);
			if (cmd->data_len < expected_size) {
				uartResp->packet_type = OW_ERROR;
				return;
			}

			// ========== STORE CONFIGURATION ==========
			profile_cycle.profile_count = n_profiles;
			profile_cycle.apod_channels = n_apod_channels;
			profile_cycle.exec_order_len = exec_order_len;
			profile_cycle.current_exec_index = 0;

			// Extract execution_order indices (1-based)
			uint8_t *exec_order_ptr = &payload[3];
			for (uint8_t i = 0; i < exec_order_len; i++) {
				uint8_t profile_idx = exec_order_ptr[i];
				if (profile_idx < 1 || profile_idx > n_profiles) {
					uartResp->packet_type = OW_ERROR;
					return;
				}
				profile_cycle.execution_order[i] = profile_idx;
			}

			// Extract apodization data per profile
			uint8_t *apod_data_ptr = &payload[3 + exec_order_len];
			for (uint8_t p = 0; p < n_profiles; p++) {
				memcpy(apodization_table[p],
					   &apod_data_ptr[p * NUM_CHANNELS],
					   NUM_CHANNELS);
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
			// Keep apodization synchronized when host switches profiles by writing 0x16 directly.
			if (reg_address == DELAY_PROFILE_SELECT_REGISTER) {
				uint8_t selected_profile =
					(uint8_t)(((reg_value >> BF_PROF_SEL_G1_SHIFT) & BF_PROF_SEL_FIELD_MASK) + 1U);
				if (IsValidPatternProfile(selected_profile)) {
					apply_profile_apodization(cmd->addr, selected_profile);
				}
			}
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
			tx_profile_status.delay_profile_count = tx_profile_cache[cmd->addr].delay_profile_count;
			tx_profile_status.pattern_profile_count = tx_profile_cache[cmd->addr].pattern_profile_count;
			tx_profile_status.delay_profiles_mask = tx_profile_cache[cmd->addr].delay_profiles_mask;
			tx_profile_status.pattern_profiles_mask = tx_profile_cache[cmd->addr].pattern_profiles_mask;
			uartResp->data_len = (uint16_t)sizeof(tx_profile_status);
			uartResp->data = (uint8_t *)&tx_profile_status;
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
