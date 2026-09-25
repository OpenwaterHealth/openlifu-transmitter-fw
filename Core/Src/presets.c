#include "presets.h"

#ifdef FDA_MODE

#include "common.h"
#include "if_commands.h"
#include "trigger.h"
#include "tx7332.h"

#include PRESET_TABLE_HEADER

extern TX7332 transmitters[TX_PER_MODULE];

uint8_t presets_count(void)
{
	return (uint8_t)PRESET_COUNT;
}

const Preset *presets_get(uint8_t index)
{
	if (index >= PRESET_COUNT) return NULL;
	return &preset_table[index];
}

// CRC-32/ISO-HDLC, bit at a time: the tables are a few KB, so no lookup table.
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

static uint32_t crc32_u16(uint32_t crc, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
	return crc32_bytes(crc, b, 2);
}

static uint32_t crc32_u32(uint32_t crc, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	return crc32_bytes(crc, b, 4);
}

static uint32_t crc32_reg_list(uint32_t crc, const PresetRegList *list)
{
	crc = crc32_u16(crc, list->count);
	for (uint16_t i = 0; i < list->count; i++) {
		crc = crc32_u16(crc, (uint16_t)list->regs[i][0]);
		crc = crc32_u32(crc, list->regs[i][1]);
	}
	return crc;
}

static const PresetRegList *profile_list(const Preset *preset, uint8_t chip, uint8_t profile)
{
	return &preset->profile_regs[(chip * preset->profile_count) + profile];
}

// Walks PRESET_CRC_STREAM (presets.h); the SDK generator walks it the same way.
uint32_t presets_regs_crc(const Preset *preset)
{
	uint32_t crc = 0xFFFFFFFFu;
	uint8_t head[4] = { preset->chip_count, preset->profile_count,
	                    preset->profile_index, preset->exec_order_len };
	crc = crc32_bytes(crc, head, 4);
	crc = crc32_bytes(crc, preset->exec_order, preset->exec_order_len);
	crc = crc32_u32(crc, preset->trig_frequency_hz);
	crc = crc32_u32(crc, preset->trig_pulse_count);
	crc = crc32_u32(crc, preset->trig_train_interval_us);
	for (uint8_t c = 0; c < preset->chip_count; c++) {
		crc = crc32_reg_list(crc, &preset->base[c]);
		for (uint8_t p = 0; p < preset->profile_count; p++) {
			crc = crc32_reg_list(crc, profile_list(preset, c, p));
		}
	}
	return ~crc;
}

// Same shape as demo.c's write_demo_registers: walk the table, write each
// {address, value} pair in order.
static void write_reg_list(TX7332 *pT, uint8_t chip_index, const PresetRegList *list)
{
	for (uint16_t i = 0; i < list->count; i++) {
		uint16_t reg_address = (uint16_t)list->regs[i][0];
		uint32_t reg_value = list->regs[i][1];
		TX7332_WriteReg(pT, reg_address, reg_value);
		profile_cache_note_write(chip_index, reg_address, 1);
	}
}

bool presets_load(uint8_t index, uint32_t train_count, uint32_t expected_regs_crc)
{
	const Preset *preset = presets_get(index);
	if (preset == NULL) return false;

	// The host must hold the same tables this image does, and the image must
	// still match what the generator baked.
	uint32_t crc = presets_regs_crc(preset);
	if (crc != preset->regs_crc || crc != expected_regs_crc) return false;

	// Rewriting the delay and pattern registers underneath a running sequence
	// would change the output mid-burst.
	if (get_trigger_status() == TRIGGER_STATUS_RUNNING) return false;

	uint8_t chips = preset->chip_count;
	if (chips > TX_PER_MODULE) chips = TX_PER_MODULE;

	auto_cycle_stop();

	// The trigger ISRs swap apodization as they cycle, reading it out of this
	// table rather than the preset, so seed it from each profile's registers.
	TX7332_ResetApodizations();
	for (uint8_t p = 0; p < preset->profile_count && p < MAX_PROFILES; p++) {
		for (uint8_t c = 0; c < chips; c++) {
			const PresetRegList *list = profile_list(preset, c, p);
			for (uint16_t i = 0; i < list->count; i++) {
				if (list->regs[i][0] == TX7332_APODIZATION_REGISTER) {
					apod_registers[p][c] = list->regs[i][1];
				}
			}
		}
	}

	// Base configuration, then the starting profile's registers on top.
	for (uint8_t c = 0; c < chips; c++) {
		write_reg_list(&transmitters[c], c, &preset->base[c]);
		write_reg_list(&transmitters[c], c, profile_list(preset, c, preset->profile_index - 1u));
		TX7332_LoadProfile(&transmitters[c]);
	}

	if (!profile_cycle_install(preset->profile_count, preset->exec_order, preset->exec_order_len)) {
		return false;
	}

	// The preset owns its sequence timing, so the host never sends SET_SWTRIG
	// in FDA mode -- which matters because that clears the profile cycle.
	uint32_t trains = train_count;
	if (trains == 0U) trains = get_trigger_train_count();
	if (trains == 0U) trains = 1U;
	if (!trigger_set_config(preset->trig_frequency_hz,
	                        PRESET_TRIGGER_PULSE_WIDTH_US,
	                        preset->trig_pulse_count,
	                        preset->trig_train_interval_us,
	                        trains,
	                        TRIGGER_MODE_SEQUENCE)) {
		return false;
	}

	// Make the starting profile active so the first trigger uses it.
	for (uint8_t c = 0; c < chips; c++) {
		TX7332_SetActiveDelayProfile(preset->profile_index, &transmitters[c], c);
	}

	return true;
}

#endif /* FDA_MODE */
