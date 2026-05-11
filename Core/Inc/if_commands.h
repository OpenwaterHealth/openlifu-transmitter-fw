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
#define PROFILE_SELECT_MASK 0x0000000F
#define MAX_NUMBER_OF_PROFILES 16
#define PROFILE_SELECT_REGISTER_G1 0x1E
#define PROFILE_SELECT_REGISTER_G2 0x1F

bool process_if_command(UartPacket *cmd, UartPacket *resp);

#endif /* INC_IF_COMMANDS_H_ */
