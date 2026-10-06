#include "presets.h"

#ifdef FDA_MODE

#include "common.h"
#include "if_commands.h"
#include "thermistor.h"
#include "trigger.h"
#include "tx7332.h"

#include <stdio.h>
#include <string.h>

#include "presets/preset_table.h"

extern TX7332 transmitters[TX_PER_MODULE];

// Thermal limits of the preset the chips were last programmed from; they
// govern every start until another load replaces them. Kept by value so they
// never depend on the blob staying where it was.
static bool preset_loaded = false;
static int16_t loaded_start_c_x10 = 0;
static int16_t loaded_shutoff_c_x10 = 0;

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

	if (!take(&c, 4U, &f)) return false;
	out->start_c_x10 = (int16_t)rd_u16(&f[0]);
	out->shutoff_c_x10 = (int16_t)rd_u16(&f[2]);

	out->chips = c.p;
	for (uint8_t chip = 0; chip < out->chip_count; chip++) {
		if (!check_runs(&c)) return false;
		for (uint8_t p = 0; p < out->profile_count; p++) {
			if (!check_pairs(&c)) return false;
		}
	}
	return c.p == c.end;
}

uint8_t presets_count(void)
{
	return (uint8_t)PRESET_COUNT;
}

bool presets_get(uint8_t index, Preset *out)
{
	if (index >= PRESET_COUNT) return false;
	return parse_blob(preset_table[index].data, preset_table[index].len, out);
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

uint32_t presets_regs_crc(const Preset *preset)
{
	return ~crc32_bytes(0xFFFFFFFFu, preset->body, preset->body_len);
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

bool presets_load(uint8_t index, uint32_t duration_index, uint32_t expected_regs_crc)
{
	Preset preset;
	if (!presets_get(index, &preset)) return false;
	if (duration_index >= preset.train_count_len) return false;

	// The host must hold the same blob this image does, and the blob must
	// still match what the generator produced.
	uint32_t crc = presets_regs_crc(&preset);
	if (crc != preset.regs_crc || crc != expected_regs_crc) return false;

	// Rewriting the delay and pattern registers underneath a running sequence
	// would change the output mid-burst.
	if (get_trigger_status() == TRIGGER_STATUS_RUNNING) return false;

	uint8_t chips = preset.chip_count;
	if (chips > TX_PER_MODULE) chips = TX_PER_MODULE;

	// From here the chips stop holding the previous preset, so a load that
	// fails part way leaves nothing loaded and the trigger refuses to start.
	preset_loaded = false;
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

	// The preset owns its sequence timing and its run-length choices, so the
	// host never sends SET_SWTRIG in FDA mode -- which matters because that
	// clears the profile cycle.
	uint32_t trains = presets_train_count(&preset, (uint8_t)duration_index);
	if (!trigger_set_config(preset.trig_frequency_hz,
	                        PRESET_TRIGGER_PULSE_WIDTH_US,
	                        preset.trig_pulse_count,
	                        preset.trig_train_interval_us,
	                        trains,
	                        TRIGGER_MODE_SEQUENCE)) {
		return false;
	}

	// Make the starting profile active so the first trigger uses it.
	for (uint8_t c = 0; c < chips; c++) {
		TX7332_SetActiveDelayProfile(preset.profile_index, &transmitters[c], c);
	}

	loaded_start_c_x10 = preset.start_c_x10;
	loaded_shutoff_c_x10 = preset.shutoff_c_x10;
	preset_loaded = true;
	return true;
}

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

#endif /* FDA_MODE */
