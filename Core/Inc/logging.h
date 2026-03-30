/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __LOGGING_H
#define __LOGGING_H

#ifdef __cplusplus
 extern "C" {
#endif

#include <main.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

 void init_dma_logging();
 bool is_using_dma();
 void logging_UART_TxCpltCallback(UART_HandleTypeDef *huart);
 void logging_UART_TxHalfCpltCallback(UART_HandleTypeDef *huart);
 void logging_UART_ErrorCallback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* __LOGGING_H*/
