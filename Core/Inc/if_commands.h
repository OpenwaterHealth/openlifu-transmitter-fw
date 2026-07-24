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

#endif /* INC_IF_COMMANDS_H_ */
