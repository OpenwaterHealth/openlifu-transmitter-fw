#include "presets.h"

#include "common.h"
#include "if_commands.h"
#include "module_manager.h"
#include "thermistor.h"
#include "trigger.h"
#include "tx7332.h"

#include <stdio.h>
#include <string.h>

#include "memory_map.h"
#ifdef PRESET_STORE
#include "flash_eeprom.h"
#endif

extern TX7332 transmitters[TX_PER_MODULE];

// Thermal limits of the preset the chips were last programmed from; they
// govern every start until another load replaces them. Kept by value so they
// never depend on the blob staying where it was.
static bool preset_loaded = false;
static int16_t loaded_start_c_x10 = 0;
static int16_t loaded_shutoff_c_x10 = 0;
// Which preset, run length and CRCs that was: OW_PRESET_LOADED reports it,
// and storing over its slot unloads it.
static PresetLoaded loaded;

#ifndef FDA_MODE
static bool locked = false;
#endif

// Blob fields are little-endian and unaligned, so they are read a byte at a time.
static uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Bounds-checked reader over a blob: take() refuses to step past the end.
typedef struct {
	const uint8_t *p;
	const uint8_t *end;
} Cursor;

static bool take(Cursor *c, uint32_t n, const uint8_t **out)
{
	if ((uint32_t)(c->end - c->p) < n) return false;
	*out = c->p;
	c->p += n;
	return true;
}

// One chip's base registers: runs of consecutive addresses.
static bool check_runs(Cursor *c)
{
	const uint8_t *f;
	if (!take(c, 2U, &f)) return false;
	for (uint16_t runs = rd_u16(f); runs > 0U; runs--) {
		if (!take(c, 4U, &f)) return false;
		if (!take(c, 4U * (uint32_t)rd_u16(&f[2]), &f)) return false;
	}
	return true;
}

// One profile's registers: address/value pairs.
static bool check_pairs(Cursor *c)
{
	const uint8_t *f;
	if (!take(c, 2U, &f)) return false;
	return take(c, 6U * (uint32_t)rd_u16(f), &f);
}

// Parses PRESET_BLOB (presets.h) and walks every list once, so the loader can
// follow the counts afterwards without checking them again.
static bool parse_blob(const uint8_t *data, uint32_t len, Preset *out)
{
	Cursor c = { data, data + len };
	const uint8_t *f;

	if (!take(&c, 20U, &f)) return false;
	if (memcmp(f, "OWPR", 4U) != 0 || f[4] != PRESET_BLOB_VERSION) return false;
	out->id_len = f[5];
	if (out->id_len < 1U || out->id_len > PRESET_ID_MAX) return false;
	out->module = f[6];
	out->modules = f[7];
	if (out->modules != 0U && out->module >= out->modules) return false;
	if (rd_u32(&f[8]) != len) return false;
	out->settings_crc = rd_u32(&f[12]);
	out->regs_crc = rd_u32(&f[16]);
	if (!take(&c, out->id_len, &f)) return false;
	out->id = (const char *)f;

	out->body = c.p;
	out->body_len = (uint32_t)(c.end - c.p);
	if (!take(&c, 4U, &f)) return false;
	out->chip_count = f[0];
	out->profile_count = f[1];
	out->profile_index = f[2];
	out->exec_order_len = f[3];
	if (out->chip_count < 1U || out->profile_count < 1U || out->profile_count > MAX_PROFILES) return false;
	if (out->profile_index < 1U || out->profile_index > out->profile_count) return false;
	if (out->exec_order_len < 1U) return false;
	if (!take(&c, out->exec_order_len, &out->exec_order)) return false;

	if (!take(&c, 13U, &f)) return false;
	out->trig_frequency_hz = rd_u32(&f[0]);
	out->trig_pulse_count = rd_u32(&f[4]);
	out->trig_train_interval_us = rd_u32(&f[8]);
	out->train_count_len = f[12];
	if (out->train_count_len < 1U) return false;
	if (!take(&c, 4U * (uint32_t)out->train_count_len, &out->train_counts)) return false;

	if (!take(&c, 6U, &f)) return false;
	out->start_c_x10 = (int16_t)rd_u16(&f[0]);
	out->shutoff_c_x10 = (int16_t)rd_u16(&f[2]);
	out->treatment_timeout_s = rd_u16(&f[4]);
	if (out->treatment_timeout_s < 1U) return false;

	out->chips = c.p;
	for (uint8_t chip = 0; chip < out->chip_count; chip++) {
		if (!check_runs(&c)) return false;
		for (uint8_t p = 0; p < out->profile_count; p++) {
			if (!check_pairs(&c)) return false;
		}
	}
	return c.p == c.end;
}

#ifndef FDA_MODE

_Static_assert(PRESET_SLOTS * PRESET_SLOT_SIZE == PRESETS_SIZE,
               "the slots must fill the reserved pages exactly");

static uint32_t slot_address(uint8_t slot)
{
	return PRESETS_ADDRESS + ((uint32_t)slot * PRESET_SLOT_SIZE);
}

static uint8_t slots_for(uint32_t len)
{
	return (len > PRESET_SLOT_SIZE) ? 2U : 1U;
}

// Length of the blob that starts at a slot, or 0 if none does. The header is
// enough to tell: commit only stores a blob that validated, magic last.
static uint32_t header_len(uint8_t slot)
{
	const uint8_t *p = (const uint8_t *)slot_address(slot);
	if (memcmp(p, "OWPR", 4U) != 0) return 0U;
	uint32_t len = rd_u32(&p[8]);
	uint32_t room = (uint32_t)(PRESET_SLOTS - slot) * PRESET_SLOT_SIZE;
	if (room > PRESET_BLOB_MAX) room = PRESET_BLOB_MAX;
	return (len <= room) ? len : 0U;
}

// Slots are claimed front to back, a long blob taking the slot after it, so
// what a slot is depends on the ones before it. Returns the length of the blob
// the slot heads, or 0 with *tail saying whether the preset before owns it.
static uint32_t slot_len(uint8_t slot, bool *tail)
{
	*tail = false;
	for (uint8_t i = 0; i < PRESET_SLOTS; ) {
		uint32_t len = header_len(i);
		if (i == slot) return len;
		if (len > PRESET_SLOT_SIZE && (uint8_t)(i + 1U) == slot) {
			*tail = true;
			return 0U;
		}
		i = (uint8_t)(i + ((len != 0U) ? slots_for(len) : 1U));
	}
	return 0U;
}

// The blob a slot holds, if it heads one.
static bool slot_blob(uint8_t slot, PresetBlob *out)
{
	bool tail;
	if (slot >= PRESET_SLOTS) return false;
	uint32_t len = slot_len(slot, &tail);
	if (len == 0U) return false;
	out->data = (const uint8_t *)slot_address(slot);
	out->len = len;
	return true;
}

#elif defined(PRESET_TABLE) /* FDA_MODE, a set of presets */

// The set, from the SDK's generate_preset_set: index i is its i-th preset.
// The blobs are const data in the image, so what runs is what was flashed.
#include "presets/preset_table.h"

_Static_assert(PRESET_COUNT >= 1u && PRESET_COUNT <= PRESET_SLOTS,
               "a linked set has 1..PRESET_SLOTS presets");
_Static_assert(PRESET_MODULES == 1u,
               "a presets-only image runs 1x presets: every module would need its own column");

#else /* FDA_MODE, one preset */

// The one preset, from the SDK. The linker puts it at the start of the
// FDA_PRESET region; it is read from there, so what runs is what was flashed.
#include "presets/fda_preset.h"
extern const uint8_t _sfda_preset[];

static bool fda_blob(PresetBlob *out)
{
	const uint8_t *p = _sfda_preset;
	if (memcmp(p, "OWPR", 4U) != 0) return false;
	uint32_t len = rd_u32(&p[8]);
	if (len > PRESET_BLOB_MAX) return false;
	out->data = p;
	out->len = len;
	return true;
}

#endif /* FDA_MODE */

uint8_t presets_slot_state(uint8_t slot)
{
	if (slot >= PRESET_COUNT) return OW_INVALID_PACKET;
#ifndef FDA_MODE
	bool tail;
	if (slot_len(slot, &tail) != 0U) return OW_SUCCESS;
	return tail ? OW_PRESET_SLOT_TAIL : OW_PRESET_EMPTY;
#elif defined(PRESET_TABLE)
	return OW_SUCCESS;   // every index below PRESET_COUNT is one of the set
#else
	PresetBlob blob;
	return fda_blob(&blob) ? OW_SUCCESS : OW_PRESET_EMPTY;
#endif
}

#ifdef PRESET_STORE

// The upload in progress; one at a time, and offset 0 starts over.
static uint8_t stage[PRESET_BLOB_MAX];
static uint32_t stage_len = 0;
static uint8_t stage_slot = PRESET_SLOTS;   // none

// Is this a well-formed PRESET_BLOB whose body matches its regs_crc?
static bool blob_valid(const uint8_t *data, uint32_t len)
{
	Preset preset;
	return parse_blob(data, len, &preset) && presets_regs_crc(&preset) == preset.regs_crc;
}

// Slots [first, first + count) are about to change. If the loaded preset is
// among them, nothing counts as loaded until the next load.
static void slots_changing(uint8_t first, uint8_t count)
{
	if (preset_loaded && loaded.index >= first && (uint8_t)(loaded.index - first) < count) {
		preset_loaded = false;
	}
}

uint8_t presets_free_slots(void)
{
	uint8_t free_slots = 0;
	for (uint8_t i = 0; i < PRESET_SLOTS; ) {
		uint32_t len = header_len(i);
		if (len == 0U) free_slots++;
		i = (uint8_t)(i + ((len != 0U) ? slots_for(len) : 1U));
	}
	return free_slots;
}

uint8_t presets_stage(uint8_t slot, uint32_t offset, const uint8_t *data, uint16_t len)
{
	if (slot >= PRESET_SLOTS) return OW_INVALID_PACKET;
	if (offset == 0U) {
		stage_slot = slot;
		stage_len = 0U;
	}
	if (slot != stage_slot || offset != stage_len || len == 0U || len > (PRESET_BLOB_MAX - stage_len)) {
		stage_slot = PRESET_SLOTS;
		return OW_INVALID_PACKET;
	}
	memcpy(&stage[stage_len], data, len);
	stage_len += len;
	return OW_SUCCESS;
}

uint8_t presets_commit(uint8_t slot, uint32_t total_len, uint32_t blob_crc)
{
	bool tail;
	uint8_t staged = stage_slot;
	stage_slot = PRESET_SLOTS;   // an upload gets one commit, pass or fail

	if (slot >= PRESET_SLOTS || slot != staged || total_len != stage_len) return OW_INVALID_PACKET;
	if (presets_crc32(stage, stage_len) != blob_crc || !blob_valid(stage, stage_len)) return OW_BAD_CRC;

	uint32_t old_len = slot_len(slot, &tail);
	if (tail) return OW_PRESET_SLOT_TAIL;
	uint8_t new_slots = slots_for(stage_len);
	uint8_t old_slots = slots_for(old_len);
	uint8_t next = (uint8_t)(slot + 1U);
	if (new_slots > old_slots && (next >= PRESET_SLOTS || header_len(next) != 0U)) return OW_PRESET_NO_ROOM;

	// A flash erase stalls the core for milliseconds, trigger ISRs included.
	if (get_trigger_status() == TRIGGER_STATUS_RUNNING) return OW_PRESET_BUSY;

	// Erase whatever the old blob held as well, so a shorter one frees its second slot.
	uint8_t span = (new_slots > old_slots) ? new_slots : old_slots;
	uint32_t base = slot_address(slot);
	uint32_t end = base + ((uint32_t)span * PRESET_SLOT_SIZE);
	slots_changing(slot, span);
	if (Flash_Erase(base, end) != HAL_OK) return OW_UNKNOWN_ERROR;
	if (Flash_Write(base + 8U, &stage[8], stage_len - 8U) != HAL_OK ||
	    Flash_Write(base, stage, 8U) != HAL_OK ||
	    memcmp((const void *)base, stage, stage_len) != 0) {
		(void)Flash_Erase(base, end);
		return OW_UNKNOWN_ERROR;
	}
	return OW_SUCCESS;
}

uint8_t presets_delete(uint8_t slot)
{
	bool tail;
	if (slot >= PRESET_SLOTS) return OW_INVALID_PACKET;
	uint32_t len = slot_len(slot, &tail);
	if (tail) return OW_PRESET_SLOT_TAIL;
	if (len == 0U) return OW_SUCCESS;
	if (get_trigger_status() == TRIGGER_STATUS_RUNNING) return OW_PRESET_BUSY;

	uint32_t base = slot_address(slot);
	slots_changing(slot, slots_for(len));
	if (Flash_Erase(base, base + ((uint32_t)slots_for(len) * PRESET_SLOT_SIZE)) != HAL_OK) return OW_UNKNOWN_ERROR;
	return OW_SUCCESS;
}

#endif /* PRESET_STORE */

uint8_t presets_count(void)
{
	return (uint8_t)PRESET_COUNT;
}

bool presets_get(uint8_t index, Preset *out)
{
	PresetBlob blob;
#ifndef FDA_MODE
	if (index >= PRESET_COUNT || !slot_blob(index, &blob)) return false;
#elif defined(PRESET_TABLE)
	if (index >= PRESET_COUNT) return false;
	blob = preset_table[index][0];
#else
	if (index != 0U || !fda_blob(&blob)) return false;
#endif
	return parse_blob(blob.data, blob.len, out);
}

uint32_t presets_train_count(const Preset *preset, uint8_t i)
{
	return rd_u32(&preset->train_counts[4U * i]);
}

// CRC-32/ISO-HDLC, bit at a time: a blob is a few KB, so no lookup table.
static uint32_t crc32_bytes(uint32_t crc, const uint8_t *p, uint32_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return crc;
}

uint32_t presets_crc32(const uint8_t *data, uint32_t len)
{
	return ~crc32_bytes(0xFFFFFFFFu, data, len);
}

uint32_t presets_regs_crc(const Preset *preset)
{
	return presets_crc32(preset->body, preset->body_len);
}

// The walkers below follow lists parse_blob() already bounds-checked.

static const uint8_t *skip_runs(const uint8_t *p)
{
	uint16_t runs = rd_u16(p);
	p += 2;
	while (runs-- > 0U) {
		p += 4U + (4U * (uint32_t)rd_u16(&p[2]));
	}
	return p;
}

static const uint8_t *skip_pairs(const uint8_t *p)
{
	return p + 2U + (6U * (uint32_t)rd_u16(p));
}

// Write a chip's base registers, each run in address order.
static void write_runs(TX7332 *pT, uint8_t chip_index, const uint8_t *p)
{
	uint16_t runs = rd_u16(p);
	p += 2;
	while (runs-- > 0U) {
		uint16_t reg_address = rd_u16(&p[0]);
		uint16_t n = rd_u16(&p[2]);
		p += 4;
		for (uint16_t i = 0; i < n; i++, p += 4) {
			TX7332_WriteReg(pT, (uint16_t)(reg_address + i), rd_u32(p));
			profile_cache_note_write(chip_index, (uint16_t)(reg_address + i), 1);
		}
	}
}

// Write one profile's {address, value} pairs in order.
static void write_pairs(TX7332 *pT, uint8_t chip_index, const uint8_t *p)
{
	uint16_t count = rd_u16(p);
	p += 2;
	for (uint16_t i = 0; i < count; i++, p += 6) {
		uint16_t reg_address = rd_u16(&p[0]);
		TX7332_WriteReg(pT, reg_address, rd_u32(&p[2]));
		profile_cache_note_write(chip_index, reg_address, 1);
	}
}

// The apodization register among one profile's pairs, if it has one.
static bool find_apodization(const uint8_t *p, uint32_t *value)
{
	uint16_t count = rd_u16(p);
	p += 2;
	for (uint16_t i = 0; i < count; i++, p += 6) {
		if (rd_u16(&p[0]) == TX7332_APODIZATION_REGISTER) {
			*value = rd_u32(&p[2]);
			return true;
		}
	}
	return false;
}

bool presets_unload(void)
{
	// What a running sequence was loaded with stays: the thermal shutoff is
	// watching its limits. A paused one resumes on them.
	uint8_t status = get_trigger_status();
	if (status == TRIGGER_STATUS_RUNNING || status == TRIGGER_STATUS_PAUSED) return false;
	preset_loaded = false;
	return true;
}

bool presets_load(uint8_t index, uint32_t duration_index, uint32_t expected_regs_crc,
                  uint32_t expected_settings_crc)
{
	// Rewriting the delay and pattern registers underneath a running sequence
	// would change the output mid-burst. Otherwise any refusal, or a load that
	// fails part way, leaves nothing loaded, so the trigger cannot start on an
	// earlier load the host no longer matches. A timed-out sequence is ended
	// with STOP first, as the SDK does, so the load cannot hide the TIMEOUT.
	if (get_trigger_status() == TRIGGER_STATUS_TIMEOUT) return false;
	if (!presets_unload()) return false;

	Preset preset;
	if (!presets_get(index, &preset)) return false;
	if (duration_index >= preset.train_count_len) return false;

	// The host must hold the same blob this device does, made from the same
	// preset file, and the blob must still match what the generator produced.
	uint32_t crc = presets_regs_crc(&preset);
	if (crc != preset.regs_crc || crc != expected_regs_crc) return false;
	if (preset.settings_crc != expected_settings_crc) return false;

	uint8_t chips = preset.chip_count;
	if (chips > TX_PER_MODULE) chips = TX_PER_MODULE;

	auto_cycle_stop();

	// One pass over this module's chips to find each one's base runs and
	// starting profile. The trigger ISRs swap apodization as they cycle,
	// reading it out of apod_registers rather than the blob, so seed that
	// from every profile on the way.
	const uint8_t *base[TX_PER_MODULE] = { NULL };
	const uint8_t *start[TX_PER_MODULE] = { NULL };
	const uint8_t *p = preset.chips;
	TX7332_ResetApodizations();
	for (uint8_t c = 0; c < chips; c++) {
		base[c] = p;
		p = skip_runs(p);
		for (uint8_t prof = 0; prof < preset.profile_count; prof++) {
			uint32_t apod;
			if (find_apodization(p, &apod)) apod_registers[prof][c] = apod;
			if (prof == (uint8_t)(preset.profile_index - 1U)) start[c] = p;
			p = skip_pairs(p);
		}
	}

	// Base configuration, then the starting profile's registers on top.
	for (uint8_t c = 0; c < chips; c++) {
		write_runs(&transmitters[c], c, base[c]);
		write_pairs(&transmitters[c], c, start[c]);
		TX7332_LoadProfile(&transmitters[c]);
	}

	if (!profile_cycle_install(preset.profile_count, preset.exec_order, preset.exec_order_len)) {
		return false;
	}

	// The preset owns its sequence timing and its run-length choices: the
	// lock refuses SET_SWTRIG, and unlocked it unloads the preset (it would
	// also clear the profile cycle installed above).
	uint32_t trains = presets_train_count(&preset, (uint8_t)duration_index);
	if (!trigger_set_config(preset.trig_frequency_hz,
	                        PRESET_TRIGGER_PULSE_WIDTH_US,
	                        preset.trig_pulse_count,
	                        preset.trig_train_interval_us,
	                        trains,
	                        TRIGGER_MODE_SEQUENCE)) {
		return false;
	}

	trigger_set_timeout_s(preset.treatment_timeout_s);

	// Make the starting profile active so the first trigger uses it.
	for (uint8_t c = 0; c < chips; c++) {
		TX7332_SetActiveDelayProfile(preset.profile_index, &transmitters[c], c);
	}

	loaded_start_c_x10 = preset.start_c_x10;
	loaded_shutoff_c_x10 = preset.shutoff_c_x10;
	loaded.index = index;
	loaded.duration_index = (uint8_t)duration_index;
	loaded.train_count = trains;
	loaded.settings_crc = preset.settings_crc;
	loaded.regs_crc = crc;
	preset_loaded = true;
	return true;
}

bool presets_raw_write(bool here)
{
	// A raw write while nothing is loaded is the research case: let it be.
	if (!preset_loaded) return true;
	if (get_trigger_status() == TRIGGER_STATUS_RUNNING) return false;
	if (here) preset_loaded = false;
	return true;
}

bool presets_loaded(void)
{
	return preset_loaded;
}

bool presets_loaded_info(PresetLoaded *out)
{
	if (!preset_loaded) return false;
	*out = loaded;
	return true;
}

#ifndef FDA_MODE
bool presets_locked(void)
{
	return locked;
}

void presets_lock(void)
{
	locked = true;
}
#endif

static bool temperature_fresh(void)
{
	return tx_temperature_valid &&
	       (HAL_GetTick() - tx_temperature_tick) <= PRESET_TEMP_MAX_AGE_MS;
}

uint8_t presets_start_check(void)
{
	if (!preset_loaded) return OW_NO_PRESET;
	if (!temperature_fresh()) return OW_TEMP_UNKNOWN;
	if (tx_temperature * 10.0f > (float)loaded_start_c_x10) return OW_TEMP_TOO_HIGH;
	return OW_SUCCESS;
}

bool presets_thermal_trip(void)
{
	// Nothing loaded means nothing could have started.
	if (!preset_loaded) return false;
	if (!temperature_fresh()) return true;
	return tx_temperature * 10.0f >= (float)loaded_shutoff_c_x10;
}

// value / 10^decimals as "-1.5" (decimals 1) or "-1.52" (decimals 2): the
// image links newlib-nano's snprintf, which has no float support.
static int format_fixed(char *buf, size_t size, int32_t value, uint8_t decimals)
{
	uint32_t scale = (decimals == 2U) ? 100U : 10U;
	uint32_t mag = (uint32_t)((value < 0) ? -value : value);
	return snprintf(buf, size, (decimals == 2U) ? "%s%lu.%02lu" : "%s%lu.%lu", (value < 0) ? "-" : "",
	                (unsigned long)(mag / scale), (unsigned long)(mag % scale));
}

uint16_t presets_start_refusal(uint8_t code, char *buf, uint16_t size)
{
	char now[16], limit[16];
	int len;

	if (size == 0U) return 0U;
	switch (code) {
	case OW_NO_PRESET:
		len = snprintf(buf, size, "no preset loaded");
		break;
	case OW_TEMP_UNKNOWN:
		len = snprintf(buf, size, "no TX temperature reading in %lu ms",
		               (unsigned long)PRESET_TEMP_MAX_AGE_MS);
		break;
	case OW_TEMP_TOO_HIGH: {
		// Hundredths for the reading, so 35.04 does not print as "35.0 above 35.0".
		float t = tx_temperature * 100.0f;
		(void)format_fixed(now, sizeof(now), (int32_t)(t + ((t >= 0.0f) ? 0.5f : -0.5f)), 2U);
		(void)format_fixed(limit, sizeof(limit), preset_loaded ? loaded_start_c_x10 : 0, 1U);
		len = snprintf(buf, size, "TX %s C above start_C %s C", now, limit);
		break;
	}
	default:
		len = 0;
		break;
	}
	if (len < 0) return 0U;
	return (len >= (int)size) ? (uint16_t)(size - 1U) : (uint16_t)len;
}
