/*
 * Host-side test of the preset slots (Core/Src/presets.c, presets.h): 10
 * slots of 4 KB, each a PRS1 header then a preset blob.
 *
 * Built by tests/unit/run.sh for a bare-metal standard image (-DBARE_METAL:
 * the slots are always there), a standard image under the bootloader (the
 * slots exist only when the bootloader's info block says so), and the
 * clinical image (-DFDA_MODE: reads only). Flash is an array here
 * (PRESETS_HOST_FLASH); the blob is the fixture's generated preset and
 * expected.h what the host computes for it.
 */

#include "common.h"
#include "memory_map.h"
#include "presets.h"
#include "trigger.h"
#include "tx7332.h"

#include "expected.h"
#include "presets/fda_preset.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                            \
	do {                                                            \
		if (!(cond)) {                                              \
			failures++;                                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
			printf(__VA_ARGS__);                                    \
			printf("\n");                                           \
		}                                                           \
	} while (0)

// What presets.c calls.
TX7332 transmitters[TX_PER_MODULE];
uint32_t apod_registers[MAX_PROFILES][TX_PER_MODULE];
volatile float tx_temperature;
volatile uint32_t tx_temperature_tick;
volatile bool tx_temperature_valid;

void TX7332_WriteReg(TX7332 *device, uint16_t addr, uint32_t val) { (void)device; (void)addr; (void)val; }
void TX7332_LoadProfile(TX7332 *device) { (void)device; }
void TX7332_SetActiveDelayProfile(uint8_t profile, TX7332 *device, uint8_t chip_index)
{
	(void)profile; (void)device; (void)chip_index;
}
void TX7332_ResetApodizations(void) { }
void profile_cache_note_write(uint8_t tx_idx, uint16_t start_addr, uint16_t reg_count)
{
	(void)tx_idx; (void)start_addr; (void)reg_count;
}
bool profile_cycle_install(uint8_t profile_count, const uint8_t *order, uint8_t len)
{
	(void)profile_count; (void)order; (void)len;
	return true;
}
void auto_cycle_stop(void) { }
uint8_t get_trigger_status(void) { return TRIGGER_STATUS_READY; }
bool trigger_set_config(uint32_t frequency_hz, uint32_t pulse_width_us, uint32_t pulse_count,
                        uint32_t train_interval_us, uint32_t train_count, uint32_t mode)
{
	(void)frequency_hz; (void)pulse_width_us; (void)pulse_count; (void)train_interval_us;
	(void)train_count; (void)mode;
	return true;
}
uint32_t HAL_GetTick(void) { return 0U; }

// Flash: the whole 256 KB, erased.
static uint8_t flash[0x40000];

const uint8_t *presets_host_flash(uint32_t address)
{
	return &flash[address - FLASH_START_ADDRESS];
}

#ifdef PRESET_STORE
static unsigned erases;

HAL_StatusTypeDef Flash_Erase(uint32_t start_address, uint32_t end_address)
{
	erases++;
	memset(&flash[start_address - FLASH_START_ADDRESS], 0xFF, end_address - start_address);
	return HAL_OK;
}

HAL_StatusTypeDef Flash_Write(uint32_t address, const void *src, uint32_t size_bytes)
{
	uint8_t *dst = &flash[address - FLASH_START_ADDRESS];
	const uint8_t *s = src;
	for (uint32_t i = 0; i < size_bytes; i++) {
		if (dst[i] != 0xFFU) return HAL_ERROR;   // flash: written bytes stay written
		dst[i] = s[i];
	}
	return HAL_OK;
}
#endif

static void wr_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// A slot image: a PRS1 header (zero signature: the app does not check it)
// carrying the blob's size, then the blob.
static uint32_t make_image(uint8_t *image, const uint8_t *blob, uint32_t blob_len)
{
	memset(image, 0, PRESET_HEADER_LEN);
	memcpy(image, PRESET_HEADER_MAGIC, 4U);
	image[4] = 1U;                          // protocol
	wr_u32(&image[8], blob_len);            // FwSize
	wr_u32(&image[20], blob_len);           // PartialFwSize
	memset(&image[192], 0xFF, 96U);         // image state
	memcpy(&image[PRESET_HEADER_LEN], blob, blob_len);
	return PRESET_HEADER_LEN + blob_len;
}

#ifndef BARE_METAL
static void write_info_block(void)
{
	uint8_t *info = &flash[BL_INFO_ADDRESS - FLASH_START_ADDRESS];
	memset(info, 0, 32U);
	wr_u32(&info[0], BL_INFO_MAGIC);
	info[4] = 1U;
	wr_u32(&info[8], APPLICATION_ADDRESS);
	wr_u32(&info[12], PRESET_STORE_ADDRESS);
	wr_u32(&info[16], PRESET_STORE_ADDRESS);
	wr_u32(&info[20], PRESET_STORE_SIZE);
	wr_u32(&info[24], 3U);
}
#endif

static void put_slot(uint8_t slot, const uint8_t *image, uint32_t len)
{
	memcpy(&flash[PRESET_SLOT_ADDRESS(slot) - FLASH_START_ADDRESS], image, len);
}

static void check_reads_slot(uint8_t slot)
{
	Preset p;
	CHECK(presets_slot_state(slot) == OW_SUCCESS, "slot %u: no preset", slot);
	CHECK(presets_get(slot, &p), "slot %u does not parse", slot);
	CHECK(p.regs_crc == expected[0].regs_crc && presets_regs_crc(&p) == expected[0].regs_crc,
	      "slot %u regs_crc 0x%08x", slot, p.regs_crc);
	CHECK(presets_header(slot) == presets_host_flash(PRESET_SLOT_ADDRESS(slot)), "slot %u header", slot);
	CHECK(presets_load(slot, 0U, expected[0].regs_crc, expected[0].settings_crc), "slot %u load", slot);
}

static void check_empty(const char *what)
{
	for (uint8_t i = 0; i < PRESET_SLOTS; i++) {
		Preset p;
		CHECK(presets_slot_state(i) == OW_PRESET_EMPTY, "%s: slot %u not empty", what, i);
		CHECK(!presets_get(i, &p) && presets_header(i) == NULL, "%s: slot %u reads", what, i);
	}
	CHECK(presets_slot_state(PRESET_SLOTS) == OW_INVALID_PACKET, "%s: index past the slots", what);
}

#ifdef PRESET_STORE
static uint8_t store(uint8_t slot, const uint8_t *image, uint32_t len, uint32_t crc)
{
	for (uint32_t off = 0; off < len; off += 512U) {
		uint32_t n = (len - off < 512U) ? len - off : 512U;
		uint8_t r = presets_stage(slot, off, &image[off], (uint16_t)n);
		if (r != OW_SUCCESS) return r;
	}
	return presets_commit(slot, len, crc);
}

static void check_store(void)
{
	static uint8_t image[PRESET_SLOT_SIZE];
	uint32_t len = make_image(image, FDA_PRESET_BLOB, sizeof FDA_PRESET_BLOB);
	uint32_t crc = presets_crc32(image, len);

	CHECK(presets_free_slots() == PRESET_SLOTS, "free slots %u", presets_free_slots());
	CHECK(store(3, image, len, crc) == OW_SUCCESS, "store into slot 3");
	check_reads_slot(3);
	CHECK(presets_free_slots() == PRESET_SLOTS - 1U, "free slots after a store");

	// Refused, with nothing changed: wrong CRC, a header size that is not the
	// blob's, a blob whose body no longer matches regs_crc, no PRS1 magic.
	unsigned before = erases;
	CHECK(store(4, image, len, crc ^ 1U) == OW_BAD_CRC, "wrong CRC");
	image[8] ^= 1U;
	CHECK(store(4, image, len, presets_crc32(image, len)) == OW_BAD_CRC, "wrong header size");
	image[8] ^= 1U;
	image[len - 1U] ^= 1U;
	CHECK(store(4, image, len, presets_crc32(image, len)) == OW_BAD_CRC, "altered body");
	image[len - 1U] ^= 1U;
	image[0] = 'X';
	CHECK(store(4, image, len, presets_crc32(image, len)) == OW_BAD_CRC, "no PRS1 magic");
	image[0] = 'P';
	CHECK(erases == before && presets_slot_state(4) == OW_PRESET_EMPTY, "a refused store touched flash");

	// Upload rules: a part out of order or for another slot abandons it, and
	// an upload gets one commit; a slot image never exceeds a slot.
	CHECK(presets_stage(5, 0U, image, 64U) == OW_SUCCESS, "first part");
	CHECK(presets_stage(5, 128U, &image[128], 64U) == OW_INVALID_PACKET, "gap");
	CHECK(presets_commit(5, 64U, presets_crc32(image, 64U)) == OW_INVALID_PACKET, "commit after a gap");
	CHECK(store(5, image, len, crc) == OW_SUCCESS && presets_commit(5, len, crc) == OW_INVALID_PACKET,
	      "a second commit");
	CHECK(presets_stage(6, 0U, image, 1U) == OW_SUCCESS &&
	      presets_stage(6, 1U, image, (uint16_t)PRESET_SLOT_SIZE) == OW_INVALID_PACKET, "over a slot");
	CHECK(presets_stage(PRESET_SLOTS, 0U, image, 1U) == OW_INVALID_PACKET, "slot past the end");

	// Replace, delete, delete again.
	CHECK(store(3, image, len, crc) == OW_SUCCESS, "store over a stored slot");
	check_reads_slot(3);
	CHECK(presets_delete(3) == OW_SUCCESS && presets_slot_state(3) == OW_PRESET_EMPTY, "delete");
	before = erases;
	CHECK(presets_delete(3) == OW_SUCCESS && erases == before, "deleting an erased slot erases nothing");
	memset(&flash[PRESET_SLOT_ADDRESS(7) - FLASH_START_ADDRESS], 0, 16U);   // remains, no preset
	CHECK(presets_slot_state(7) == OW_PRESET_EMPTY && presets_delete(7) == OW_SUCCESS &&
	      flash[PRESET_SLOT_ADDRESS(7) - FLASH_START_ADDRESS] == 0xFFU, "delete clears remains");
	CHECK(presets_delete(PRESET_SLOTS) == OW_INVALID_PACKET, "delete past the end");
}
#endif

int main(void)
{
	static uint8_t image[PRESET_SLOT_SIZE];
	uint32_t len = make_image(image, FDA_PRESET_BLOB, sizeof FDA_PRESET_BLOB);
	memset(flash, 0xFF, sizeof flash);

	CHECK(presets_count() == 10U && PRESET_SLOT_SIZE == 4096U && PRESET_BLOB_MAX == 3776U,
	      "10 slots of 4 KB");
	check_empty("erased flash");

#ifdef BARE_METAL
	CHECK(presets_have_slots(), "a bare-metal image always has the slots");
#else
	// Under a bootloader without the info block (before 1.1.0-rc.1) the
	// slots are part of its slot: they read as empty, even with a preset in
	// them, and nothing is stored.
	put_slot(2, image, len);
	CHECK(!presets_have_slots(), "slots without the bootloader's info block");
	check_empty("no info block");
#ifdef PRESET_STORE
	CHECK(presets_stage(1, 0U, image, (uint16_t)len) == OW_SUCCESS &&
	      presets_commit(1, len, presets_crc32(image, len)) == OW_PRESET_NO_ROOM, "store without slots");
	CHECK(presets_delete(1) == OW_PRESET_NO_ROOM, "delete without slots");
#endif
	write_info_block();
	CHECK(presets_have_slots(), "slots with the info block");
	check_reads_slot(2);
	memset(&flash[PRESET_SLOT_ADDRESS(2) - FLASH_START_ADDRESS], 0xFF, PRESET_SLOT_SIZE);
#endif

	// A preset put in a slot (by the bootloader) is read, whatever the image.
	put_slot(9, image, len);
	check_reads_slot(9);
	memset(&flash[PRESET_SLOT_ADDRESS(9) - FLASH_START_ADDRESS], 0xFF, PRESET_SLOT_SIZE);

#ifdef PRESET_STORE
	check_store();
#endif

	printf("%s: preset slots, %d failure(s)\n",
#if defined(FDA_MODE)
	       "FDA build",
#elif defined(BARE_METAL)
	       "bare-metal build",
#else
	       "slot build",
#endif
	       failures);
	return failures ? 1 : 0;
}
