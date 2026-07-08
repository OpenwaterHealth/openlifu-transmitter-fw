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

#ifdef __cplusplus
}
#endif

#endif /* TI7332_H */

