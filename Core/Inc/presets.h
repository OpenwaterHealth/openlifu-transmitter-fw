/*
 * presets.h
 *
 * Presets the firmware -- not the host -- programs the TX7332 chips from. The
 * host picks one with OW_PRESET_LOAD.
 *
 *   FDA_MODE  one preset, linked into the image at the end of the
 *             application slot (pages 108-109, the FDA linker scripts'
 *             FDA_PRESET region), from Core/Inc/presets/fda_preset.h.
 *             Flashing or updating the image writes it; nothing in the image
 *             writes it, or the user config. Always locked (below).
 *   DEV       every other build: presets in pages 110-125, above the
 *             application slot: 16 slots of one page, slot N being preset
 *             index N. A blob longer than a page runs on into the next slot,
 *             which then has no preset of its own. The host may also write
 *             the registers and trigger itself, as a research build does.
 *
 * The lock (OW_PRESET_LOCK) puts a DEV image in the FDA image's place: the
 * host may load presets but not change them, the registers, the trigger, the
 * user config or the firmware (preset_policy.h). Only a reset unlocks it.
 * Unlocked, a raw register, profile or trigger write leaves nothing loaded.
 *
 * Only a bare-metal DEV image stores presets (PRESET_STORE). Under the
 * bootloader, pages 110-125 are the end of its slot, which it checks are
 * erased at every boot, so a stored preset would stop the image booting.
 *
 * A module programs only its own chips, so a preset spanning modules is one
 * blob per module, in that module's own flash, with its own regs_crc.
 *
 * OW_PRESET_LOAD requires both CRC-32s (IEEE, zlib-compatible) the host
 * computes, so host and device cannot disagree about what preset N runs:
 *   settings_crc  over the preset .json the blob was generated from
 *   regs_crc      over the blob's body, everything the firmware executes from
 *
 * Once a preset is loaded the trigger will not start above its start_C (or
 * without a fresh reading), and a running sequence stops at its shutoff_C.
 * With nothing loaded the trigger does not start.
 *
 * A stored upload is staged in RAM and written only once all of it has
 * arrived and checks out; its magic is programmed last, so a write cut short
 * leaves a slot that reads as empty.
 */

#ifndef INC_PRESETS_H_
#define INC_PRESETS_H_

// USER_VECT_TAB_ADDRESS is set for every image that runs under the bootloader.
#if !defined(FDA_MODE) && !defined(USER_VECT_TAB_ADDRESS)
#define PRESET_STORE
#endif

#include "tx7332.h"

#include <stdbool.h>
#include <stdint.h>

// Largest id a blob may carry, so the OW_PRESET_GET reply stays bounded.
#define PRESET_ID_MAX 64u
// Most run-length choices OW_PRESET_GET reports, for the same reason.
#define PRESET_TRAIN_SEL_MAX 16u

/*
 * PRESET_BLOB -- one preset. Little-endian, no padding, no pointers; the SDK's
 * preset_blob() writes it and presets.c walks it.
 *
 *   header
 *     u8  magic[4]           "OWPR"
 *     u8  version            PRESET_BLOB_VERSION
 *     u8  id_len             1..PRESET_ID_MAX
 *     u8  module             whose part of the preset this is
 *     u8  modules            modules the preset spans: 1 for a 1x, 2 for a 2x
 *                            (0 in a blob from before this was recorded)
 *     u32 total_len          the whole blob, header included
 *     u32 settings_crc
 *     u32 regs_crc           CRC-32 of the body, as generated
 *     u8  id[id_len]         ASCII, not NUL-terminated
 *   body -- the rest of the blob, and exactly what regs_crc covers
 *     u8  chip_count, u8 profile_count, u8 profile_index, u8 exec_order_len
 *     u8  exec_order[exec_order_len]
 *     u32 trig_frequency_hz, u32 trig_pulse_count, u32 trig_train_interval_us
 *     u8  train_count_len, u32 train_counts[train_count_len]
 *     i16 start_c_x10, i16 shutoff_c_x10
 *     per chip:
 *       the base registers, as runs of consecutive addresses:
 *         u16 run_count, then per run: u16 start_addr, u16 n, u32 value[n]
 *       per profile, the registers each profile switch rewrites:
 *         u16 count, then per register: u16 addr, u32 value
 */
#define PRESET_BLOB_VERSION 1u

#define PRESET_SLOTS       16u
#define PRESET_SLOT_SIZE   2048u                       // one flash page
#define PRESET_BLOB_MAX    (2u * PRESET_SLOT_SIZE)     // a blob may take two slots

// Preset indexes: FDA mode runs one preset, from slot 0; DEV has every slot.
#ifdef FDA_MODE
#define PRESET_COUNT 1u
#else
#define PRESET_COUNT PRESET_SLOTS
#endif

// A blob, parsed and bounds-checked by presets_get(). The pointers aim into
// the blob, where multi-byte fields sit unaligned: read train counts with
// presets_train_count().
typedef struct {
	const char *id;                    // id_len bytes, not NUL-terminated
	uint8_t id_len;
	uint8_t module;                    // whose part this is
	uint8_t modules;                   // modules the preset spans; 0 if not recorded
	uint8_t chip_count;
	uint8_t profile_count;
	uint8_t profile_index;             // profile active before the first trigger (1-based)
	uint8_t exec_order_len;
	const uint8_t *exec_order;
	// Trigger timing the preset defines.
	uint32_t trig_frequency_hz;
	uint32_t trig_pulse_count;
	uint32_t trig_train_interval_us;
	// Run lengths the operator may choose from, in pulse trains
	// (pulse_train_count_selections in the preset). OW_PRESET_LOAD picks one
	// by index; a raw count is never accepted.
	uint8_t train_count_len;
	const uint8_t *train_counts;
	// TX thermistor limits, tenths of a degree C (start_C / shutoff_C).
	int16_t start_c_x10;
	int16_t shutoff_c_x10;
	uint32_t settings_crc;             // CRC-32 of the source .json bytes
	uint32_t regs_crc;                 // CRC-32 of the body, as generated
	const uint8_t *body;               // what regs_crc covers
	uint32_t body_len;
	const uint8_t *chips;              // the first chip's register lists
} Preset;

// Width of the trigger line marker pulse. Not the acoustic burst -- that
// length is already in the pattern registers the preset carries.
#define PRESET_TRIGGER_PULSE_WIDTH_US 20u

/** Number of preset indexes, PRESET_COUNT. */
uint8_t presets_count(void);

/**
 * @brief  What a preset index holds.
 * @return OW_SUCCESS if it heads a blob, else OW_PRESET_EMPTY,
 *         OW_PRESET_SLOT_TAIL, or OW_INVALID_PACKET past the last index.
 */
uint8_t presets_slot_state(uint8_t slot);

/**
 * @brief  Parse the preset at index.
 * @return false if index is out of range, holds no preset, or its blob is not
 *         a well-formed PRESET_BLOB; *out is only meaningful on true.
 */
bool presets_get(uint8_t index, Preset *out);

/** CRC-32 (IEEE) of a byte range: the one every preset CRC is. */
uint32_t presets_crc32(const uint8_t *data, uint32_t len);

/** The i-th run length a preset offers, in pulse trains (i < train_count_len). */
uint32_t presets_train_count(const Preset *preset, uint8_t i);

/**
 * @brief  CRC-32 over a preset's body, computed from its stored bytes now.
 *
 * Equal to preset->regs_crc unless the blob no longer matches what the
 * generator produced; callers treat a mismatch as a refused preset.
 */
uint32_t presets_regs_crc(const Preset *preset);

/**
 * @brief  Program this module's TX7332 chips from a preset.
 *
 * Refused unless expected_regs_crc equals the CRC computed from the blob and
 * expected_settings_crc equals the blob's settings_crc, so a host that holds
 * a different idea of preset N -- its registers, or anything else in its
 * source file -- cannot run it. Writes the base registers, installs the
 * execution order so the trigger ISRs can cycle profiles, applies the
 * preset's trigger timing, and activates its starting profile.
 *
 * A refusal leaves nothing loaded, except while a trigger is running, when
 * the load in use is left as it is.
 *
 * @param duration_index        which of the preset's train_counts to run.
 * @param expected_regs_crc     the regs_crc the host computed for this preset.
 * @param expected_settings_crc the CRC-32 of the host's preset file.
 * @return true on success; false on a bad index, a duration index the
 *         preset does not offer, a CRC mismatch, or if a trigger is running.
 */
bool presets_load(uint8_t index, uint32_t duration_index, uint32_t expected_regs_crc,
                  uint32_t expected_settings_crc);

/**
 * @brief  Leave nothing loaded, so the trigger refuses to start.
 *
 * What the host asks for when the console did not take the same preset.
 *
 * @return false, changing nothing, if a trigger is running.
 */
bool presets_unload(void);

/**
 * @brief  An unlocked host write is about to change the registers, profiles
 *         or trigger (preset_policy_raw_write()) of this module (here) or of
 *         another one it is forwarded to. Here, leave nothing loaded, since
 *         the loaded preset no longer describes the chips; elsewhere, that
 *         module unloads itself when it runs the write.
 * @return false, changing nothing, if a loaded preset is running: the write,
 *         wherever it goes, would change it mid-burst and is refused. With
 *         nothing loaded, always true.
 */
bool presets_raw_write(bool here);

/** Is a preset loaded on this module? */
bool presets_loaded(void);

// What this module has loaded (OW_PRESET_LOADED).
typedef struct {
	uint8_t index;
	uint8_t duration_index;            // the run-length choice the load made
	uint32_t train_count;              // and the pulse-train count it resolved to
	uint32_t settings_crc;
	uint32_t regs_crc;                 // this module's own
} PresetLoaded;

/** @return false, leaving *out alone, if nothing is loaded. */
bool presets_loaded_info(PresetLoaded *out);

#ifdef FDA_MODE
static inline bool presets_locked(void) { return true; }
static inline void presets_lock(void) { }
#else
/** Is the lock on? Always, in FDA_MODE. */
bool presets_locked(void);
/** Turn the lock on, until the next reset. */
void presets_lock(void);
#endif

// A reading older than this counts as no reading: a sensor that stops
// answering must not leave a run going on its last value.
#define PRESET_TEMP_MAX_AGE_MS 3000u

/**
 * @brief  May the trigger start now?
 * @return OW_SUCCESS, or why not (OW_NO_PRESET, OW_TEMP_UNKNOWN,
 *         OW_TEMP_TOO_HIGH), for the OW_ERROR reply's reserved byte.
 */
uint8_t presets_start_check(void);

/**
 * @brief  Explain a presets_start_check() refusal for the OW_ERROR payload.
 * @return Length of the NUL-free text written to buf.
 */
uint16_t presets_start_refusal(uint8_t code, char *buf, uint16_t size);

/**
 * @brief  Must a running sequence stop? True at or above the loaded preset's
 *         shutoff_C, or when the reading has gone stale. Polled by the main
 *         loop after each temperature reading.
 */
bool presets_thermal_trip(void);

#ifdef PRESET_STORE

/** Slots that hold nothing: neither a preset nor the second half of one. */
uint8_t presets_free_slots(void);

/**
 * @brief  Take the next part of an upload for a slot. Offset 0 starts one;
 *         later parts must follow on from the last, for the same slot.
 * @return OW_SUCCESS, or OW_INVALID_PACKET, which also abandons the upload.
 */
uint8_t presets_stage(uint8_t slot, uint32_t offset, const uint8_t *data, uint16_t len);

/**
 * @brief  Store the uploaded blob in its slot, replacing what was there.
 *
 * Refused, with nothing changed, unless the upload is total_len bytes with
 * CRC-32 blob_crc, is a well-formed PRESET_BLOB whose body matches its
 * regs_crc, and fits: a blob longer than one slot needs the next slot to be
 * free or already its own.
 *
 * @return OW_SUCCESS, OW_INVALID_PACKET, OW_BAD_CRC, OW_PRESET_SLOT_TAIL,
 *         OW_PRESET_NO_ROOM, OW_PRESET_BUSY, or OW_UNKNOWN_ERROR if the flash
 *         write failed (the slot is then empty).
 */
uint8_t presets_commit(uint8_t slot, uint32_t total_len, uint32_t blob_crc);

/**
 * @brief  Empty a slot. Already empty is not an error.
 * @return OW_SUCCESS, OW_INVALID_PACKET, OW_PRESET_SLOT_TAIL, OW_PRESET_BUSY,
 *         or OW_UNKNOWN_ERROR if the erase failed.
 */
uint8_t presets_delete(uint8_t slot);

#endif /* PRESET_STORE */

#endif /* INC_PRESETS_H_ */
