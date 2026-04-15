#include "main.h"
#include "tx7332.h"
#include <stdio.h>
#include <stdbool.h>

#define COMMS_TIMEOUT 250

static SPI_HandleTypeDef* spi_ = &hspi1;

static const int READ_DIE1 = (1 << 1);
static const int READ_DIE2 = (1 << 2);
static const int LOAD_PROF = (1 << 3);
static const int BURST_WR_EN = (1 << 8);

static const uint32_t SwapEndian(uint32_t val) {
	val = (val >> 24) |
		  ((val >> 8) & 0xFF00) |
		  ((val << 8) & 0xFF0000) |
		  ((val << 24));
	return val;
}

static uint32_t SE(uint32_t val) {
  val = (val >> 24) |
		((val >> 8) & 0xFF00) |
		((val << 8) & 0xFF0000) |
		((val << 24));
  return val;
}

// Private function implementation
static void WriteAddr(uint16_t addr) {
    GPIO_InitTypeDef init = {0};
    init.Pin = SPI_SCK_Pin | SPI_MOSI_Pin;
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &init);

    for (int i = 0; i < 10; ++i) {
        HAL_GPIO_WritePin(SPI_SCK_GPIO_Port, SPI_SCK_Pin, GPIO_PIN_RESET);
        if (addr & 0x200) {
            HAL_GPIO_WritePin(SPI_MOSI_GPIO_Port, SPI_MOSI_Pin, GPIO_PIN_SET);
        } else {
            HAL_GPIO_WritePin(SPI_MOSI_GPIO_Port, SPI_MOSI_Pin, GPIO_PIN_RESET);
        }
        HAL_GPIO_WritePin(SPI_SCK_GPIO_Port, SPI_SCK_Pin, GPIO_PIN_SET);
        addr <<= 1;
    }
    HAL_GPIO_WritePin(SPI_SCK_GPIO_Port, SPI_SCK_Pin, GPIO_PIN_RESET);

    init.Pin = SPI_SCK_Pin|SPI_MISO_Pin|SPI_MOSI_Pin;
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    init.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &init);
}


void TX7332_Init(TX7332* device, GPIO_TypeDef* cs_port, uint16_t cs_pin) {
    device->cs_port = cs_port;
    device->cs_pin = cs_pin;
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_SET);
}

void TX7332_Reset(void) {
    HAL_GPIO_WritePin(TX_STDBY_GPIO_Port, TX_STDBY_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TX_RESET_L_GPIO_Port, TX_RESET_L_Pin, GPIO_PIN_SET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(TX_RESET_L_GPIO_Port, TX_RESET_L_Pin, GPIO_PIN_RESET);
    HAL_Delay(5);
}

void TX7332_WriteReg(TX7332* device, uint16_t addr, uint32_t val) {
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_RESET);
    WriteAddr(addr);
    val = SwapEndian(val);
    HAL_SPI_Transmit(spi_, (uint8_t*)&val, 4, COMMS_TIMEOUT);
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_SET);
}

uint32_t TX7332_ReadReg(TX7332* device, uint16_t addr) {
    uint32_t read[2];

    // Read chip 0
    TX7332_WriteReg(device, 0, READ_DIE1);
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_RESET);
    WriteAddr(addr);
    HAL_SPI_Receive(spi_, (uint8_t*)&read[0], 4, COMMS_TIMEOUT);
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_SET);

    // Read chip 1
    TX7332_WriteReg(device, 0, READ_DIE2);
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_RESET);
    WriteAddr(addr);
    HAL_SPI_Receive(spi_, (uint8_t*)&read[1], 4, COMMS_TIMEOUT);
    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_SET);

    // Restore the original state
    TX7332_WriteReg(device, 0, 0);

    // Combine the read values and return
    read[0] = read[0] | read[1];
    return SwapEndian(read[0]);
}

bool TX7332_WriteVerify(TX7332* device, uint16_t addr, uint32_t val){
	TX7332_WriteReg(device, addr, val);
	return TX7332_ReadReg(device, addr) == val;
}

bool TX7332_WriteBulk(TX7332* device, uint16_t addr, uint32_t* pInts, int len) {
	HAL_StatusTypeDef status = HAL_OK;

    // Validate parameters
    if (device == NULL || pInts == NULL) {
        return false;
    }
    if (len <= 0) {
        return false;
    }

    if (len > 1) {
        TX7332_WriteReg(device, 0, BURST_WR_EN);
    }

    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_RESET);
    WriteAddr(addr);

    for (int i = 0; i < len; ++i) {
    	uint32_t val = *(pInts + i);
        uint32_t swap_val = SwapEndian(val);
        status = HAL_SPI_Transmit(spi_, (uint8_t*)&swap_val, 4, COMMS_TIMEOUT);
        if(status != HAL_OK){
        	break;
        }
    }

    HAL_GPIO_WritePin(device->cs_port, device->cs_pin, GPIO_PIN_SET);
    // Check final status
    if (status == HAL_OK) {
        return true;
    } else {
        return false;
    }
}

bool TX7332_WriteBulkVerify(TX7332* device, uint16_t addr, uint32_t* be_bytes, int len)
{
    // Write the data in bulk
    if(!TX7332_WriteBulk(device, addr, be_bytes, len))
    {
    	return false;
    }

    // Verify each written value
    for (int i = 0; i < len; ++i) {
        uint32_t expectedValue = SE(be_bytes[i]); // Convert to the expected format
        uint32_t readValue = TX7332_ReadReg(device, addr + i);

        if (readValue != expectedValue) {
            // Optional: Print a message if a mismatch occurs
            // printf("WriteVerify Error at %X: Expected %lX, Read %lX\n", addr + i, expectedValue, readValue);
            return false; // Return false if any value does not match
        }
    }

    return true; // Return true if all values match
}

void TX7332_SetRepeat(TX7332* device, int count) {
    // Check for count validity
    if (count > 32) {
        // printf("Repeat count %d is greater than max 32\n", count);
        return;
    }

    // Read the current value of the register (assuming 0x19 is the register address)
    uint32_t reg = TX7332_ReadReg(device, 0x19);

    // Clear the relevant bits and set the new count
    // Assuming the count is stored in bits 1-5 (modify as per your device's specification)
    reg &= ~(((1 << 5) - 1) << 1); // Clear the bits
    reg |= (count << 1); // Set new count

    // Write the modified value back to the register
    TX7332_WriteReg(device, 0x19, reg);
}

void TX7332_LoadProfile(TX7332* device) {
    TX7332_WriteReg(device, 0, LOAD_PROF);
}

