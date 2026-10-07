/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "main.h"
#include "logging.h"
#include "lwrb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

static bool bInit_dma = false;
volatile bool bPrintfTransferComplete = false;

static uint8_t usart_start_tx_dma_transfer(void);

/* Ring buffer for TX data - Must be in DMA-accessible memory (not DTCM) */
lwrb_t usart_tx_buff;
uint8_t usart_tx_buff_data[1024]={0};
volatile size_t usart_tx_dma_current_len;


#ifdef __GNUC__
	/* With GCC/RAISONANCE, small printf (option LD Linker->Libraries->Small printf
		 set to 'Yes') calls __io_putchar() */
	#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
	#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif /* __GNUC__ */


#ifdef __GNUC__
int _write(int fd, const void *buf, size_t count){
	UNUSED(fd);
	uint8_t * src = (uint8_t *)buf;
	if(bInit_dma)
	{
	    if (lwrb_get_free(&usart_tx_buff) >= count) {
	        lwrb_write(&usart_tx_buff, buf, count);
	        usart_start_tx_dma_transfer();
	    }
	}
	else
	{
		HAL_StatusTypeDef com_tx_status = HAL_UART_Transmit(&DEBUG_UART, src, count, 10);
		if(com_tx_status != HAL_OK)
		{
			Error_Handler();
		}
	}

	return count;
}
#else
PUTCHAR_PROTOTYPE
{
  /* Place your implementation of fputc here */
	//HAL_UART_Transmit(&DEBUG_UART, (uint8_t *)&ch, 1,10);
	UART_putChar( (uint8_t) ch);
  return ch;
}
#endif

void init_dma_logging()
{
    /* Initialize ringbuff */
    lwrb_init(&usart_tx_buff, usart_tx_buff_data, sizeof(usart_tx_buff_data));

    bInit_dma = true;
	bPrintfTransferComplete = true;
}

bool is_using_dma(){
	return bInit_dma;
}

static uint8_t usart_start_tx_dma_transfer(void) {
    if (usart_tx_dma_current_len == 0 && (usart_tx_dma_current_len = lwrb_get_linear_block_read_length(&usart_tx_buff)) > 0) {

        /* Limit maximal size to transmit at a time */
        if (usart_tx_dma_current_len > 32) {
            usart_tx_dma_current_len = 32;
        }
    	bPrintfTransferComplete = false;

	#ifdef DEBUG_HALF_DUPLEX
		// transmit
		DEBUG_UART.Instance->CR1 |= USART_CR1_TE;
		DEBUG_UART.Instance->CR1 &= ~USART_CR1_RE;
	#endif

		if(HAL_UART_Transmit_DMA(&DEBUG_UART, (uint8_t*)lwrb_get_linear_block_read_address(&usart_tx_buff), usart_tx_dma_current_len)!= HAL_OK)
		{
			Error_Handler();
		}
    }
    return 1;
}

void logging_UART_TxHalfCpltCallback(UART_HandleTypeDef *huart)
{

}


void logging_UART_ErrorCallback(UART_HandleTypeDef *huart)
{

}


void logging_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	bPrintfTransferComplete = true;
    lwrb_skip(&usart_tx_buff, usart_tx_dma_current_len);/* Data sent, ignore these */
    usart_tx_dma_current_len = 0;
    usart_start_tx_dma_transfer();          /* Try to send more data */
}
