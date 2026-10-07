/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * flash_eeprom.c
 *
 *  Created on: Mar 4, 2024
 *      Author: gvigelet
 */

#include "flash_eeprom.h"

#include <stdio.h>
#include <string.h>


/**
  * @brief  Gets the page of a given address
  * @param  Addr: Address of the FLASH Memory
  * @retval The page of a given address
  */
static uint32_t GetPage(uint32_t Addr)
{
  uint32_t page = 0;

  if (Addr < (FLASH_BASE + FLASH_BANK_SIZE))
  {
    /* Bank 1 */
    page = (Addr - FLASH_BASE) / FLASH_PAGE_SIZE;
  }
  else
  {
    /* Bank 2 */
    page = (Addr - (FLASH_BASE + FLASH_BANK_SIZE)) / FLASH_PAGE_SIZE;
  }

  return page;
}


/* Handle DBANK configuration (single vs dual bank) */
static uint32_t GetBank(uint32_t Addr)
{
#if defined(FLASH_OPTR_DBANK)
  /* If DBANK == 0 → single-bank (Bank1), else dual-bank */
  if ((FLASH->OPTR & FLASH_OPTR_DBANK) == 0U) {
    return FLASH_BANK_1;
  } else {
    return (Addr < (FLASH_BASE + FLASH_BANK_SIZE)) ? FLASH_BANK_1 : FLASH_BANK_2;
  }
#else
  return FLASH_BANK_1;
#endif
}

/* Clear error flags before programming */
static inline void Flash_ClearErrors(void)
{
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGSERR |
                         FLASH_FLAG_SIZERR | FLASH_FLAG_MISERR | FLASH_FLAG_FASTERR |
                         FLASH_FLAG_RDERR | FLASH_FLAG_OPERR | FLASH_FLAG_EOP);
}



/* --- Public: erase pages covering [start, end) --- */
/* end_address is treated as EXCLUSIVE here */
HAL_StatusTypeDef Flash_Erase(uint32_t start_address, uint32_t end_address_exclusive)
{
  if (end_address_exclusive <= start_address) return HAL_ERROR;

  HAL_StatusTypeDef st = HAL_FLASH_Unlock();
  if (st != HAL_OK) return st;

  Flash_ClearErrors();

  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_first = GetPage(start_address);
  uint32_t page_last  = GetPage(end_address_exclusive - 1U);
  uint32_t pages      = (page_last - page_first) + 1U;

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks     = GetBank(start_address);
  erase.Page      = page_first;
  erase.NbPages   = pages;

  uint32_t err = 0;
  st = HAL_FLASHEx_Erase(&erase, &err);

  HAL_FLASH_Lock();
  return st;
}

/* --- Public: read N bytes from flash into buffer --- */
HAL_StatusTypeDef Flash_Read(uint32_t address, void *dst, uint32_t size_bytes)
{
  memcpy(dst, (const void*)address, size_bytes);
  return HAL_OK;
}

/* --- Public: program arbitrary-length buffer using 64-bit writes --- */
/* Requirements:
   - 'address' must be 8-byte aligned (assert/return error otherwise)
   - Flash must be erased (0xFF) on the destination range before calling
*/
HAL_StatusTypeDef Flash_Write(uint32_t address, const void *src, uint32_t size_bytes)
{
  if (size_bytes == 0U) return HAL_OK;
  if ((address & 0x7U) != 0U) return HAL_ERROR;  /* must be 64-bit aligned */

  HAL_StatusTypeDef st = HAL_FLASH_Unlock();
  if (st != HAL_OK) return st;

  Flash_ClearErrors();

  const uint8_t *p = (const uint8_t*)src;
  uint32_t       addr = address;

  /* Program full 8-byte chunks */
  while (size_bytes >= 8U) {
    /* L4 requires DOUBLEWORD (64-bit) programming */
    uint64_t dw;
    memcpy(&dw, p, 8U);

    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw);
    if (st != HAL_OK) { HAL_FLASH_Lock(); return st; }

    /* Optional: verify (cheap and catches alignment/erase issues) */
    if (*(uint64_t*)addr != dw) { HAL_FLASH_Lock(); return HAL_ERROR; }

    p          += 8U;
    addr       += 8U;
    size_bytes -= 8U;
  }

  /* Handle tail (1..7 bytes) by composing one last doubleword */
  if (size_bytes > 0U) {
    uint8_t tmp[8];
    memset(tmp, 0xFF, sizeof(tmp));        /* flash erased state */
    memcpy(tmp, p, size_bytes);

    uint64_t dw;
    memcpy(&dw, tmp, 8U);

    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw);
    if (st != HAL_OK) { HAL_FLASH_Lock(); return st; }
    if (*(uint64_t*)addr != dw) { HAL_FLASH_Lock(); return HAL_ERROR; }
  }

  HAL_FLASH_Lock();
  return HAL_OK;
}
