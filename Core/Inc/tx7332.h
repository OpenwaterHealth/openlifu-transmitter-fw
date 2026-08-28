#ifndef TI7332_H
#define TI7332_H

#include "main.h"
#include "common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TX7332 {
    GPIO_TypeDef* cs_port;
    uint16_t cs_pin;
} TX7332;

// Self-imposed stored-profile limit; the chip itself supports up to 32 pattern profiles.
#define MAX_PROFILES 16

// Maximum execution-order (profile-cycle) length. Distinct from MAX_PROFILES:
// the order may repeat the <=16 profiles, so its length is bounded only by the
// 1-byte exec_order_len wire field, not the profile count.
#define MAX_EXECUTION_ORDER 255

#define TX7332_APODIZATION_REGISTER 0x1B

#define TX7332_PATTERN_MODE_REGISTER   0x18
#define TX7332_PATTERN_MODE_READ_MASK  0x0FFFFFFF // upper 4 bits should be ignored

// Pattern profile select fields live in two registers (one per channel group).
#define PATTERN_PROFILE_SELECT_REG_G1  0x1F
#define PATTERN_PROFILE_SELECT_REG_G2  0x1E
#define PATTERN_PROFILE_SELECT_MASK    0x3F  // 6-bit selector field; values stay 0-15 due to MAX_PROFILES

// Delay profile select fields for both channel groups share one register.
#define DELAY_PROFILE_SELECT_REGISTER  0x16
#define BF_PROF_SEL_G1_SHIFT           28  // bits 28-31 select the group-1 delay profile
#define BF_PROF_SEL_G2_SHIFT           12  // bits 12-15 select the group-2 delay profile
#define BF_PROF_SEL_FIELD_MASK         0x0F

// Delay profile RAM starts at 0x20 (16 regs/profile), pattern RAM starts at 0x120 (4 regs/profile).
#define TX7332_DELAY_DATA_START        0x20
#define TX7332_DELAY_DATA_END          0x11F
#define TX7332_PATTERN_DATA_START      0x120
#define TX7332_PATTERN_DATA_END        0x19F
#define TX7332_DELAY_PROFILE_OFFSET    16
#define TX7332_PATTERN_PROFILE_OFFSET  4

// Pre-computed apodization register values from SDK, one uint32 per chip per profile.
extern uint32_t apod_registers[MAX_PROFILES][TX_PER_MODULE];
extern uint32_t active_apod_registers[TX_PER_MODULE];


// Function prototypes
void TX7332_Init(TX7332* device, GPIO_TypeDef* cs_port, uint16_t cs_pin);
void TX7332_Reset(void);
void TX7332_WriteReg(TX7332* device, uint16_t addr, uint32_t val);
bool TX7332_WriteVerify(TX7332* device, uint16_t addr, uint32_t val);
uint32_t TX7332_ReadReg(TX7332* device, uint16_t addr);

bool TX7332_WriteBulk(TX7332* device, uint16_t addr, uint32_t* pInts, int len);
bool TX7332_WriteBulkVerify(TX7332* device, uint16_t addr, uint32_t* be_bytes, int len);

void TX7332_SetRepeat(TX7332* device, int count);
void TX7332_LoadProfile(TX7332* device);
void TX7332_SetActiveDelayProfile(uint8_t profile, TX7332* device, uint8_t chip_index);
bool TX7332_GetActiveDelayProfile(TX7332* device, uint8_t* profile);
void TX7332_ResetApodizations(void);

#ifdef __cplusplus
}
#endif

#endif /* TI7332_H */

