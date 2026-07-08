#ifndef __TRIGGER_H
#define __TRIGGER_H

#include "stm32l4xx_hal.h"
#include <stdbool.h>

typedef enum {
    TRIGGER_MODE_SEQUENCE = 0,
    TRIGGER_MODE_CONTINUOUS = 1,
    TRIGGER_MODE_SINGLE = 2
} TriggerSequenceMode;

typedef enum {
    TRIGGER_STATUS_READY = 0,
	TRIGGER_STATUS_RUNNING = 1,
	TRIGGER_STATUS_ERROR = 2,
	TRIGGER_STATUS_NOT_CONFIGURED = 3
} TriggerStatus;

typedef enum {
    TRIGGER_STATE_READY = 0,
	TRIGGER_STATE_PULSE = 1,
	TRIGGER_STATE_PULSE_INTERVAL = 2,
	TRIGGER_STATE_TRAIN_INTERVAL = 3
} TriggerState;

typedef enum {
	AUTO_CYCLE_IDLE = 0,
	AUTO_CYCLE_RUNNING = 1,
	AUTO_CYCLE_PENDING_APPLY = 2,
	AUTO_CYCLE_ERROR = 3
} AutoCycleState_e;

typedef struct {
    uint32_t TriggerFrequencyHz;
    uint32_t TriggerPulseWidthUsec;
    uint32_t TriggerPulseCount;
    uint32_t TriggerPulseTrainInterval;  // in microseconds
    uint32_t TriggerPulseTrainCount;
    uint32_t TriggerMode;
    uint32_t ProfileIndex;
    uint32_t ProfileIncrement;
    uint32_t TriggerState;
    uint32_t TriggerStatus;
} OW_TimerData;

typedef struct {
	AutoCycleState_e state;              // Current state of auto-cycle state machine
	bool is_active;                      // Is auto-cycle enabled for current trigger sequence
	uint32_t cycles_completed;           // Number of complete profile cycles executed
	uint32_t cycles_remaining;           // Number of cycles left to run (decrements each completion)
	uint8_t current_profile;             // Profile executed in current/last cycle
	uint8_t next_profile;                // Profile to apply next
	volatile bool apply_pending;         // Flag set by callback, serviced by main loop
} AutoCycleContext_t;

extern volatile uint8_t _running;

// Function prototypes
void deinit_trigger(void);
void init_trigger_pulse(OW_TimerData _timerDataConfig);
uint8_t get_trigger_status(void);
uint8_t start_trigger_pulse(void);
uint8_t stop_trigger_pulse(void);
bool get_trigger_data(char *jsonString, size_t max_length);
bool set_trigger_data(const char *jsonString, size_t str_len);
uint8_t get_trigger_mode(void);
const char* get_trigger_mode_str(void);

void TRIG_TIM2_IRQHandler(void);
void TRIG_TIM1_IRQHandler(void);
void print_OW_TimerData(const OW_TimerData *data);

// ========== AUTO-CYCLE API ==========
// Start auto-cycle mode for a trigger sequence (called when cycle list is configured)
void auto_cycle_start(uint32_t total_cycles);

// Stop auto-cycle mode (called on stop or error)
void auto_cycle_stop(void);

// Service routine - called from main loop to handle deferred profile apply
void auto_cycle_service(void);

// Check if auto-cycle is currently active
bool auto_cycle_is_active(void);

// Get current auto-cycle state
AutoCycleState_e auto_cycle_get_state(void);

// Get cycles remaining
uint32_t auto_cycle_get_remaining_cycles(void);

// Request next profile apply (called from trigger callbacks)
void auto_cycle_request_profile_apply(void);

// Weak callback functions
__weak void pulse_complete_callback(uint32_t curr_count, uint32_t total_count);
__weak void pulsetrain_complete_callback(uint32_t curr_count, uint32_t total_count);
__weak void sequence_complete_callback(uint32_t total_count) ;

#endif /* __TRIGGER_H */
