/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * thermistor.c
 *
 *  Created on: Dec 16, 2024
 *      Author: GeorgeVigelette
 */
#include "thermistor.h"
#include "i2c_master.h"
#include <math.h>

// Private variables
static ADC_HandleTypeDef *adcHandle = NULL;  // Pointer to the ADC handle
static float referenceVoltage;        // ADC reference voltage
static float pullUpResistance;        // Pull-up resistance value

volatile float tx_temperature = 0.0f;
volatile float ambient_temperature = 0.0f;

// Initialize the thermistor
void Thermistor_Start(ADC_HandleTypeDef *hadc, float vRef, float rPullUp)
{
    adcHandle = hadc;
    referenceVoltage = vRef;
    pullUpResistance = rPullUp;

    __HAL_ADC_CLEAR_FLAG(adcHandle, ADC_FLAG_OVR | ADC_FLAG_EOC | ADC_FLAG_EOS);

    // Start the ADC in continuous mode
    HAL_ADC_Start(adcHandle);

    // throw away first sample; ADCs often have a "warm-up" code
    (void)HAL_ADC_PollForConversion(adcHandle, 5);
    (void)HAL_ADC_GetValue(adcHandle);

}

void Thermistor_Stop()
{
    if(adcHandle)
    {
    	HAL_ADC_Stop(adcHandle);
    	adcHandle = NULL;
    }
}

// Read thermistor resistance
static float Thermistor_GetResistance(void)
{

    // With AutoWait ENABLE, ADC won’t start the next conversion until DR is read.
    // With Overrun=OVERWRITTEN, you’ll never get “stuck” on old data.
    if (HAL_ADC_PollForConversion(adcHandle, 5) != HAL_OK) {
        // If you ever suspect OVR, clear it:
        __HAL_ADC_CLEAR_FLAG(adcHandle, ADC_FLAG_OVR);
        return false;
    }

    // Get ADC value
    uint16_t adcValue = HAL_ADC_GetValue(adcHandle);

    // Calculate voltage measured
    float vADC = (referenceVoltage / 4095.0f) * adcValue;

    // Calculate thermistor resistance
    return pullUpResistance * (vADC/(referenceVoltage - vADC));
}

// Read temperature in Celsius
float Thermistor_ReadTemperature(void)
{
    if(adcHandle)
    {
		float resistance = Thermistor_GetResistance();

		// Use the Beta equation to calculate temperature
		float invT = (1.0f / T0) + (log(resistance / R0) / BETA);
		return (1.0f / invT) - 273.15f;  // Convert Kelvin to Celsius
    }

    return 0;
}
