#include "lifu_config.h"

#include "common.h" 

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

static lifu_cfg_t g_cfg;
static bool       g_cfg_loaded = false;

static uint8_t g_cfg_wire_buf[DATA_MAX_SIZE];

// ------------------- CRC16-CCITT -------------------
// CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no final XOR.
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint16_t lifu_cfg_calc_crc(const lifu_cfg_t *cfg)
{
    // compute CRC across everything BEFORE the crc field
    return crc16_ccitt((const uint8_t *)cfg,
                       offsetof(lifu_cfg_t, crc));
}

// ------------------- Helpers -------------------

// Ensure json is NUL-terminated and zero-pad tail so CRC is stable
static void lifu_cfg_normalize_json(lifu_cfg_t *cfg)
{
    cfg->json[LIFU_CFG_JSON_MAX - 1U] = '\0';

    size_t used = strnlen(cfg->json, LIFU_CFG_JSON_MAX);
    if (used < (LIFU_CFG_JSON_MAX - 1U)) {
        memset(&cfg->json[used + 1U],
               0U,
               (LIFU_CFG_JSON_MAX - 1U) - used);
    }
}

// Build a clean default config in RAM
static void lifu_cfg_make_defaults(lifu_cfg_t *dst)
{
    memset(dst, 0, sizeof(lifu_cfg_t));

    dst->magic      = LIFU_MAGIC;
    dst->version    = LIFU_VER;
    dst->seq        = 0;

    /* Initialize JSON with sensible defaults so an empty config contains useful values. */
    {
        const char *default_json = "{\"SN\": 12345678}";
        /* Ensure we never overflow the JSON buffer. */
        snprintf(dst->json, LIFU_CFG_JSON_MAX, "%s", default_json);
    }

    lifu_cfg_normalize_json(dst);
    dst->crc        = lifu_cfg_calc_crc(dst);
}

// Validate magic, version, CRC, and that JSON is terminated
static bool lifu_cfg_is_valid(const lifu_cfg_t *cfg)
{
    if (cfg->magic   != LIFU_MAGIC) { return false; }
    if (cfg->version != LIFU_VER)   { return false; }

    // json must contain a '\0' somewhere in range
    if (memchr(cfg->json, '\0', LIFU_CFG_JSON_MAX) == NULL) {
        return false;
    }

    uint16_t calc = lifu_cfg_calc_crc(cfg);
    if (calc != cfg->crc)           { return false; }

    return true;
}

// Do a complete write of g_cfg into flash page.
// - bumps seq
// - normalizes json
// - recomputes crc
// - erases page and writes words
static HAL_StatusTypeDef lifu_cfg_writeback(void)
{
    HAL_StatusTypeDef st;

    // bump monotonic sequence
    g_cfg.seq++;

    lifu_cfg_normalize_json(&g_cfg);
    g_cfg.crc = lifu_cfg_calc_crc(&g_cfg);

    // Erase page [ADDR_FLASH_PAGE_62 .. ADDR_FLASH_PAGE_63)
    st = Flash_Erase(LIFU_CFG_PAGE_ADDR, LIFU_CFG_PAGE_END);
    if (st != HAL_OK) {
        return st;
    }

    // Program entire struct (full 2048 bytes)
    st = Flash_Write(LIFU_CFG_PAGE_ADDR,
                     &g_cfg,
                     sizeof(lifu_cfg_t));

    return st;
}

// Raw load from flash into g_cfg
static void lifu_cfg_load_raw(void)
{
    Flash_Read(LIFU_CFG_PAGE_ADDR,
               &g_cfg,
               sizeof(lifu_cfg_t));
}

// Ensure g_cfg is initialized and valid
static void lifu_cfg_ensure_loaded(void)
{
    if (g_cfg_loaded) {
        return;
    }

    lifu_cfg_load_raw();

    if (!lifu_cfg_is_valid(&g_cfg)) {
        // First boot or corrupt -> defaults and persist
        lifu_cfg_make_defaults(&g_cfg);
        (void)lifu_cfg_writeback();
    }

    g_cfg_loaded = true;
}

// ------------------- Public API -------------------

const lifu_cfg_t *lifu_cfg_get(void)
{
    lifu_cfg_ensure_loaded();
    return &g_cfg;
}

const char *lifu_cfg_get_json_ptr(void)
{
    lifu_cfg_ensure_loaded();
    return g_cfg.json;
}

HAL_StatusTypeDef lifu_cfg_set_json(const char *json, size_t len)
{
    if (json == NULL) {
        return HAL_ERROR;
    }

    lifu_cfg_ensure_loaded();

    // Copy up to max-1 so we can always NUL-terminate.
    if (len >= LIFU_CFG_JSON_MAX) {
        len = LIFU_CFG_JSON_MAX - 1U;
    }

    memcpy(g_cfg.json, json, len);
    g_cfg.json[len] = '\0';

    // Persist.
    return lifu_cfg_writeback();
}

HAL_StatusTypeDef lifu_cfg_wire_read(const uint8_t **out_buf,
                                       uint16_t *out_len,
                                       uint16_t max_payload_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return HAL_ERROR;
    }

    lifu_cfg_ensure_loaded();

    if (max_payload_len > (uint16_t)sizeof(g_cfg_wire_buf)) {
        max_payload_len = (uint16_t)sizeof(g_cfg_wire_buf);
    }

    if (max_payload_len < (uint16_t)sizeof(lifu_cfg_wire_hdr_t)) {
        return HAL_ERROR;
    }

    lifu_cfg_wire_hdr_t hdr;
    hdr.magic = g_cfg.magic;
    hdr.version = g_cfg.version;
    hdr.seq = g_cfg.seq;
    hdr.crc = g_cfg.crc;

    const uint16_t max_json = (uint16_t)(max_payload_len - sizeof(lifu_cfg_wire_hdr_t));

    size_t json_total = strnlen(g_cfg.json, LIFU_CFG_JSON_MAX);
    // include '\0' if present/space allows
    if (json_total < LIFU_CFG_JSON_MAX) {
        json_total += 1U;
    }

    uint16_t json_len = (uint16_t)((json_total > max_json) ? max_json : json_total);
    hdr.json_len = json_len;

    memcpy(g_cfg_wire_buf, &hdr, sizeof(hdr));
    if (json_len > 0U) {
        memcpy(&g_cfg_wire_buf[sizeof(hdr)], g_cfg.json, json_len);
    }

    *out_buf = g_cfg_wire_buf;
    *out_len = (uint16_t)(sizeof(hdr) + json_len);
    return HAL_OK;
}

HAL_StatusTypeDef lifu_cfg_wire_write(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U) {
        return HAL_ERROR;
    }

    // Try to parse full wire format first.
    if (len >= (uint16_t)sizeof(lifu_cfg_wire_hdr_t)) {
        lifu_cfg_wire_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));

        uint32_t expected_magic = LIFU_MAGIC;
        uint32_t expected_ver = LIFU_VER;

        if (hdr.magic == expected_magic && hdr.version == expected_ver) {
            uint32_t total = (uint32_t)sizeof(lifu_cfg_wire_hdr_t) + (uint32_t)hdr.json_len;
            if (total <= (uint32_t)len) {
                const uint8_t *json_ptr = &buf[sizeof(lifu_cfg_wire_hdr_t)];
                return lifu_cfg_set_json((const char *)json_ptr, (size_t)hdr.json_len);
            }
        }
    }

    // Fallback: treat payload as raw JSON bytes.
    return lifu_cfg_set_json((const char *)buf, (size_t)len);
}

HAL_StatusTypeDef lifu_cfg_snapshot(lifu_cfg_t *out)
{
    if (out == NULL) {
        return HAL_ERROR;
    }

    lifu_cfg_ensure_loaded();
    memcpy(out, &g_cfg, sizeof(lifu_cfg_t));
    return HAL_OK;
}

HAL_StatusTypeDef lifu_cfg_save(const lifu_cfg_t *new_cfg)
{
    lifu_cfg_ensure_loaded();

    // Copy caller-updated fields into our working config.
    // We *trust* their chosen values for hv_settng/hv_enabled/auto_on/json.
    // We IGNORE their seq/crc/magic/version and regenerate those.

    g_cfg.magic      = LIFU_MAGIC;
    g_cfg.version    = LIFU_VER;

    // Copy JSON safely. Caller might not have padded or '\0' at the end.
    memcpy(g_cfg.json, new_cfg->json, LIFU_CFG_JSON_MAX);
    g_cfg.json[LIFU_CFG_JSON_MAX - 1U] = '\0';

    // Now write full page with updated data
    return lifu_cfg_writeback();
}

HAL_StatusTypeDef lifu_cfg_commit(void)
{
    lifu_cfg_ensure_loaded();
    // Write current g_cfg (useful if caller directly edited *lifu_cfg_get()).
    return lifu_cfg_writeback();
}

HAL_StatusTypeDef lifu_cfg_factory_reset(void)
{
    lifu_cfg_ensure_loaded();

    lifu_cfg_make_defaults(&g_cfg);
    return lifu_cfg_writeback();
}
