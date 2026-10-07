/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef LIFU_CONFIG_H
#define LIFU_CONFIG_H

#include "stm32l443xx.h"
#include "flash_eeprom.h"
#include "memory_map.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Magic/version identifiers stored in flash
#define LIFU_MAGIC   (0x4C494655UL)  // 'LIFU'
#define LIFU_VER     (0x00010002UL)  // bump if layout changes

// Flash layout info: one 2KB page at 0x0801F000
#define LIFU_CFG_PAGE_ADDR      (ADDR_FLASH_PAGE_127)
#define LIFU_CFG_PAGE_END       (ADDR_FLASH_END_ADDRESS)
#define LIFU_CFG_PAGE_SIZE      (2048U)

// magic(4) + version(4) + seq(4) + hv_settng(2) + hv_enabled(1) + auto_on(1) = 16 bytes
#define LIFU_CFG_HEADER_SIZE    (16U)


// The rest of the page is JSON storage (must include '\0'):
// 2048 - 16 - 2 = 2030
#define LIFU_CFG_JSON_MAX       (LIFU_CFG_PAGE_SIZE - LIFU_CFG_HEADER_SIZE)
// -> 2030 bytes

// Persistent config blob that exactly fills one flash page.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;        // LIFU_MAGIC
    uint32_t version;      // LIFU_VER
    uint32_t seq;          // monotonic counter
    
    uint16_t crc;          // CRC16-CCITT over bytes [0 .. offsetof(crc)-1]
    uint16_t reserved;

    char     json[LIFU_CFG_JSON_MAX]; // NUL-terminated text blob

} lifu_cfg_t;

// Sanity checks for layout
_Static_assert(sizeof(lifu_cfg_t) == LIFU_CFG_PAGE_SIZE,
               "lifu_cfg_t must fill one 2KB flash page");
_Static_assert((sizeof(lifu_cfg_t) % 4U) == 0U,
               "lifu_cfg_t size must be 32-bit word aligned for flash writes");

// ======================== PUBLIC API ========================

// Returns pointer to the live in-RAM copy of the config.
// On first call, it will load from flash, validate magic/version/CRC,
// and if invalid it writes factory defaults to flash and returns that.
const lifu_cfg_t *lifu_cfg_get(void);

// Copies the current config into *out so you can edit it offline.
// Example:
//    lifu_cfg_t work;
//    lifu_cfg_snapshot(&work);
//    work.hv_settng = 123;
//    lifu_cfg_save(&work);
HAL_StatusTypeDef lifu_cfg_snapshot(lifu_cfg_t *out);

// Saves a modified struct to flash.
// - You pass in a struct you edited (typically from lifu_cfg_snapshot).
// - We copy fields we care about into internal storage,
//   bump seq, recalc CRC, erase/program flash.
HAL_StatusTypeDef lifu_cfg_save(const lifu_cfg_t *new_cfg);

// Commits the *current* live config (as returned by lifu_cfg_get())
// back to flash. This is only needed if you directly mutate *lifu_cfg_get().
HAL_StatusTypeDef lifu_cfg_commit(void);

// Restores factory defaults and writes them to flash.
HAL_StatusTypeDef lifu_cfg_factory_reset(void);

// Convenience helpers for working with the JSON blob.
// - lifu_cfg_get_json_ptr() returns a pointer into the live config (NUL-terminated).
// - lifu_cfg_set_json() persists the provided JSON text into flash.
const char *lifu_cfg_get_json_ptr(void);
HAL_StatusTypeDef lifu_cfg_set_json(const char *json, size_t len);

// ======================== WIRE FORMAT (UART/USB) ========================
// When sending config over the command interface, we serialize as:
//   [lifu_cfg_wire_hdr_t][json bytes (json_len)]
// For WRITE, you may send either:
//   - full wire buffer (header+json), OR
//   - raw JSON bytes (no header)
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint16_t crc;
    uint16_t json_len; // number of JSON bytes included (may include trailing '\0')
} lifu_cfg_wire_hdr_t;

// Produces a pointer to an internal buffer containing the serialized config.
// max_payload_len is the caller's maximum allowed payload size.
HAL_StatusTypeDef lifu_cfg_wire_read(const uint8_t **out_buf,
                                       uint16_t *out_len,
                                       uint16_t max_payload_len);

// Applies a serialized buffer to config and writes it to flash.
// Accepts either full wire buffer (header+json) or raw JSON bytes.
HAL_StatusTypeDef lifu_cfg_wire_write(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif // LIFU_CONFIG_H
