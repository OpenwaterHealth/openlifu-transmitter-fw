/*
 * common.h
 *
 *  Created on: Apr 3, 2024
 *      Author: gvigelet
 */

#ifndef INC_COMMON_H_
#define INC_COMMON_H_

#include "version.h"

#include <stdint.h>

#define COMMAND_MAX_SIZE 2080
#define ONEWIRE_MAX_SIZE 48
#define DATA_MAX_SIZE 2048

// Configuration: each module has two transmitters; adjust as necessary.
#define TX_PER_MODULE 2
#define MAX_MODULES   6  // Total number of modules (master + slaves)

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif
#ifndef FW_SHA
#define FW_SHA "unknown"
#endif
#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

#define FW_VERSION_STRING FW_VERSION
#define FW_SHA_STRING FW_SHA
#define FW_BUILD_TIME_STRING FW_BUILD_TIME


typedef enum {
	OW_START_BYTE = 0xAA,
	OW_END_BYTE = 0xDD,
} USTX_ProtocolTypes;

typedef enum {
    ROLE_UNDEFINED,
    ROLE_MASTER,
    ROLE_SLAVE
} DEVICE_ROLE;

typedef enum {
	OW_ACK = 0xE0,
	OW_NAK = 0xE1,
	OW_CMD = 0xE2,
	OW_RESP = 0xE3,
	OW_DATA = 0xE4,
	OW_ONE_WIRE = 0xE5,
	OW_TX7332 = 0xE6,
	OW_AFE_READ = 0xE7,
	OW_AFE_SEND = 0xE8,
	OW_I2C_PASSTHRU = 0xE9,
	OW_CONTROLLER = 0xEA,
	OW_POWER = 0xEB,
	OW_ONEWIRE_RESP = 0xEC,
	OW_TIMEOUT = 0xEE,
	OW_ERROR = 0xEF,
} UartPacketTypes;

typedef enum {
	OW_SUCCESS = 0x00,
	OW_UNKNOWN_COMMAND = 0xFC,
	OW_BAD_CRC = 0xFD,
	OW_INVALID_PACKET = 0xFE,
	OW_UNKNOWN_ERROR = 0xFF,
} UstxErrorCodes;

typedef enum {
	OW_CMD_PING = 0x00,
	OW_CMD_PONG = 0x01,
	OW_CMD_VERSION = 0x02,
	OW_CMD_ECHO = 0x03,
	OW_CMD_TOGGLE_LED = 0x04,
	OW_CMD_HWID = 0x05,
	OW_CMD_GET_TEMP = 0x06,
	OW_CMD_GET_AMBIENT = 0x07,
	OW_CMD_ASYNC = 0x09,
	OW_CMD_USR_CFG = 0x0A,
	OW_CMD_DISCOVERY = 0x0C,
	OW_CMD_DFU = 0x0D,
	OW_CMD_NOP = 0x0E,
	OW_CMD_RESET = 0x0F,
} UstxGlobalCommands;

typedef enum {
	OW_CTRL_SCAN_I2C = 0x10,
	OW_CTRL_WRITE_I2C = 0x11,
	OW_CTRL_READ_I2C = 0x12,
	OW_CTRL_SET_SWTRIG = 0x13,
	OW_CTRL_GET_SWTRIG = 0x14,
	OW_CTRL_START_SWTRIG = 0x15,
	OW_CTRL_STOP_SWTRIG = 0x16,
	OW_CTRL_STATUS_SWTRIG = 0x17,
	OW_CTRL_SET_HV = 0x18,
	OW_CTRL_GET_HV = 0x19,
	OW_CTRL_GET_MODULE_COUNT = 0x1A,
} UstxControllerCommands;

typedef enum {
	OW_TX7332_STATUS = 0x20,
	OW_TX7332_ENUM = 0x21,
	OW_TX7332_WREG = 0x22,
	OW_TX7332_RREG = 0x23,
	OW_TX7332_WBLOCK = 0x24,
	OW_TX7332_VWREG = 0x25,
	OW_TX7332_VWBLOCK = 0x26,
	OW_TX7332_RBLOCK = 0x27,
	OW_TX7332_DEVICE_COUNT = 0x2C,
	OW_TX7332_DEMO = 0x2D,
	OW_TX7332_RESET = 0x2F,
} UstxTX7332Commands;

typedef enum {
	OW_AFE_STATUS = 0x30,
	OW_AFE_ENUM_TX7332 = 0x31,
} UstxAfeCommands;

typedef struct  {
	uint16_t id;
	uint8_t packet_type;
	uint8_t command;
	uint8_t addr;
	uint8_t reserved;
	uint16_t data_len;
	uint8_t* data;
	uint16_t crc;
} UartPacket;

#endif /* INC_COMMON_H_ */
