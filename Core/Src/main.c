/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "i2c_master.h"
#include "config_CDCE6214_10MHZ_ZDM.h"

#include "tx7332.h"
#include "usbd_cdc_if.h"
#include "uart_comms.h"
#include "module_manager.h"
#include "lifu_config.h"
#include "trigger.h"
#include "i2c_slave.h"
#include "thermistor.h"

#ifdef DEBUG_ENABLED
#include "logging.h"
#endif

#include "utils.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TOGGLE_INTERVAL 500       // Toggle every 500ms
#define TEMPERATURE_INTERVAL 1000 // Toggle every 1000ms
#define DEBOUNCE_DELAY_MS 10

#define TX_OVERHEAT_TRIP_POINT   75.0f  // °C — trip threshold
#define TX_OVERHEAT_HYSTERESIS    5.0f  // °C — must not exceed TX_OVERHEAT_TRIP_POINT
_Static_assert(TX_OVERHEAT_HYSTERESIS <= TX_OVERHEAT_TRIP_POINT,
               "TX_OVERHEAT_HYSTERESIS must not exceed TX_OVERHEAT_TRIP_POINT");

#define BL_BKP_SIGNATURE (0x4F57424CU)     /* 'OWBL' */
#define BL_BKP_REQ_DFU_MAGIC (0xB007C0DEU) /* "BOOT CODE" — SBSFU bootloader DFU request, RTC->BKP7R */

/* STM32L4 system-memory (ROM) bootloader entry point */
#define STM32_SYS_BL_ADDR   (0x1FFF0000U)
/* Our custom bootloader occupies 0x08000000..0x0800FFFF (62 KB) */
#define CUSTOM_BL_START     (0x08000000U)
#define CUSTOM_BL_END       (0x08010000U)

/**
 * @brief Return true if our custom bootloader is programmed.
 *        Inspects the reset-handler vector stored at 0x08000004; if it
 *        falls inside the custom BL flash region a BL is present.
 */
static bool is_custom_bootloader_present(void)
{
  uint32_t reset_handler = *(volatile uint32_t *)(CUSTOM_BL_START + 4U);
  return (reset_handler >= CUSTOM_BL_START && reset_handler < CUSTOM_BL_END);
}

static void bl_bkp_enable(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();

  if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U)
  {
    if ((RCC->BDCR & RCC_BDCR_RTCSEL) == 0U)
    {
      /* RTCSEL=10b -> LSI (RCC_BDCR_RTCSEL_1) */
      MODIFY_REG(RCC->BDCR, RCC_BDCR_RTCSEL, RCC_BDCR_RTCSEL_1);
    }
    SET_BIT(RCC->BDCR, RCC_BDCR_RTCEN);
  }
}

void bootloader_mark_boot_ok(void)
{
  bl_bkp_enable();
  RTC->BKP0R = BL_BKP_SIGNATURE;
  RTC->BKP2R = 0U; /* clears in-progress/force bits + failure count */
  RTC->BKP3R = 0U; /* clears last-bad-fw marker */
  /* SBSFU secure bootloader failsafe boot counter: the bootloader increments
   * BKP6R before every launch and falls back to DFU after 3 attempts that
   * were not confirmed. Clearing it here confirms this boot succeeded. */
  RTC->BKP6R = 0U;
  __DSB();
  __ISB();
}

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

CRC_HandleTypeDef hcrc;

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

IWDG_HandleTypeDef hiwdg;

LPTIM_HandleTypeDef hlptim1;
LPTIM_HandleTypeDef hlptim2;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim7;
TIM_HandleTypeDef htim15;
TIM_HandleTypeDef htim16;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;
DMA_HandleTypeDef hdma_usart3_rx;
DMA_HandleTypeDef hdma_usart3_tx;

/* USER CODE BEGIN PV */

uint32_t id_words[3] = {0};

// Define the pointers
I2C_HandleTypeDef *GLOBAL_I2C_DEVICE = NULL;
I2C_HandleTypeDef *LOCAL_I2C_DEVICE = NULL;

volatile bool _enter_dfu = false;
volatile bool _force_stm32_dfu = false; // for testing purposes, forces to enter STM32 system bootloader instead of custom DFU mode
volatile bool _usb_interrupt_flag = false;
volatile bool tx_overheat_flag = false;
TX7332 transmitters[TX_PER_MODULE];

static lifu_cfg_t *cfg;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_CRC_Init(void);
static void MX_I2C1_Init(void);
static void MX_I2C2_Init(void);
static void MX_RTC_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM7_Init(void);
static void MX_TIM15_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_LPTIM1_Init(void);
static void MX_LPTIM2_Init(void);
static void MX_TIM16_Init(void);
static void MX_IWDG_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void SetPinsHighImpedance(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  // De-initialize the REF_SEL pin
  HAL_GPIO_DeInit(REFSEL_GPIO_Port, REFSEL_Pin);

  // Configure REF_SEL pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = REFSEL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(REFSEL_GPIO_Port, &GPIO_InitStruct);

  // De-initialize the HW_SW_CTRL pin
  HAL_GPIO_DeInit(HW_SW_CTRL_GPIO_Port, HW_SW_CTRL_Pin);

  // Configure HW_SW_CTRL pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = HW_SW_CTRL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(HW_SW_CTRL_GPIO_Port, &GPIO_InitStruct);

  // De-initialize the TRIGGER pin
  HAL_GPIO_DeInit(TRIGGER_GPIO_Port, TRIGGER_Pin);

  // Configure TRIGGER pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = TRIGGER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TRIGGER_GPIO_Port, &GPIO_InitStruct);

  // De-initialize the the 2MHz reference signal pin
  HAL_GPIO_DeInit(REF_CLK_GPIO_Port, REF_CLK_Pin);

  // Configure 2MHz pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = REF_CLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(REF_CLK_GPIO_Port, &GPIO_InitStruct);

  // De-initialize the INT pin
  HAL_GPIO_DeInit(INT_GPIO_Port, INT_Pin);

  // Configure INT pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(INT_GPIO_Port, &GPIO_InitStruct);

  // De-initialize the ESTOP pin
  HAL_GPIO_DeInit(EXT_GPIO_Port, EXT_Pin);

  // Configure INT pin to high impedance (input mode, no pull-up, no pull-down)
  GPIO_InitStruct.Pin = EXT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(EXT_GPIO_Port, &GPIO_InitStruct);
}

static void Setup_Reference_Clock()
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_MSI, RCC_MCODIV_2);
  HAL_RCCEx_EnableMSIPLLMode();

  HAL_Delay(1);

  GPIO_InitStruct.Pin = REF_CLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(REF_CLK_GPIO_Port, &GPIO_InitStruct);
}

bool AreAllSlavesReady(void)
{
  return HAL_GPIO_ReadPin(RST_GPIO_Port, RST_Pin) == GPIO_PIN_SET;
}

void WaitForAllSlavesReady(void)
{
  while (!AreAllSlavesReady())
  {
    FW_DEBUG("Waiting for all slaves to be ready...\r\n");
    HAL_Delay(100); // Delay for stability
  }

  FW_DEBUG("All slaves are ready!\r\n");
}

void SetSlaveReadyState(bool ready)
{
  if (ready)
  {
    HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_SET); // Hi-Z (Ready)
  }
  else
  {
    HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_RESET); // Drive LOW (Not Ready)
  }
}

void ConfigureResetPin(bool master)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  // De-initialize the REF_SEL pin
  HAL_GPIO_DeInit(RST_GPIO_Port, RST_Pin);

  if (master)
  {
    // Configure PA9 as INPUT with Pull-up
    GPIO_InitStruct.Pin = RST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(RST_GPIO_Port, &GPIO_InitStruct);
  }
  else
  {
    // Configure PA9 as open-drain output (Hi-Z when ready)
    GPIO_InitStruct.Pin = RST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_NOPULL; // No internal pull-up
    HAL_GPIO_Init(RST_GPIO_Port, &GPIO_InitStruct);
    SetSlaveReadyState(false);
  }
}

void ConfigureHIzPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  // De-initialize the REF_SEL pin
  HAL_GPIO_DeInit(GPIOx, GPIO_Pin);

  // Configure as input with no pull-up/down (Hi-Z)
  GPIO_InitStruct.Pin = GPIO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOx, &GPIO_InitStruct);
}

static bool ConfigureClock()
{
  int count = 0;

  // reset
  HAL_GPIO_WritePin(PDN_GPIO_Port, PDN_Pin, GPIO_PIN_RESET);
  HAL_Delay(250);
  HAL_GPIO_WritePin(PDN_GPIO_Port, PDN_Pin, GPIO_PIN_SET);
  HAL_Delay(25);
  // I2C_write_CDCE6214_reg(0x67, 0x000F, 0x5020); // unlock eeprom
  HAL_Delay(25);

  // printf("Configuring Clock chip\r\n");
  // Calculate the number of elements in the array
  // blur6214_64mhz_values
  size_t num_elements = sizeof(cdce6214_v6_10mhz_values) / sizeof(uint32_t);

  // Iterate through the array and split each uint32_t value into two uint16_t values
  for (size_t i = 0; i < num_elements; i++)
  {
    uint32_t value = cdce6214_v6_10mhz_values[i];

    // Split the value into upper and lower words
    uint16_t reg_addr = (uint16_t)(value >> 16); // Upper word is reg_addr
    uint16_t reg_value = (uint16_t)value;        // Lower word is reg_value

    // Print the split values
    if (!I2C_write_CDCE6214_reg(0x67, reg_addr, reg_value))
    {
      // printf("failed Index %zu: reg_addr = 0x%04X, reg_value = 0x%04X\r\n", i, reg_addr, reg_value);
      return false;
    }
    HAL_Delay(1);
  }

  I2C_write_CDCE6214_reg(0x67, 0x0000, 0x1130); // calibrate
  HAL_Delay(1);
  I2C_write_CDCE6214_reg(0x67, 0x0000, 0x1120); // calibrate

  for (count = 0; count < 10; count++)
  { // check for lock
    HAL_Delay(50);
    uint16_t v = I2C_read_CDCE6214_reg(0x67, 0x0007);
    if ((v & 0x01) == 0x01)
    {
      HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_RESET);
      break;
    }
  }

  return true;
}

static void Detect_MAX31875_Bus(void)
{
  uint8_t dummy = 0;

  // Try hi2c1 first
  if (HAL_I2C_Master_Transmit(&hi2c1, MAX31875_ADDRESS << 1, &dummy, 0, 100) == HAL_OK ||
      HAL_I2C_IsDeviceReady(&hi2c1, MAX31875_ADDRESS << 1, 1, 100) == HAL_OK)
  {
    LOCAL_I2C_DEVICE = &hi2c1;
    GLOBAL_I2C_DEVICE = &hi2c2;
  }
  else if (HAL_I2C_Master_Transmit(&hi2c2, MAX31875_ADDRESS << 1, &dummy, 0, 100) == HAL_OK ||
           HAL_I2C_IsDeviceReady(&hi2c2, MAX31875_ADDRESS << 1, 1, 100) == HAL_OK)
  {
    LOCAL_I2C_DEVICE = &hi2c2;
    GLOBAL_I2C_DEVICE = &hi2c1;
  }
  else
  {
    // Could not detect MAX31875 on either bus
    Error_Handler();
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  uint32_t last_led_toggle_time = HAL_GetTick(); // Store the initial time
  uint32_t last_temp_toggle_time = HAL_GetTick();
  uint32_t current_time;

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  MX_CRC_Init();
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_RTC_Init();
  MX_SPI1_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM7_Init();
  MX_TIM15_Init();
  MX_USART1_UART_Init();
  MX_LPTIM1_Init();
  MX_LPTIM2_Init();
  MX_TIM16_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
  {
    /* Refresh Error */
    Error_Handler();
  }
  HAL_TIM_Base_Start_IT(&htim16);
#ifdef DEBUG_ENABLED
  init_dma_logging();
#endif
  bootloader_mark_boot_ok();
  printf("\033c");
  printf("LIFU Transmitter Firmware\r\n");
  printf("VER: %s (%s)\r\n", FW_VERSION_STRING, FW_SHA_STRING);
  printf("Date: %s\r\n", FW_BUILD_TIME_STRING);

  FW_DEBUG("Initializing peripherals\r\n");
  SetPinsHighImpedance();
  FW_DEBUG("Pins set to high impedance\r\n");

  // setup default
  deinit_trigger();

  cfg = (lifu_cfg_t *)lifu_cfg_get();
  FW_DEBUG("LIFU config loaded\r\n");

  HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_SET);

  HAL_Delay(250); // wait for role to be set if connected to usb

  // Initialize thermistor library
  Thermistor_Start(&hadc1, 2.5f, 10000.0f);
  FW_DEBUG("Thermistor started\r\n");
  HAL_Delay(5);

  // I2C_scan();
  Detect_MAX31875_Bus();
  FW_DEBUG("MAX31875 bus detected\r\n");
  HAL_Delay(5);

  // Initializing TX7332
  HAL_GPIO_WritePin(GPIOC, TX_RESET_L_Pin | TX_CW_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TX_STDBY_GPIO_Port, TX_STDBY_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR1_EN_GPIO_Port, TR1_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR2_EN_GPIO_Port, TR2_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR3_EN_GPIO_Port, TR3_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR4_EN_GPIO_Port, TR4_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR5_EN_GPIO_Port, TR5_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR6_EN_GPIO_Port, TR6_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR7_EN_GPIO_Port, TR7_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR8_EN_GPIO_Port, TR8_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, TX1_CS_Pin | TX2_CS_Pin, GPIO_PIN_RESET); // TODO: Verify initial state

  // reset TX7332
  TX7332_Reset();
  FW_DEBUG("TX7332 reset complete\r\n");
  HAL_Delay(25);

  // configure CS for TX7332
  TX7332_Init(&transmitters[0], TX1_CS_GPIO_Port, TX1_CS_Pin);
  TX7332_Init(&transmitters[1], TX2_CS_GPIO_Port, TX2_CS_Pin);
  FW_DEBUG("TX7332 initialized (2 tx chips)\r\n");
  HAL_Delay(50);

  TX7332_ResetApodizations();

  HAL_GPIO_WritePin(TX_CW_EN_GPIO_Port, TX_CW_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TR1_EN_GPIO_Port, TR1_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR2_EN_GPIO_Port, TR2_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR3_EN_GPIO_Port, TR3_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR4_EN_GPIO_Port, TR4_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR5_EN_GPIO_Port, TR5_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR6_EN_GPIO_Port, TR6_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR7_EN_GPIO_Port, TR7_EN_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TR8_EN_GPIO_Port, TR8_EN_Pin, GPIO_PIN_SET);
  HAL_Delay(50);
  MX_USB_DEVICE_Init();

  HAL_Delay(500);

  // system entering ready state
  HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_RESET);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    current_time = HAL_GetTick(); // Get current time

    if (!get_configured())
    {
      // start listen
      if (get_device_role() == ROLE_MASTER)
      {
        FW_DEBUG("Role: MASTER - starting configuration\r\n");
        OW_TimerData timerDataConfig;

        ConfigureResetPin(true);
        configure_master();

        timerDataConfig.TriggerFrequencyHz = 10;
        timerDataConfig.TriggerPulseWidthUsec = 2000;
        timerDataConfig.TriggerPulseCount = 5; // no pulse count
        timerDataConfig.TriggerPulseTrainCount = 2;
        timerDataConfig.TriggerPulseTrainInterval = 0;
        timerDataConfig.TriggerMode = TRIGGER_MODE_CONTINUOUS; // TRIGGER_MODE_SEQUENCE TRIGGER_MODE_CONTINUOUS TRIGGER_MODE_SINGLE
        timerDataConfig.ProfileIncrement = 0;
        timerDataConfig.ProfileIndex = 0;
        init_trigger_pulse(timerDataConfig);

        // 2MHz REF CLK
        Setup_Reference_Clock();
        // reset clock chip
        HAL_GPIO_WritePin(PDN_GPIO_Port, PDN_Pin, GPIO_PIN_SET);
        HAL_Delay(10);

        // clock chip setup
        ConfigureClock();
        FW_DEBUG("Master clock configured\r\n");
      }
      else
      {
        FW_DEBUG("Role: SLAVE — starting configuration\r\n");
        ConfigureHIzPin(TRIGGER_GPIO_Port, TRIGGER_Pin);
        ConfigureResetPin(false);
        configure_slave();
        SetSlaveReadyState(true);
        FW_DEBUG("Slave ready state set\r\n");
      }

      HAL_Delay(100);

      if (get_device_role() == ROLE_MASTER)
      {
        WaitForAllSlavesReady();
        FW_DEBUG("All slaves ready\r\n");
        // Phase 2: the discovery walk is idempotent — a slave already configured
        // for a given module_id simply re-confirms it (keeping its I2C address),
        // while fresh nodes (including bootloader-mode nodes) claim a new address.
        // So a master-only reboot with slaves left configured re-enumerates cleanly
        // with no teardown/re-arm race, and no stale addresses to clear first.
        enumerate_slaves();
        FW_DEBUG("Slaves enumerated\r\n");
        set_configured(true);
        HAL_Delay(1);
        I2C_scan_global();
        FW_DEBUG("Starting host comms\r\n");
        comms_host_start();
      }
      else
      {
        while (!get_configured() && !_usb_interrupt_flag)
        {
          comms_onewire_check_received();
          uint8_t my_slave_address = get_slave_addres();
          if (my_slave_address >= 0x20)
          {
            HAL_GPIO_WritePin(PDN_GPIO_Port, PDN_Pin, GPIO_PIN_SET);
            I2C_Slave_Init(my_slave_address);
            HAL_Delay(5);
            ConfigureClock();
          }
          HAL_Delay(1);
        }
        if (_usb_interrupt_flag)
        {
          _usb_interrupt_flag = false;
        }
      }
    }
    else
    {
      if (get_device_role() == ROLE_MASTER)
      {
        comms_host_check_received(); // check comms
      }
      else
      {
        comms_onewire_check_received();
        I2C_Process();
        // Self-heal: if we are enumerated but our I2C slave peripheral got disabled
        // OR is listening on the wrong own-address (a BERR from a bus wedge de-inits
        // it and can lose OwnAddress1), re-init it at our assigned address so the
        // master's forwarded reads work once it has recovered the bus. No-op while
        // healthy (PE set and OA1 == our address).
        if (get_configured() && get_slave_addres() >= 0x20)
        {
          uint32_t pe   = GLOBAL_I2C_DEVICE->Instance->CR1 & I2C_CR1_PE;
          uint8_t  oa1  = (uint8_t)((GLOBAL_I2C_DEVICE->Instance->OAR1 >> 1) & 0x7FU);
          if (pe == 0U || oa1 != get_slave_addres())
          {
            I2C_Slave_Init(get_slave_addres());
          }
        }
      }
    }

    if ((current_time - last_led_toggle_time) >= TOGGLE_INTERVAL)
    {
      HAL_GPIO_TogglePin(LD_HB_GPIO_Port, LD_HB_Pin);
      last_led_toggle_time = current_time; // Update the last toggle time
    }

    if ((current_time - last_temp_toggle_time) >= TEMPERATURE_INTERVAL)
    {
      float new_tx_temp = Thermistor_ReadTemperature();
      ambient_temperature = MAX31875_ReadTemperature();
      last_temp_toggle_time = current_time; // Update the last toggle time

      /* Reject invalid readings (NaN or out-of-physical-range); retain last valid state */
      if (!isnan(new_tx_temp) && (new_tx_temp > -50.0f) && (new_tx_temp < 150.0f))
      {
        tx_temperature = new_tx_temp;
      }

      /* Overheat detection with hysteresis */
      if (!tx_overheat_flag)
      {
        if (tx_temperature >= TX_OVERHEAT_TRIP_POINT)
        {
          tx_overheat_flag = true;
          if(get_trigger_status() == TRIGGER_STATUS_RUNNING) {
            stop_trigger_pulse();
            // send update
            
          }
        }
      }
      else
      {
        if (tx_temperature <= (TX_OVERHEAT_TRIP_POINT - TX_OVERHEAT_HYSTERESIS))
        {
          tx_overheat_flag = false;
        }
      }
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 16;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_MSI, RCC_MCODIV_2);
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x10805D88;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x10805D88;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Window = 4095;
  hiwdg.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief LPTIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPTIM1_Init(void)
{

  /* USER CODE BEGIN LPTIM1_Init 0 */

  /* USER CODE END LPTIM1_Init 0 */

  /* USER CODE BEGIN LPTIM1_Init 1 */

  /* USER CODE END LPTIM1_Init 1 */
  hlptim1.Instance = LPTIM1;
  hlptim1.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
  hlptim1.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV16;
  hlptim1.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
  hlptim1.Init.OutputPolarity = LPTIM_OUTPUTPOLARITY_HIGH;
  hlptim1.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
  hlptim1.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
  hlptim1.Init.Input1Source = LPTIM_INPUT1SOURCE_GPIO;
  hlptim1.Init.Input2Source = LPTIM_INPUT2SOURCE_GPIO;
  if (HAL_LPTIM_Init(&hlptim1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPTIM1_Init 2 */

  /* USER CODE END LPTIM1_Init 2 */

}

/**
  * @brief LPTIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPTIM2_Init(void)
{

  /* USER CODE BEGIN LPTIM2_Init 0 */

  /* USER CODE END LPTIM2_Init 0 */

  /* USER CODE BEGIN LPTIM2_Init 1 */

  /* USER CODE END LPTIM2_Init 1 */
  hlptim2.Instance = LPTIM2;
  hlptim2.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
  hlptim2.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV1;
  hlptim2.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
  hlptim2.Init.OutputPolarity = LPTIM_OUTPUTPOLARITY_HIGH;
  hlptim2.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
  hlptim2.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
  hlptim2.Init.Input1Source = LPTIM_INPUT1SOURCE_GPIO;
  hlptim2.Init.Input2Source = LPTIM_INPUT2SOURCE_GPIO;
  if (HAL_LPTIM_Init(&hlptim2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPTIM2_Init 2 */

  /* USER CODE END LPTIM2_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 48-1;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 1000-1;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_ENABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 48-1;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 1000-1;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM7 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM7_Init(void)
{

  /* USER CODE BEGIN TIM7_Init 0 */

  /* USER CODE END TIM7_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM7_Init 1 */

  /* USER CODE END TIM7_Init 1 */
  htim7.Instance = TIM7;
  htim7.Init.Prescaler = 4800-1;
  htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim7.Init.Period = 1000-1;
  htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim7) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim7, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM7_Init 2 */

  /* USER CODE END TIM7_Init 2 */

}

/**
  * @brief TIM15 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM15_Init(void)
{

  /* USER CODE BEGIN TIM15_Init 0 */

  /* USER CODE END TIM15_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_SlaveConfigTypeDef sSlaveConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM15_Init 1 */

  /* USER CODE END TIM15_Init 1 */
  htim15.Instance = TIM15;
  htim15.Init.Prescaler = 48-1;
  htim15.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim15.Init.Period = 1000-1;
  htim15.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim15.Init.RepetitionCounter = 0;
  htim15.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim15) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim15, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim15) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_OnePulse_Init(&htim15, TIM_OPMODE_SINGLE) != HAL_OK)
  {
    Error_Handler();
  }
  sSlaveConfig.SlaveMode = TIM_SLAVEMODE_TRIGGER;
  sSlaveConfig.InputTrigger = TIM_TS_ITR0;
  if (HAL_TIM_SlaveConfigSynchro(&htim15, &sSlaveConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim15, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1000;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim15, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
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
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim15, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM15_Init 2 */

  /* USER CODE END TIM15_Init 2 */
  HAL_TIM_MspPostInit(&htim15);

}

/**
  * @brief TIM16 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM16_Init(void)
{

  /* USER CODE BEGIN TIM16_Init 0 */

  /* USER CODE END TIM16_Init 0 */

  /* USER CODE BEGIN TIM16_Init 1 */

  /* USER CODE END TIM16_Init 1 */
  htim16.Instance = TIM16;
  htim16.Init.Prescaler = 4800-1;
  htim16.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim16.Init.Period = 5000-1;
  htim16.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim16.Init.RepetitionCounter = 0;
  htim16.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim16) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM16_Init 2 */

  /* USER CODE END TIM16_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_HalfDuplex_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_HalfDuplex_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
  /* DMA1_Channel3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  /* DMA1_Channel4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
  /* DMA1_Channel5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
  /* DMA1_Channel6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
  /* DMA1_Channel7_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel7_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TR1_EN_Pin|REFSEL_Pin|TR3_EN_Pin|HW_SW_CTRL_Pin
                          |TR2_EN_Pin|TR7_EN_Pin|TR6_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, TX1_CS_Pin|TX2_CS_Pin|TR8_EN_Pin|TX_STDBY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, TR4_EN_Pin|LD_HB_Pin|TX_RESET_L_Pin|TX_CW_EN_Pin
                          |TR5_EN_Pin|RDY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : INT_Pin */
  GPIO_InitStruct.Pin = INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(INT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : GPIO_1_Pin TX1_SHUTZ_Pin RX_I2C_SDA_Pin PC1
                           RX_I2C_SCL_Pin RX_RDY_Pin PC3 */
  GPIO_InitStruct.Pin = GPIO_1_Pin|TX1_SHUTZ_Pin|RX_I2C_SDA_Pin|GPIO_PIN_1
                          |RX_I2C_SCL_Pin|RX_RDY_Pin|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : TR1_EN_Pin REFSEL_Pin TR3_EN_Pin HW_SW_CTRL_Pin
                           TR2_EN_Pin TR7_EN_Pin TR6_EN_Pin */
  GPIO_InitStruct.Pin = TR1_EN_Pin|REFSEL_Pin|TR3_EN_Pin|HW_SW_CTRL_Pin
                          |TR2_EN_Pin|TR7_EN_Pin|TR6_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PDN_Pin EXT_Pin */
  GPIO_InitStruct.Pin = PDN_Pin|EXT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : TX1_CS_Pin TX2_CS_Pin TR8_EN_Pin TX_STDBY_Pin */
  GPIO_InitStruct.Pin = TX1_CS_Pin|TX2_CS_Pin|TR8_EN_Pin|TX_STDBY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : TR4_EN_Pin LD_HB_Pin TX_RESET_L_Pin TX_CW_EN_Pin
                           TR5_EN_Pin RDY_Pin */
  GPIO_InitStruct.Pin = TR4_EN_Pin|LD_HB_Pin|TX_RESET_L_Pin|TX_CW_EN_Pin
                          |TR5_EN_Pin|RDY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : SYSTEM_RDY_Pin */
  GPIO_InitStruct.Pin = SYSTEM_RDY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SYSTEM_RDY_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : REF_CLK_Pin */
  GPIO_InitStruct.Pin = REF_CLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(REF_CLK_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : TX2_SHUTZ_Pin POWER_GOOD_Pin */
  GPIO_InitStruct.Pin = TX2_SHUTZ_Pin|POWER_GOOD_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : RST_Pin (PA1) — inter-board READY line.
   * Default it to open-drain driven LOW ("not ready") from the first GPIO
   * setup, before the USB role is known. The secure bootloader also holds this
   * line LOW while it runs, so defaulting it LOW here means there is no window
   * where the line floats HIGH between the bootloader releasing it and
   * ConfigureResetPin()/configure_slave() taking over — which would let the
   * master enumerate this board before it is actually ready. ConfigureResetPin()
   * later switches it to input-pullup on the master (the reader) or keeps it
   * open-drain on a slave (released to Hi-Z = ready once configure_slave()
   * completes). */
  GPIO_InitStruct.Pin = RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RST_GPIO_Port, &GPIO_InitStruct);
  HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_RESET); /* LOW = not ready */

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{

  if (huart->Instance == CALL_IN_UART.Instance)
  {
    comms_handle_ow_CallIn_RxEventCallback(huart, Size);
  }
  else if (huart->Instance == CALL_OUT_UART.Instance)
  {
    comms_handle_ow_CallOut_RxEventCallback(huart, Size);
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
#ifdef DEBUG_ENABLED
  if (huart->Instance == DEBUG_UART.Instance)
  {
    logging_UART_TxCpltCallback(huart);
    return;
  }
#endif
  if (huart->Instance == CALL_OUT_UART.Instance)
  {
    comms_handle_ow_CallOut_TxCpltCallback(huart);
  }
  else if (huart->Instance == CALL_IN_UART.Instance)
  {
    comms_handle_ow_CallIn_TxCpltCallback(huart);
  }
}

void delay_ms(uint32_t ms)
{
  FW_DEBUG("Clock: %ld\r\n", SystemCoreClock);
  uint32_t delay_cycles = (SystemCoreClock / 1000) * ms;
  while (delay_cycles--)
  {
    __NOP(); // Ensures the loop doesn't get optimized away
  }
}

void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{

  if (hlptim->Instance == RESET_TIMER.Instance)
  {
    // Stop the timer to prevent re-triggering
    HAL_LPTIM_Counter_Stop_IT(hlptim);

    delay_ms(100);

    if (_enter_dfu)
    {
      if (is_custom_bootloader_present() && _force_stm32_dfu == false)
      {
        /* SBSFU secure bootloader present — request DFU mode by writing the
         * one-shot magic to RTC->BKP7R and resetting. The bootloader consumes
         * the magic, skips launching the application, and enters DFU (USB
         * DfuSe when a host is attached, otherwise I2C slave at 0x72). */
        bl_bkp_enable();
        RTC->BKP7R = BL_BKP_REQ_DFU_MAGIC;
      }
      else
      {
        // jump to bootloader DFU
        // 16k SRAM in address 0x2000 0000 - 0x2000 3FFF
        *((unsigned long *)0x20003FF0) = 0xDEADBEEF;
      }

    }

    MX_USB_DEVICE_DeInit();
    __DSB();
    __ISB();
    delay_ms(200);
    NVIC_SystemReset();
  }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
// cppcheck-suppress constParameterPointer -- must match the HAL weak callback signature (non-const TIM_HandleTypeDef *)
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  if (htim->Instance == TIM16)
  {
    (void)HAL_IWDG_Refresh(&hiwdg);
  }

  if (htim->Instance == CDC_TIMER.Instance)
  {
    CDC_Idle_Timer_Handler();
  }

  if (htim->Instance == TIM1)
  {
    TRIG_TIM1_IRQHandler();
  }

  if (htim->Instance == TIM2)
  {
    TRIG_TIM2_IRQHandler();
  }

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_SET);
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
