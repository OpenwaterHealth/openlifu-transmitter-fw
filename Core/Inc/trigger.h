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

// enum for auto-cycling during rastered focusing
typedef enum {
	AUTO_CYCLE_IDLE = 0,
	AUTO_CYCLE_RUNNING = 1,
	AUTO_CYCLE_ERROR = 2
} AutoCycleState_e;

// 1ms window reserved for allowing the profile switch to happen
// in-between trigger pulses. Measured SPI write for all apods + profiles
// is ~500us so setting double for margin.
// #TODO: switch to write + successful read and then apply the switch
// rather than default fixed time.  
#define MIN_PROFILE_SWITCH_US 1000

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
	uint32_t pulses_per_profile;         // Number of consecutive pulses per profile slot
	uint32_t pulse_counter_in_profile;   // Counts pulses within current profile slot
} AutoCycleContext_t;

extern volatile uint8_t _running;

// Function prototypes
void deinit_trigger(void);
void init_trigger_pulse(OW_TimerData _timerDataConfig);
uint8_t get_trigger_status(void);
uint32_t get_trigger_pulse_count(void);
uint8_t start_trigger_pulse(void);
uint8_t stop_trigger_pulse(void);
bool get_trigger_data(char *jsonString, size_t max_length);
bool set_trigger_data(const char *jsonString, size_t str_len);
uint8_t get_trigger_mode(void);
const char* get_trigger_mode_str(void);

void TRIG_TIM2_IRQHandler(void);
void TRIG_TIM1_IRQHandler(void);
void TRIG_ONESHOT_IRQHandler(void);
void print_OW_TimerData(const OW_TimerData *data);

// Auto-cycle API: pulse-level profile switching driven from the trigger ISRs.
void auto_cycle_start(uint32_t pulses_per_profile);
void auto_cycle_stop(void);
bool auto_cycle_is_active(void);
AutoCycleState_e auto_cycle_get_state(void);
void auto_cycle_reset_pulse_counter(void);

// Weak callback functions
__weak void pulse_complete_callback(uint32_t curr_count, uint32_t total_count);
__weak void pulsetrain_complete_callback(uint32_t curr_count, uint32_t total_count);
__weak void sequence_complete_callback(uint32_t total_count) ;

#endif /* __TRIGGER_H */
