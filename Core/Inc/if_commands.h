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

// TX7332 stored pattern and delay profile limit (this is a self-imposed limit, the chip supports up to 32 pattern profiles)
#define MAX_NUMBER_OF_PROFILES          16

// Pattern profile select fields (unfortunately) live in different registers.
#define PATTERN_PROFILE_SELECT_REG_G1  0x1F
#define PATTERN_PROFILE_SELECT_REG_G2  0x1E
#define PATTERN_PROFILE_SELECT_MASK    0x3F // 6-bit mask, only bits 0-3 used due to 16 profile limit #TODO: replace with 0x0F?

// Delay profile select fields live in single register.
#define DELAY_PROFILE_SELECT_REGISTER  0x16
#define BF_PROF_SEL_G1_SHIFT           28 // decimal bitshift (bits 28-31 used for profile selection)
#define BF_PROF_SEL_G2_SHIFT           12 // decimal bitshift (bits 12-15 used for profile selection)
#define BF_PROF_SEL_FIELD_MASK         0x0F

bool process_if_command(UartPacket *cmd, UartPacket *resp);

#endif /* INC_IF_COMMANDS_H_ */
