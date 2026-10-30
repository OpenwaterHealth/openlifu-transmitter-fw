#include "trigger.h"
#include "main.h"
#include "if_commands.h"
#include "module_manager.h"
#include "usb_events.h"

#include "jsmn.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// Internal state variables
static volatile uint32_t _pulseCount = 0;
static volatile uint32_t _trainCount = 0;

// What TRIG_ONESHOT_IRQHandler does once the pulse in flight ends.
#define END_NONE     0
#define END_SEQUENCE 1
#define END_PAUSE    2
static volatile uint8_t _end_action = END_NONE;

// Pause/resume. While paused _trainCount is the trains delivered; the gap the
// last one owed is taken from the pause before the next train may start.
static volatile bool _pause_pending = false;
static volatile bool _resume_wait = false;   // HIRES is running out that gap
static volatile uint32_t _gap_elapsed_us = 0;  // of the last train's period, at the pause
static volatile uint32_t _paused_tick = 0;

// Treatment timeout: START begins the clock, and pauses do not hold it.
static volatile uint32_t _run_start_tick = 0;
static uint32_t _timeout_ms = 0;               // 0: none

static volatile OW_TimerData _timerDataConfig = {
		.TriggerFrequencyHz = 0,
		.TriggerPulseWidthUsec = 0,
		.TriggerPulseCount = 0,
		.TriggerMode = TRIGGER_MODE_SEQUENCE,
		.TriggerPulseTrainCount = 0,
		.TriggerPulseTrainInterval = 0,
		.ProfileIndex = 0,
		.ProfileIncrement = 0,
		.TriggerStatus = TRIGGER_STATUS_NOT_CONFIGURED
};

// Auto-cycle state for pulse-level profile switching
static AutoCycleContext_t _auto_cycle = {
	.state = AUTO_CYCLE_IDLE,
	.is_active = false,
	.pulses_per_profile = 0,
	.pulse_counter_in_profile = 0,
};

// Trigger-slave state (slave modules only, see the block near the bottom).
typedef struct {
	bool armed;                    // TIM15 is watching the shared trigger line
	bool cycling;                  // still advancing the execution order
	uint32_t pulses_per_profile;
	uint32_t pulse_count;          // pulses per train, 0 = no train boundary
	uint32_t train_count;          // trains per sequence, 0 = continuous
	uint32_t pulse_in_train;
	uint32_t trains_done;
	uint32_t pulse_in_profile;
} TriggerSlave_t;

static volatile TriggerSlave_t _slave = {0};

static void slave_dead_time_tick(void);


static int jsoneq(const char *json, const jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
	   strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
	 return 0;
  }
  return -1;
}

static void timerDataToJson(char *jsonString, size_t max_length)
{
	 memset(jsonString, 0, max_length);
	 snprintf(jsonString, max_length,
			  "{"
			  "\"TriggerFrequencyHz\": %lu,"
			  "\"TriggerPulseCount\": %lu,"
			  "\"TriggerPulseWidthUsec\": %lu,"
			  "\"TriggerPulseTrainInterval\": %lu,"
			  "\"TriggerPulseTrainCount\": %lu,"
			  "\"TriggerMode\": %lu,"
			  "\"ProfileIndex\": %lu,"
			  "\"ProfileIncrement\": %lu,"
			  "\"TrainCount\": %lu,"
			  "\"TriggerStatus\": \"%s\""
			  "}",
			  _timerDataConfig.TriggerFrequencyHz,
			  _timerDataConfig.TriggerPulseCount,
			  _timerDataConfig.TriggerPulseWidthUsec,
			  _timerDataConfig.TriggerPulseTrainInterval,
			  _timerDataConfig.TriggerPulseTrainCount,
			  _timerDataConfig.TriggerMode,
			  _timerDataConfig.ProfileIndex,
			  _timerDataConfig.ProfileIncrement,
			  (unsigned long)_trainCount,
			  _timerDataConfig.TriggerStatus == TRIGGER_STATUS_RUNNING ? "RUNNING" :
			  _timerDataConfig.TriggerStatus == TRIGGER_STATUS_PAUSED ? "PAUSED" :
			  _timerDataConfig.TriggerStatus == TRIGGER_STATUS_TIMEOUT ? "TIMEOUT" : "STOPPED");
}

static int jsonToTimerData(const char *jsonString)
{
	 int i, r;
	 jsmn_parser parser;
	 jsmntok_t t[32];


	 printf("%s\r\n", jsonString);

	 jsmn_init(&parser, NULL);
	 r = jsmn_parse(&parser, jsonString, strlen(jsonString), t,
				  sizeof(t) / sizeof(t[0]), NULL);

	 if (r < 0) {
		 printf("jsonToTimerData Failed to parse JSON: %d\r\n", r);
		 return 1;
	 }

	 if (r < 1 || t[0].type != JSMN_OBJECT) {
		 printf("jsonToTimerData Object expected\r\n");
		 return 1;
	 }

	 for (i = 1; i < r - 1; i++) {
		 if (jsoneq(jsonString, &t[i], "TriggerFrequencyHz") == 0) {
			 _timerDataConfig.TriggerFrequencyHz = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "TriggerPulseCount") == 0) {
			 _timerDataConfig.TriggerPulseCount = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "TriggerPulseWidthUsec") == 0) {
			 _timerDataConfig.TriggerPulseWidthUsec = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "TriggerPulseTrainInterval") == 0) {
			 _timerDataConfig.TriggerPulseTrainInterval = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "TriggerPulseTrainCount") == 0) {
			 _timerDataConfig.TriggerPulseTrainCount = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "TriggerMode") == 0) {
			 _timerDataConfig.TriggerMode = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "ProfileIndex") == 0) {
			 _timerDataConfig.ProfileIndex = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 } else if (jsoneq(jsonString, &t[i], "ProfileIncrement") == 0) {
			 _timerDataConfig.ProfileIncrement = strtol(jsonString + t[i + 1].start, NULL, 10);
			 i++;
		 }
	 }

	 return 0;
}

static void Configure_TIMERS_Frequency(TIM_HandleTypeDef* htim, uint32_t frequencyHz, bool is32BIT)
{
    uint32_t timer_clk = 48000000;  // 48 MHz source clock
    uint32_t prescaler = 0;
    uint32_t arr = 0;

    // Try to find prescaler and arr such that arr <= 0xFFFF
    for (prescaler = 0; prescaler <= 0xFFFF; prescaler++) {
        uint32_t temp_arr = (timer_clk / (frequencyHz * (prescaler + 1))) - 1;

        if (is32BIT) {
            if (temp_arr <= 0xFFFFFFFF) {
                arr = temp_arr;
                break;
            }
        } else {
            if (temp_arr <= 0xFFFF) {
                arr = temp_arr;
                break;
            }
        }
    }

    // If we reach max prescaler without a valid arr, fallback
    if (prescaler > 0xFFFF) {
        prescaler = 0xFFFF;
        arr = 0xFFFF;
    }

    // Reset and prepare the timer
    __HAL_TIM_DISABLE(htim);
    __HAL_TIM_SET_COUNTER(htim, 0);

    htim->Instance->PSC = prescaler;
    htim->Instance->ARR = arr;

	// Fix for first pulse count sequence being off by 1 when board first powers on. 
	// Force an update event so the new PSC/ARR values take effect immediately.
    htim->Instance->EGR = TIM_EGR_UG;

    // Clear interrupt flags
    __HAL_TIM_CLEAR_FLAG(htim, TIM_FLAG_UPDATE);
}


static void Configure_ONESHOT_Timer(TIM_HandleTypeDef* htim, uint16_t pulsewidth)
{

	  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
	  TIM_SlaveConfigTypeDef sSlaveConfig = {0};
	  TIM_MasterConfigTypeDef sMasterConfig = {0};
	  TIM_OC_InitTypeDef sConfigOC = {0};
	  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

	  __HAL_TIM_DISABLE(htim);  // Stop timer if running

	  htim->Init.Prescaler = 48-1;
	  htim->Init.CounterMode = TIM_COUNTERMODE_UP;
	  htim->Init.Period = (pulsewidth *2) - 1;
	  htim->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	  htim->Init.RepetitionCounter = 0;
	  htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	  if (HAL_TIM_Base_Init(htim) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	  if (HAL_TIM_ConfigClockSource(htim, &sClockSourceConfig) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  if (HAL_TIM_PWM_Init(htim) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  if (HAL_TIM_OnePulse_Init(htim, TIM_OPMODE_SINGLE) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  sSlaveConfig.SlaveMode = TIM_SLAVEMODE_TRIGGER;
	  sSlaveConfig.InputTrigger = TIM_TS_ITR0;
	  if (HAL_TIM_SlaveConfigSynchro(htim, &sSlaveConfig) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
	  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	  if (HAL_TIMEx_MasterConfigSynchronization(htim, &sMasterConfig) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  sConfigOC.OCMode = TIM_OCMODE_PWM1;
	  sConfigOC.Pulse = pulsewidth;
	  sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
	  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_LOW;
	  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
	  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
	  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
	  if (HAL_TIM_PWM_ConfigChannel(&TRIGGER_TIMER, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
	  {
	    Error_Handler();
	  }
	  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
	  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
	  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
	  sBreakDeadTimeConfig.DeadTime = 0;
	  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
	  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
	  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
	  if (HAL_TIMEx_ConfigBreakDeadTime(&TRIGGER_TIMER, &sBreakDeadTimeConfig) != HAL_OK)
	  {
	    Error_Handler();
	  }

	  // Fix for first pulse width being wrong when board first powers on.
	  // Force an update event so the new CCR (pulse width) value takes effect immediately.
	  __HAL_TIM_DISABLE(htim);
	  htim->Instance->EGR = TIM_EGR_UG;
	  __HAL_TIM_CLEAR_FLAG(htim, TIM_FLAG_UPDATE);

	  /* USER CODE BEGIN TIM15_Init 2 */

	  /* USER CODE END TIM15_Init 2 */
	  HAL_TIM_MspPostInit(htim);

}

void print_OW_TimerData(const OW_TimerData *data) {
    printf("TriggerFrequencyHz: %lu\r\n", data->TriggerFrequencyHz);
    printf("TriggerPulseWidthUsec: %lu\r\n", data->TriggerPulseWidthUsec);
    printf("TriggerPulseCount: %lu\r\n", data->TriggerPulseCount);
    printf("TriggerPulseTrainInterval: %lu\r\n", data->TriggerPulseTrainInterval);
    printf("TriggerPulseTrainCount: %lu\r\n", data->TriggerPulseTrainCount);

    switch(data->TriggerMode){
    case TRIGGER_MODE_CONTINUOUS:
        printf("TriggerMode: CONTINUOUS\r\n");
    	break;
    case TRIGGER_MODE_SEQUENCE:
        printf("TriggerMode: SEQUENCE\r\n");
    	break;
    default:
        printf("TriggerMode: SINGLE\r\n");
    	break;
    }

    printf("ProfileIndex: %lu\r\n", data->ProfileIndex);
    printf("ProfileIncrement: %lu\r\n", data->ProfileIncrement);

    switch(data->TriggerStatus){
    case TRIGGER_STATUS_READY:
        printf("TriggerStatus: READY\r\n");
    	break;
    case TRIGGER_STATUS_RUNNING:
        printf("TriggerStatus: RUNNING\r\n");
    	break;
    case TRIGGER_STATUS_ERROR:
        printf("TriggerStatus: ERROR\r\n");
    	break;
    case TRIGGER_STATUS_PAUSED:
        printf("TriggerStatus: PAUSED\r\n");
    	break;
    case TRIGGER_STATUS_TIMEOUT:
        printf("TriggerStatus: TIMEOUT\r\n");
    	break;
    default:
        printf("TriggerStatus: NOT_CONFIGURED\r\n");
    	break;
    }
}


bool get_trigger_data(char *jsonString, size_t max_length)
{
	 timerDataToJson(jsonString, max_length);
	 return true;
}

// How many pulse trains the current config will run. Preset loading preserves
// this across a reconfigure, since run length is the host's call, not the
// preset's.
uint32_t get_trigger_train_count(void)
{
	return _timerDataConfig.TriggerPulseTrainCount;
}

// Apply a trigger configuration directly, without going through the JSON that
// the host would normally send. Loading a preset uses this: the timing belongs
// to the preset, so loading one configures the sequence as well as the chips.
bool trigger_set_config(uint32_t frequency_hz, uint32_t pulse_width_us,
                        uint32_t pulse_count, uint32_t train_interval_us,
                        uint32_t train_count, uint32_t mode)
{
	if (frequency_hz == 0U || pulse_count == 0U || train_count == 0U) return false;
	if (pulse_width_us == 0U) return false;
	if (mode > TRIGGER_MODE_SINGLE) return false;

	if (_timerDataConfig.TriggerStatus == TRIGGER_STATUS_RUNNING) {
		stop_trigger_pulse();
	}

	_timerDataConfig.TriggerFrequencyHz = frequency_hz;
	_timerDataConfig.TriggerPulseWidthUsec = pulse_width_us;
	_timerDataConfig.TriggerPulseCount = pulse_count;
	_timerDataConfig.TriggerPulseTrainInterval = train_interval_us;
	_timerDataConfig.TriggerPulseTrainCount = train_count;
	_timerDataConfig.TriggerMode = mode;
	_timerDataConfig.TriggerStatus = TRIGGER_STATUS_READY;
	return true;
}

bool set_trigger_data(const char *jsonString, size_t str_len)
{
	 uint8_t tempArr[255] = {0};
	 bool ret = false;

	 // Copy the JSON string to tempArr
	 memcpy((char *)tempArr, (char *)jsonString, str_len);
	 if(_timerDataConfig.TriggerStatus == TRIGGER_STATUS_RUNNING){
		 stop_trigger_pulse();
	 }

	 _timerDataConfig.TriggerStatus = TRIGGER_STATUS_READY;

	 if (jsonToTimerData((const char *)tempArr) == 0)
	 {
		 ret = true;
	 }

	 return ret;
}

// Park the trigger output as a push-pull GPIO driven low. HAL_TIM_PWM_Stop
// clears MOE, and with OSSI disabled the TIM15 output is left high-impedance;
// the floating line can then couple from the switching TX stage and
// self-retrigger the TX7332. Driving it low holds the trigger inactive. The
// next start_trigger_pulse() restores AF mode via Configure_ONESHOT_Timer().
static void trigger_pin_park_low(void)
{
	 HAL_GPIO_DeInit(TRIGGER_GPIO_Port, TRIGGER_Pin);

	 GPIO_InitTypeDef GPIO_InitStruct = {0};
	 GPIO_InitStruct.Pin = TRIGGER_Pin;
	 GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	 GPIO_InitStruct.Pull = GPIO_NOPULL;
	 GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	 HAL_GPIO_Init(TRIGGER_GPIO_Port, &GPIO_InitStruct);

	 HAL_GPIO_WritePin(TRIGGER_GPIO_Port, TRIGGER_Pin, GPIO_PIN_RESET);
}

void deinit_trigger(void)
 {
	 /* USER CODE BEGIN TIM15_DeInit 0 */

	 /* USER CODE END TIM15_DeInit 0 */

	 /* 1. Stop the PWM generation on TIM15 Channel 4 */
	 if (HAL_TIM_PWM_Stop(&TRIGGER_TIMER, TIM_CHANNEL_2) != HAL_OK)
	 {
		 Error_Handler();
	 }

	 /* 2. Deinitialize the TIM15 peripheral */
	 if (HAL_TIM_PWM_DeInit(&TRIGGER_TIMER) != HAL_OK)
	 {
		 Error_Handler();
	 }

	 /* 3. Park the trigger pin low (GPIO output) */
	 trigger_pin_park_low();

	 /* USER CODE BEGIN TIM15_DeInit 1 */

	 /* USER CODE END TIM15_DeInit 1 */
 }

void init_trigger_pulse(OW_TimerData new_timerDataConfig) {
    memcpy((void *)&_timerDataConfig, &new_timerDataConfig, sizeof(OW_TimerData));

    Configure_TIMERS_Frequency(&LORES_TIMER, new_timerDataConfig.TriggerFrequencyHz, false);

    _timerDataConfig.TriggerState = TRIGGER_STATE_READY;
    _timerDataConfig.TriggerStatus = TRIGGER_STATUS_READY;

}

const char* get_trigger_mode_str(void) {
    switch (_timerDataConfig.TriggerMode) {
        case TRIGGER_MODE_SEQUENCE: return "SEQUENCE";
        case TRIGGER_MODE_CONTINUOUS: return "CONTINUOUS";
        case TRIGGER_MODE_SINGLE: return "SINGLE";
        default: return "UNKNOWN";
    }
}

uint8_t get_trigger_mode(void)
{
	return (uint8_t)_timerDataConfig.TriggerMode;
}

uint8_t get_trigger_status(void)
{
	return (uint8_t)_timerDataConfig.TriggerStatus;
}

uint32_t get_trigger_pulse_count(void)
{
	return _timerDataConfig.TriggerPulseCount;
}

uint32_t get_trigger_period_us(void)
{
	if (_timerDataConfig.TriggerFrequencyHz == 0) return 0;
	return 1000000UL / _timerDataConfig.TriggerFrequencyHz;
}

// Deferred profile switching.
//
// The trigger edge that raises the LORES_TIMER update interrupt is the same
// hardware event that starts the TX7332 acoustic burst, which keeps sounding
// for the host-programmed pattern duration - far longer than the trigger
// pulse itself. Running the profile-switch SPI writes from the update
// interrupt therefore lands them inside the burst, glitching the output.
// Instead the ISRs only schedule an action here, and a LORES_TIMER compare
// interrupt placed MIN_PROFILE_SWITCH_US before the next trigger edge
// performs the writes in guaranteed dead time.
#define PROFILE_ACTION_NONE   0
#define PROFILE_ACTION_SWITCH 1  // apply the next profile in the execution order
#define PROFILE_ACTION_RESET  2  // restart the execution order from the beginning
static volatile uint8_t _pending_profile_action = PROFILE_ACTION_NONE;

static void schedule_profile_action(uint8_t action)
{
    uint32_t arr = LORES_TIMER.Instance->ARR;
    uint32_t psc = LORES_TIMER.Instance->PSC;
    // Timer ticks in MIN_PROFILE_SWITCH_US at the 48 MHz timer clock.
    uint32_t lead_ticks = (MIN_PROFILE_SWITCH_US * 48) / (psc + 1);
    // Fire the compare just before the next update; if the period is too
    // short to hold the lead time (blocked by start_trigger_pulse validation),
    // fall back to firing as soon as possible.
    uint32_t compare = (lead_ticks < arr) ? (arr - lead_ticks) : 1;

    _pending_profile_action = action;
    __HAL_TIM_SET_COMPARE(&LORES_TIMER, TIM_CHANNEL_1, compare);
    __HAL_TIM_CLEAR_FLAG(&LORES_TIMER, TIM_FLAG_CC1);
    __HAL_TIM_ENABLE_IT(&LORES_TIMER, TIM_IT_CC1);
}

static void cancel_profile_action(void)
{
    __HAL_TIM_DISABLE_IT(&LORES_TIMER, TIM_IT_CC1);
    __HAL_TIM_CLEAR_FLAG(&LORES_TIMER, TIM_FLAG_CC1);
    _pending_profile_action = PROFILE_ACTION_NONE;
}

// LORES_TIMER compare interrupt: runs the scheduled profile action late in
// the trigger period (routed here from TIM1_CC_IRQHandler via the HAL).
// cppcheck-suppress constParameterPointer
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
	// Slave-only: dead-time timed off trigger line (master uses period timer)
    if (htim->Instance == TRIGGER_TIMER.Instance) {
        slave_dead_time_tick();
        return;
    }

    if (htim->Instance != LORES_TIMER.Instance) return;

    uint8_t action = _pending_profile_action;
    cancel_profile_action();

    if (_timerDataConfig.TriggerStatus != TRIGGER_STATUS_RUNNING || !_auto_cycle.is_active) {
        return;
    }

    if (action == PROFILE_ACTION_SWITCH) {
        if (!apply_next_profile_in_cycle()) {
            _auto_cycle.state = AUTO_CYCLE_ERROR;
            _auto_cycle.is_active = false;
        }
    } else if (action == PROFILE_ACTION_RESET) {
        reset_profile_cycle_to_start();
    }
}

// Configures and clears the timers and arms the one-shot; the caller starts
// LORES (the pulses) and HIRES (the train period).
static void arm_trigger_timers(void)
{
    _end_action = END_NONE;
    _pause_pending = false;
    _resume_wait = false;
    __HAL_TIM_DISABLE_IT(&TRIGGER_TIMER, TIM_IT_UPDATE);
    cancel_profile_action();

    Configure_ONESHOT_Timer(&TRIGGER_TIMER, _timerDataConfig.TriggerPulseWidthUsec);
    Configure_TIMERS_Frequency(&LORES_TIMER, _timerDataConfig.TriggerFrequencyHz, false);

    __HAL_TIM_DISABLE(&HIRES_TIMER);
    __HAL_TIM_SET_COUNTER(&HIRES_TIMER, 0);

    HIRES_TIMER.Instance->ARR = _timerDataConfig.TriggerPulseTrainInterval - 1;

    // Clear interrupt flags
    __HAL_TIM_CLEAR_FLAG(&HIRES_TIMER, TIM_FLAG_UPDATE);
    __HAL_TIM_CLEAR_FLAG(&LORES_TIMER, TIM_FLAG_UPDATE);

    // Fix for the one-shot ignoring the first trigger and skipping the first pulse.
    // Force it idle because the LORES update event above may have trigger-started it.
    __HAL_TIM_DISABLE(&TRIGGER_TIMER);
    __HAL_TIM_SET_COUNTER(&TRIGGER_TIMER, 0);
    __HAL_TIM_CLEAR_FLAG(&TRIGGER_TIMER, TIM_FLAG_UPDATE);
    __HAL_TIM_SET_COUNTER(&LORES_TIMER, 0);
    __HAL_TIM_SET_COUNTER(&HIRES_TIMER, 0);

    __HAL_TIM_ENABLE_IT(&LORES_TIMER, TIM_IT_UPDATE);
    __HAL_TIM_ENABLE_IT(&HIRES_TIMER, TIM_IT_UPDATE);

    HAL_TIM_PWM_Start(&TRIGGER_TIMER, TIM_CHANNEL_2);
}

uint8_t start_trigger_pulse(void) {
    if (_timerDataConfig.TriggerStatus != TRIGGER_STATUS_READY) return _timerDataConfig.TriggerStatus;


    // Period from frequency; 0 Hz yields 0 and fails the pulse-width check below.
    uint32_t triggerPeriodUsec = get_trigger_period_us();

    // Validate: Pulse width must be less than the period
    if (_timerDataConfig.TriggerPulseWidthUsec >= triggerPeriodUsec) {
        _timerDataConfig.TriggerStatus = TRIGGER_STATUS_ERROR;
        return TRIGGER_STATUS_ERROR;
    }

    // Validate: If auto-cycle is active, inter-pulse dead time must be sufficient for SPI writes
    if (_auto_cycle.is_active) {
        uint32_t dead_time_us = triggerPeriodUsec - _timerDataConfig.TriggerPulseWidthUsec;
        if (dead_time_us < MIN_PROFILE_SWITCH_US) {
            _timerDataConfig.TriggerStatus = TRIGGER_STATUS_ERROR;
            return TRIGGER_STATUS_ERROR;
        }
    }

    // Validate: Pulse train interval must be 0 or large enough to contain the full pulse train
    if (_timerDataConfig.TriggerPulseTrainInterval > 0 &&
        _timerDataConfig.TriggerPulseTrainInterval < triggerPeriodUsec * _timerDataConfig.TriggerPulseCount) {
        _timerDataConfig.TriggerStatus = TRIGGER_STATUS_ERROR;
        return TRIGGER_STATUS_ERROR;
    }

    _pulseCount = 0;
    _trainCount = 0;
    arm_trigger_timers();

    HAL_TIM_Base_Start_IT(&LORES_TIMER);
    if(_timerDataConfig.TriggerPulseTrainInterval>0) {
        HAL_TIM_Base_Start_IT(&HIRES_TIMER);
    }
    _run_start_tick = HAL_GetTick();
    _timerDataConfig.TriggerStatus = TRIGGER_STATUS_RUNNING;
    return TRIGGER_STATUS_RUNNING;
}

uint8_t stop_trigger_pulse(void) {
    // Disarm before doing anything to avoid race condition - if there 
	// is a forced stop (overheat, STOP command) from the main loop while 
	// end_after_final_pulse() has armed the TRIGGER_TIMER update interrupt, 
	// the ISR could run finish_sequence() mid-stop and report a completed sequence.
    __HAL_TIM_DISABLE_IT(&TRIGGER_TIMER, TIM_IT_UPDATE);
    _end_action = END_NONE;
    _pause_pending = false;
    _resume_wait = false;

	// Paused or timed out: the timers are already stopped and the pin parked.
	if (_timerDataConfig.TriggerStatus == TRIGGER_STATUS_PAUSED ||
	    _timerDataConfig.TriggerStatus == TRIGGER_STATUS_TIMEOUT) {
		_timerDataConfig.TriggerStatus = TRIGGER_STATUS_READY;
		return TRIGGER_STATUS_READY;
	}
	if(_timerDataConfig.TriggerStatus != TRIGGER_STATUS_RUNNING) return _timerDataConfig.TriggerStatus;

    HAL_TIM_PWM_Stop(&TRIGGER_TIMER, TIM_CHANNEL_2);
    HAL_TIM_Base_Stop_IT(&LORES_TIMER);
    HAL_TIM_Base_Stop_IT(&HIRES_TIMER);
    cancel_profile_action();
    // Hold the trigger line low so it can't float and self-retrigger the TX7332.
    trigger_pin_park_low();
    _timerDataConfig.TriggerStatus = TRIGGER_STATUS_READY;
    return TRIGGER_STATUS_READY;
}

/**
 * @brief  USB event callback - stops the trigger when the host link is lost
 *         (SR-015). Runs in USB interrupt context.
 */
static void trigger_usb_event_cb(usb_event_t event)
{
    (void)event;
    stop_trigger_pulse();
}

void trigger_register_interlock(void)
{
    usb_register_callback(USB_EVENT_PORT_CLOSE, trigger_usb_event_cb);
    usb_register_callback(USB_EVENT_DISCONNECT, trigger_usb_event_cb);
}

static void finish_sequence(void)
{
	stop_trigger_pulse(); // disables update interrupt and clears _end_action
	sequence_complete_callback(_timerDataConfig.TriggerPulseTrainCount);
}

static void finish_pause(void)
{
	stop_trigger_pulse();
	// Resume starts it again, from the first profile.
	auto_cycle_stop();
	_paused_tick = HAL_GetTick();
	_timerDataConfig.TriggerStatus = TRIGGER_STATUS_PAUSED;
	sequence_paused_callback(_trainCount, _timerDataConfig.TriggerPulseTrainCount);
}

// The ISRs fire as soon as the last pulse starts. 
// If we stop the PWM there the pulse is truncated and the line is
// left floating. Since the one-shot re-arms itself it's not stopped mid-run
// on purpose. Instead, this arms the end-of-cycle update interrupt and then
// TRIG_ONESHOT_IRQHandler runs `action`. Returns true, with nothing armed, if
// the pulse has already ended: the caller runs it now.
static bool end_after_final_pulse(uint8_t action)
{
	_end_action = action;
	__HAL_TIM_CLEAR_FLAG(&TRIGGER_TIMER, TIM_FLAG_UPDATE);
	__HAL_TIM_ENABLE_IT(&TRIGGER_TIMER, TIM_IT_UPDATE);
	if (TRIGGER_TIMER.Instance->CR1 & TIM_CR1_CEN) return false;
	// It may have ended between the clear and the enable: disarm, so the ISR can't run it too.
	__HAL_TIM_DISABLE_IT(&TRIGGER_TIMER, TIM_IT_UPDATE);
	_end_action = END_NONE;
	return true;
}

static void stop_after_final_pulse(void)
{
	if (end_after_final_pulse(END_SEQUENCE)) finish_sequence();
}

// TRIGGER_TIMER update: only armed by end_after_final_pulse(), so this is the
// final pulse of the sequence, or of the train a pause ends, finishing.
// Routed from HAL_TIM_PeriodElapsedCallback.
void TRIG_ONESHOT_IRQHandler(void)
{
	__HAL_TIM_DISABLE_IT(&TRIGGER_TIMER, TIM_IT_UPDATE);
	uint8_t action = _end_action;
	_end_action = END_NONE;
	if (action == END_SEQUENCE) finish_sequence();
	else if (action == END_PAUSE) finish_pause();
}

static void start_next_train(void)
{
    // Reset pulse-level profile cycling for the new pulse train
    if (_auto_cycle.is_active) {
        _auto_cycle.pulse_counter_in_profile = 0;
        schedule_profile_action(PROFILE_ACTION_RESET);
    }

    _pulseCount = 0;
    __HAL_TIM_ENABLE_IT(&LORES_TIMER, TIM_IT_UPDATE);
    HAL_TIM_Base_Start_IT(&LORES_TIMER);

    if(_timerDataConfig.TriggerPulseTrainInterval>0) {
        __HAL_TIM_ENABLE_IT(&HIRES_TIMER, TIM_IT_UPDATE);
        HAL_TIM_Base_Start_IT(&HIRES_TIMER);
    }
}

// The current train's pulses are out and HIRES is running its gap: end the
// train here. False on the final train, whose gap ends the sequence instead.
static bool pause_in_gap(void)
{
	_pause_pending = false;
	// A resume running out the gap it owes has already counted that train.
	if (!_resume_wait) {
		if (_trainCount + 1U >= _timerDataConfig.TriggerPulseTrainCount) return false;
		_trainCount++;
	}
	_resume_wait = false;
	_gap_elapsed_us = __HAL_TIM_GET_COUNTER(&HIRES_TIMER);
	__HAL_TIM_DISABLE_IT(&HIRES_TIMER, TIM_IT_UPDATE);
	HAL_TIM_Base_Stop_IT(&HIRES_TIMER);
	return true;
}

bool pause_trigger_pulse(void)
{
	if (_timerDataConfig.TriggerStatus == TRIGGER_STATUS_PAUSED) return true;
	if (_timerDataConfig.TriggerStatus != TRIGGER_STATUS_RUNNING ||
	    _timerDataConfig.TriggerMode != TRIGGER_MODE_SEQUENCE) return false;

	bool now = false;
	__disable_irq();
	// LORES idle with a train period set means a gap: nothing of the train is left.
	if (_timerDataConfig.TriggerPulseTrainInterval > 0U &&
	    !(LORES_TIMER.Instance->CR1 & TIM_CR1_CEN) && _end_action == END_NONE) {
		if (pause_in_gap()) now = end_after_final_pulse(END_PAUSE);
	} else {
		_pause_pending = true;
	}
	__enable_irq();

	// With the IRQs back on: the async report waits on the USB interrupt.
	if (now) finish_pause();
	return true;
}

bool resume_trigger_pulse(void)
{
	if (_timerDataConfig.TriggerStatus == TRIGGER_STATUS_RUNNING) {
		__disable_irq();
		bool withdrawn = _pause_pending;
		_pause_pending = false;
		__enable_irq();
		return withdrawn;
	}
	if (_timerDataConfig.TriggerStatus != TRIGGER_STATUS_PAUSED) return false;

	// How much of the last train's period is over. The pause counts one ms
	// short, so the tick's granularity can't start the next train early.
	uint32_t interval = _timerDataConfig.TriggerPulseTrainInterval;
	uint32_t elapsed_us = interval;
	if (interval > 0U) {
		uint32_t paused_ms = HAL_GetTick() - _paused_tick;
		uint32_t paused_us = 0U;
		if (paused_ms > 1U) {
			paused_us = (paused_ms - 1U > interval / 1000U) ? interval : (paused_ms - 1U) * 1000U;
		}
		if (paused_us < interval - _gap_elapsed_us) elapsed_us = _gap_elapsed_us + paused_us;
	}

	_pulseCount = 0;
	arm_trigger_timers();
	// Before HIRES starts: its interrupt checks it.
	_timerDataConfig.TriggerStatus = TRIGGER_STATUS_RUNNING;

	if (elapsed_us < interval) {
		// HIRES runs out the rest of that period, then starts the next train.
		__HAL_TIM_SET_COUNTER(&HIRES_TIMER, elapsed_us);
		_resume_wait = true;
		HAL_TIM_Base_Start_IT(&HIRES_TIMER);
	} else {
		HAL_TIM_Base_Start_IT(&LORES_TIMER);
		if (interval > 0U) {
			HAL_TIM_Base_Start_IT(&HIRES_TIMER);
		}
	}
	return true;
}

void trigger_set_timeout_s(uint32_t timeout_s)
{
	_timeout_ms = timeout_s * 1000U;
}

bool trigger_timeout_expired(void)
{
	uint8_t status = _timerDataConfig.TriggerStatus;
	return _timeout_ms != 0U &&
	       (status == TRIGGER_STATUS_RUNNING || status == TRIGGER_STATUS_PAUSED) &&
	       (HAL_GetTick() - _run_start_tick) >= _timeout_ms;
}

void trigger_end_timeout(void)
{
	// The final train went out before the stop landed: that is a completion.
	if (_timerDataConfig.TriggerStatus != TRIGGER_STATUS_READY ||
	    _trainCount >= _timerDataConfig.TriggerPulseTrainCount) return;
	_timerDataConfig.TriggerStatus = TRIGGER_STATUS_TIMEOUT;
	sequence_timeout_callback(_trainCount, _timerDataConfig.TriggerPulseTrainCount);
}

void TRIG_TIM2_IRQHandler(void) {

	if(_timerDataConfig.TriggerStatus != TRIGGER_STATUS_RUNNING) return;
    __HAL_TIM_DISABLE_IT(&HIRES_TIMER, TIM_IT_UPDATE);
    HAL_TIM_Base_Stop_IT(&HIRES_TIMER);

    // One-shot PWM deliberately not stopped here.

	// A resumed sequence's owed gap is over: no train ended here.
	if (_resume_wait) {
		_resume_wait = false;
		start_next_train();
		return;
	}

	_trainCount++;
    if(_timerDataConfig.TriggerMode == TRIGGER_MODE_SINGLE) {
        stop_after_final_pulse();
        return;
    }else if(_trainCount>=_timerDataConfig.TriggerPulseTrainCount &&  _timerDataConfig.TriggerMode != TRIGGER_MODE_CONTINUOUS) {
        stop_after_final_pulse();
	}else{
	    start_next_train();

	    // Report after restarting timers since in async mode this blocks ~1 ms on USB.
	    pulsetrain_complete_callback(_trainCount, _timerDataConfig.TriggerPulseTrainCount);
	}
}

// Pulse-level profile switching: the trigger edge that raised this interrupt
// also started an acoustic burst that keeps sounding for the TX7332 pattern
// duration, so the SPI writes are deferred to the compare interrupt late in
// this trigger period (see schedule_profile_action).
static void handle_auto_cycle_pulse_switch(void)
{
    if (!_auto_cycle.is_active) return;

    _auto_cycle.pulse_counter_in_profile++;
    if (_auto_cycle.pulse_counter_in_profile < _auto_cycle.pulses_per_profile) return;

    _auto_cycle.pulse_counter_in_profile = 0;
    bool pass_done = (_pulseCount % _timerDataConfig.TriggerPulseCount) == 0U;
    if (_timerDataConfig.TriggerPulseTrainInterval == 0 &&
        _timerDataConfig.TriggerMode == TRIGGER_MODE_CONTINUOUS) {
        // Free-run: restart the order each pass instead of holding the last profile.
        schedule_profile_action(pass_done ? PROFILE_ACTION_RESET : PROFILE_ACTION_SWITCH);
    } else if (!pass_done) {
        schedule_profile_action(PROFILE_ACTION_SWITCH);
    }
}

// Back-to-back train boundary (TriggerPulseTrainInterval == 0). Returns true
// if TRIG_TIM1_IRQHandler should return immediately without reporting the
// pulse that just completed.
static bool handle_back_to_back_train_boundary(void)
{
    _trainCount++;
    if (_timerDataConfig.TriggerMode == TRIGGER_MODE_SINGLE) {
        stop_after_final_pulse();
        return true;
    }

    if (_timerDataConfig.TriggerMode == TRIGGER_MODE_SEQUENCE &&
        _trainCount >= _timerDataConfig.TriggerPulseTrainCount) {
        stop_after_final_pulse();
        return true;
    }

    // No gap to owe: resume can start the next train at once.
    if (_pause_pending) {
        _pause_pending = false;
        if (end_after_final_pulse(END_PAUSE)) finish_pause();
        return true;
    }

    // Back-to-back restart, one-shot PWM deliberately not stopped here.
    _pulseCount = 0;

    // Reset pulse-level profile cycling for the new pulse train
    // (deferred: the final pulse's burst is still sounding here)
    if (_auto_cycle.is_active) {
        _auto_cycle.pulse_counter_in_profile = 0;
        schedule_profile_action(PROFILE_ACTION_RESET);
    }

    __HAL_TIM_ENABLE_IT(&LORES_TIMER, TIM_IT_UPDATE);
    HAL_TIM_Base_Start_IT(&LORES_TIMER);
    pulsetrain_complete_callback(_trainCount, _timerDataConfig.TriggerPulseTrainCount);
    return true;
}

void TRIG_TIM1_IRQHandler(void) {
	if(_timerDataConfig.TriggerStatus != TRIGGER_STATUS_RUNNING) return;

    _pulseCount++;

    handle_auto_cycle_pulse_switch();

	if(_timerDataConfig.TriggerPulseTrainInterval == 0 && _timerDataConfig.TriggerMode == TRIGGER_MODE_CONTINUOUS){
		// Free-run - the raster sequence wrapping is in the auto-cycle block above.
	}
	else if(_pulseCount>=_timerDataConfig.TriggerPulseCount)
    {
        __HAL_TIM_DISABLE_IT(&LORES_TIMER, TIM_IT_UPDATE);
        HAL_TIM_Base_Stop_IT(&LORES_TIMER);
        if(_timerDataConfig.TriggerPulseTrainInterval == 0 && handle_back_to_back_train_boundary()) {
            return;
    	}
        // The train's pulses are out: a pending pause lands now, not after its gap.
        if (_pause_pending && pause_in_gap()) {
            if (end_after_final_pulse(END_PAUSE)) finish_pause();
            return;
        }
    }
    pulse_complete_callback(_pulseCount, _timerDataConfig.TriggerPulseCount);
}

// Split pulse count per number of profiles
bool trigger_pulses_per_profile(uint8_t n_profiles, uint32_t *pulses_per_profile)
{
	uint32_t pulse_count = _timerDataConfig.TriggerPulseCount;

	if (n_profiles == 0U || pulse_count == 0U || (pulse_count % n_profiles) != 0U) {
		return false;
	}

	*pulses_per_profile = pulse_count / n_profiles;
	return true;
}

void auto_cycle_start(uint32_t pulses_per_profile)
{
	_auto_cycle.state = AUTO_CYCLE_RUNNING;
	_auto_cycle.is_active = true;
	_auto_cycle.pulses_per_profile = pulses_per_profile;
	_auto_cycle.pulse_counter_in_profile = 0;
}

void auto_cycle_stop(void)
{
	_auto_cycle.state = AUTO_CYCLE_IDLE;
	_auto_cycle.is_active = false;
	_auto_cycle.pulse_counter_in_profile = 0;
}

bool auto_cycle_is_active(void)
{
	return _auto_cycle.is_active;
}

AutoCycleState_e auto_cycle_get_state(void)
{
	return _auto_cycle.state;
}

void auto_cycle_reset_pulse_counter(void)
{
	_auto_cycle.pulse_counter_in_profile = 0;
}

// Slaves advance their own execution order off the master's shared trigger edge.
// TRIGGER_TIMER (idle on slaves) watches the trigger pin - each edge resets the
// counter, CC1 fires MIN_PROFILE_SWITCH_US before the next edge to switch, and
// an ARR update (two silent periods) means the trigger stopped.
static void slave_park_trigger_pin(void)
{
	GPIO_InitTypeDef gpio = {0};

	HAL_GPIO_DeInit(TRIGGER_GPIO_Port, TRIGGER_Pin);
	gpio.Pin = TRIGGER_Pin;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(TRIGGER_GPIO_Port, &gpio);
}

bool trigger_slave_arm(uint32_t pulses_per_profile)
{
	TIM_IC_InitTypeDef ic = {0};
	TIM_SlaveConfigTypeDef slave = {0};
	GPIO_InitTypeDef gpio = {0};

	// Every module runs the same trigger config.
	uint32_t period_us = get_trigger_period_us();
	uint32_t pulse_count = _timerDataConfig.TriggerPulseCount;
	uint32_t train_count;

	if (get_device_role() == ROLE_MASTER) return false;
	if (pulses_per_profile == 0U || period_us <= MIN_PROFILE_SWITCH_US) return false;

	switch (_timerDataConfig.TriggerMode) {
	case TRIGGER_MODE_SINGLE:
		train_count = 1;
		break;
	case TRIGGER_MODE_CONTINUOUS:
		train_count = 0;
		break;
	default:
		train_count = _timerDataConfig.TriggerPulseTrainCount;
		break;
	}

	trigger_slave_disarm();

	// Stretch the tick until two trigger periods fit in TIM15's 16-bit ARR
	uint32_t tick_us = 1U;
	while (((2U * period_us) / tick_us) > 0xFFFFU) tick_us++;

	uint32_t compare = (period_us - MIN_PROFILE_SWITCH_US) / tick_us;
	if (compare == 0U) compare = 1U;

	// TRIGGER_Pin is a high-Z input on slaves until now; hand it to TIM15.
	__HAL_RCC_GPIOB_CLK_ENABLE();
	gpio.Pin = TRIGGER_Pin;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	gpio.Alternate = GPIO_AF14_TIM15;
	HAL_GPIO_Init(TRIGGER_GPIO_Port, &gpio);

	__HAL_TIM_DISABLE(&TRIGGER_TIMER);
	TRIGGER_TIMER.Init.Prescaler = (48U * tick_us) - 1U;
	TRIGGER_TIMER.Init.CounterMode = TIM_COUNTERMODE_UP;
	TRIGGER_TIMER.Init.Period = (2U * period_us) / tick_us;
	TRIGGER_TIMER.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	TRIGGER_TIMER.Init.RepetitionCounter = 0;
	TRIGGER_TIMER.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_IC_Init(&TRIGGER_TIMER) != HAL_OK) goto fail;

	ic.ICPolarity = TIM_ICPOLARITY_RISING;
	ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
	ic.ICPrescaler = TIM_ICPSC_DIV1;
	ic.ICFilter = 4;  // shared net between boards; filter ringing on the edge
	if (HAL_TIM_IC_ConfigChannel(&TRIGGER_TIMER, &ic, TIM_CHANNEL_2) != HAL_OK) goto fail;

	slave.SlaveMode = TIM_SLAVEMODE_COMBINED_RESETTRIGGER;
	slave.InputTrigger = TIM_TS_TI2FP2;
	slave.TriggerPolarity = TIM_TRIGGERPOLARITY_RISING;
	slave.TriggerFilter = 4;
	if (HAL_TIM_SlaveConfigSynchro(&TRIGGER_TIMER, &slave) != HAL_OK) goto fail;

	// Clear the one-pulse mode left by MX_TIM15_Init; a slave keeps counting between edges.
	TRIGGER_TIMER.Instance->CR1 &= ~TIM_CR1_OPM;
	// Update interrupt on overflow only, not on the per-edge slave-mode reset.
	__HAL_TIM_URS_ENABLE(&TRIGGER_TIMER);

	_slave.armed = true;
	_slave.cycling = true;
	_slave.pulses_per_profile = pulses_per_profile;
	_slave.pulse_count = pulse_count;
	_slave.train_count = train_count;
	_slave.pulse_in_train = 0;
	_slave.trains_done = 0;
	_slave.pulse_in_profile = 0;

	__HAL_TIM_SET_COUNTER(&TRIGGER_TIMER, 0);
	__HAL_TIM_SET_COMPARE(&TRIGGER_TIMER, TIM_CHANNEL_1, compare);
	__HAL_TIM_CLEAR_FLAG(&TRIGGER_TIMER, TIM_FLAG_CC1 | TIM_FLAG_UPDATE);
	__HAL_TIM_ENABLE_IT(&TRIGGER_TIMER, TIM_IT_CC1);
	// Counter stays stopped until the first trigger edge starts it, so nothing
	// ticks between arming and the master starting.
	if (HAL_TIM_Base_Start_IT(&TRIGGER_TIMER) != HAL_OK) goto fail;

	return true;

fail:
	trigger_slave_disarm();
	return false;
}

void trigger_slave_disarm(void)
{
	if (get_device_role() == ROLE_MASTER) return;

	__HAL_TIM_DISABLE_IT(&TRIGGER_TIMER, TIM_IT_CC1 | TIM_IT_UPDATE);
	HAL_TIM_Base_Stop_IT(&TRIGGER_TIMER);
	TRIGGER_TIMER.Instance->CR1 &= ~TIM_CR1_CEN;
	__HAL_TIM_CLEAR_FLAG(&TRIGGER_TIMER, TIM_FLAG_CC1 | TIM_FLAG_UPDATE);

	_slave.armed = false;
	_slave.cycling = false;
	_slave.pulse_in_train = 0;
	_slave.trains_done = 0;
	_slave.pulse_in_profile = 0;

	slave_park_trigger_pin();
}

bool trigger_slave_is_armed(void)
{
	return _slave.armed;
}

// CC1: just before the next trigger edge, after the current burst ends.
// Mirrors the pulse accounting in TRIG_TIM1_IRQHandler.
static void slave_dead_time_tick(void)
{
	if (!_slave.armed || !_slave.cycling) return;

	_slave.pulse_in_train++;

	if (_slave.pulse_count != 0U && _slave.pulse_in_train >= _slave.pulse_count) {
		_slave.pulse_in_train = 0;
		_slave.pulse_in_profile = 0;
		_slave.trains_done++;
		if (_slave.train_count != 0U && _slave.trains_done >= _slave.train_count) {
			_slave.cycling = false;
			return;
		}
		reset_profile_cycle_to_start();
		return;
	}

	_slave.pulse_in_profile++;
	if (_slave.pulse_in_profile >= _slave.pulses_per_profile) {
		_slave.pulse_in_profile = 0;
		if (!apply_next_profile_in_cycle()) {
			_slave.cycling = false;
		}
	}
}

// Update: two trigger periods with no edge, so the train ended or the master
// stopped. Restart the execution order and wait for the next edge.
void TRIG_TIM15_IRQHandler(void)
{
	// Not armed means this is the master's one-shot update, not ours.
	if (!_slave.armed) {
		TRIG_ONESHOT_IRQHandler();
		return;
	}

	// Direct CEN clear: __HAL_TIM_DISABLE is a no-op while any CCxE bit is set.
	TRIGGER_TIMER.Instance->CR1 &= ~TIM_CR1_CEN;
	__HAL_TIM_SET_COUNTER(&TRIGGER_TIMER, 0);

	_slave.pulse_in_train = 0;
	_slave.pulse_in_profile = 0;
	if (_slave.cycling) {
		reset_profile_cycle_to_start();
	}
}