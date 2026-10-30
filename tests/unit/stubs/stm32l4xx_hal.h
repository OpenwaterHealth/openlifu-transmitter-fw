/* Stand-in for the STM32 HAL header, so Core/Src/presets.c compiles on the
 * host: only the names Core/Inc/main.h and the headers presets.c includes
 * refer to. The test provides the functions presets.c calls. */
#ifndef STM32L4XX_HAL_H
#define STM32L4XX_HAL_H

#include <stddef.h>
#include <stdint.h>

#define __weak __attribute__((weak))

typedef struct GPIO_TypeDef GPIO_TypeDef;
typedef struct ADC_HandleTypeDef ADC_HandleTypeDef;
typedef struct CRC_HandleTypeDef CRC_HandleTypeDef;
typedef struct I2C_HandleTypeDef I2C_HandleTypeDef;
typedef struct LPTIM_HandleTypeDef LPTIM_HandleTypeDef;
typedef struct SPI_HandleTypeDef SPI_HandleTypeDef;
typedef struct TIM_HandleTypeDef TIM_HandleTypeDef;
typedef struct UART_HandleTypeDef UART_HandleTypeDef;

uint32_t HAL_GetTick(void);

#endif /* STM32L4XX_HAL_H */
