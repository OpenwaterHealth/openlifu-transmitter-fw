/*
 * if_commands.h
 *
 *  Created on: Nov 15, 2024
 *      Author: GeorgeVigelette
 */

#ifndef INC_IF_COMMANDS_H_
#define INC_IF_COMMANDS_H_


#include "common.h"
#include "utils.h"
#include <stdbool.h>

#define REG_DATA_LEN 62

#define HW_ID_DATA_LENGTH 12
#define TEMPERATURE_DATA_LENGTH 4

#define HEADER_SIZE 11

bool process_if_command(UartPacket *cmd, UartPacket *resp);

// Profile auto-cycle hooks, called from the trigger ISRs in trigger.c.
bool apply_next_profile_in_cycle(void);
void reset_profile_cycle_to_start(void);

// FDA mode (presets.c): install an execution order that was computed at build
// time, instead of the one the host used to push with OW_CTRL_SET_PROFILE_CYCLE.
bool profile_cycle_install(uint8_t profile_count, const uint8_t *order, uint8_t len);

// FDA mode (presets.c): record which profiles a baked register block covers, so
// OW_TX7332_STATUS reports the same profile masks a host-driven write would.
void profile_cache_note_write(uint8_t tx_idx, uint16_t start_addr, uint16_t reg_count);

#endif /* INC_IF_COMMANDS_H_ */
