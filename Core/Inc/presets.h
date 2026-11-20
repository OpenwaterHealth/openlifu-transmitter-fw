/*
 * presets.h
 *
 * Presets the firmware -- not the host -- programs the TX7332 chips from. The
 * host picks one with OW_PRESET_LOAD.
 *
 * Every build keeps its presets in the preset slots (memory_map.h): pages
 * 106-125, 10 slots of 4 KB, slot N being preset index N. A slot holds one
 * signed preset: its PRS1 header (the SFU1 header layout, PRESET_HEADER_LEN
 * bytes: the blob's size and SHA-256, the diathermy commit and context it
 * came from, and a signature with the firmware's key), then the PRESET_BLOB.
 * The presets are not part of the image: the firmware is the same for every
 * preset set (one image per release).
 *
 *   FDA_MODE  the clinical image: it only reads the slots. The host may load a
 *             preset and change nothing else: not the registers, the trigger,
 *             the user config or the presets (preset_policy.h). The slots are
 *             written through the bootloader, which takes only a preset signed
 *             with its key (open-lifu-transmitter-bl dfu_preset.h).
 *   standard  every other build (PRESET_STORE): the host may also store and
 *             delete presets (OW_PRESET_WRITE / COMMIT / DELETE). The app checks
 *             a stored preset's integrity, not its signature: the host's own
 *             presets are a research use. It also keeps the registers and the
 *             trigger: a raw register, profile or trigger write leaves nothing
 *             loaded on the module that runs it, and is refused
 *             (OW_PRESET_BUSY) while a loaded preset runs or is paused.
 *
 * The slots exist only outside the bootloader's slot: a bare-metal image
 * always has them; an image under the bootloader has them only if the
 * bootloader's info block says so (BL_INFO_ADDRESS, open-lifu-transmitter-bl
 * >= 1.1.0-rc.1). Under an older bootloader every slot reads as empty and a
 * store is refused, since its slot check would refuse an image with anything
 * written there.
 *
 * Each module keeps its own load: a raw write to one module's chips unloads
 * that module alone, and OW_PRESET_LOADED then reports nothing loaded. Keeping
 * every module on the same preset is the host's job; configuring modules
 * differently is a research use of the low-level commands. A raw write the
 * handler then rejects has still unloaded: a preset counts as loaded only on
 * the happy path.
 *
 * A module programs only its own chips, so a preset spanning modules is one
 * blob per module, in that module's own slots, with its own regs_crc.
 *
 * OW_PRESET_LOAD requires both CRC-32s (IEEE, zlib-compatible) the host
 * computes, so host and device cannot disagree about what preset N runs:
 *   settings_crc  over the preset .json the blob was generated from
 *   regs_crc      over the blob's body, everything the firmware executes from
 *
 * A load also arms the preset's start_C, shutoff_C and treatment_timeout_s
 * (thermal_limits.h): the trigger will not start above start_C (or without a
 * fresh reading), a running sequence stops at shutoff_C, a paused one will
 * not resume at it (OW_TEMP_TOO_HIGH, as START above start_C), and a running
 * or paused one ends as TIMEOUT once treatment_timeout_s has passed since
 * START. The limits outlast the load: a
 * raw write, an unload or a timeout leaves them armed. With nothing loaded, an
 * FDA_MODE image's trigger does not start; a standard image's runs on the
 * host's own configuration, under whatever limits are armed.
 *
 * A stored upload is collected in RAM (SRAM2) and checked whole before its
 * slot is touched; the header's first doubleword (the magic) is programmed
 * last, so a write cut short leaves a slot that reads as empty.
 */

#ifndef INC_PRESETS_H_
#define INC_PRESETS_H_

#ifndef FDA_MODE
#define PRESET_STORE
#endif

#include "memory_map.h"
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
 *     u16 treatment_timeout_s  total time from START, pauses included, after
 *                            which the run ends as TIMEOUT (SR-024); >= 1,
 *                            so every preset has one (only the host's
 *                            SET_THERMAL_LIMITS may arm none)
 *     per chip:
 *       the base registers, as runs of consecutive addresses:
 *         u16 run_count, then per run: u16 start_addr, u16 n, u32 value[n]
 *       per profile, the registers each profile switch rewrites:
 *         u16 count, then per register: u16 addr, u32 value
 */
// 2 added treatment_timeout_s; a version 1 blob is refused, not defaulted.
#define PRESET_BLOB_VERSION 2u

// The slots are fixed in the flash map (memory_map.h PRESET_SLOT_*).
#define PRESET_SLOTS       PRESET_SLOT_COUNT           // memory_map.h: the slots are fixed
#define PRESET_COUNT       PRESET_SLOTS
// A slot: the PRS1 header, then the blob.
#define PRESET_HEADER_LEN  0x140u
#define PRESET_HEADER_MAGIC "PRS1"
#define PRESET_BLOB_MAX    (PRESET_SLOT_SIZE - PRESET_HEADER_LEN)

// Where one preset's blob is, and how long.
typedef struct {
	const uint8_t *data;
	uint32_t len;
} PresetBlob;

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
	// Treatment timeout, seconds from START (treatment_timeout_s).
	uint16_t treatment_timeout_s;
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

/** Does this image have the preset slots (see above)? */
bool presets_have_slots(void);

/**
 * @brief  The PRS1 header of the preset in a slot (PRESET_HEADER_LEN bytes),
 *         for the host to check its signature and provenance (OW_PRESET_INFO).
 * @return NULL if the slot holds no preset.
 */
const uint8_t *presets_header(uint8_t slot);

/**
 * @brief  What a preset index holds.
 * @return OW_SUCCESS if it holds a preset, else OW_PRESET_EMPTY, or
 *         OW_INVALID_PACKET past the last index.
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
 * A refusal leaves nothing loaded, except while a sequence is running or
 * paused, when the load in use is left as it is.
 *
 * @param duration_index        which of the preset's train_counts to run.
 * @param expected_regs_crc     the regs_crc the host computed for this preset.
 * @param expected_settings_crc the CRC-32 of the host's preset file.
 * @return true on success; false on a bad index, a duration index the
 *         preset does not offer, a CRC mismatch, or if a sequence is running or
 *         paused.
 */
bool presets_load(uint8_t index, uint32_t duration_index, uint32_t expected_regs_crc,
                  uint32_t expected_settings_crc);

/**
 * @brief  Leave nothing loaded, so an FDA_MODE trigger refuses to start.
 *
 * What the host asks for when the console did not take the same preset. The
 * thermal limits the load armed stay armed.
 *
 * @return false, changing nothing, if a sequence is running or paused.
 */
bool presets_unload(void);

/**
 * @brief  A standard image's host write is about to change the registers,
 *         profiles or trigger (preset_policy_raw_write()) of this module (here) or of
 *         another one it is forwarded to. Here, leave nothing loaded, since
 *         the loaded preset no longer describes the chips; elsewhere, that
 *         module unloads itself when it runs the write.
 * @return false, changing nothing, if a loaded preset is running or paused
 *         here: the write would change it mid-sequence and is refused. Only the master
 *         runs the trigger, so a module never refuses (#57). With nothing
 *         loaded, always true.
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

// A reading older than this counts as no reading: a sensor that stops
// answering must not leave a run going on its last value.
#define PRESET_TEMP_MAX_AGE_MS 3000u

/**
 * @brief  May the trigger start now, under the armed thermal limits and this
 *         module's reading (thermal_limits_start_check())?
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
 * @brief  May a sequence go on running, or resume, under the armed shutoff_C
 *         and this module's reading (thermal_limits_shutoff_check())?
 *         Whether or not a preset is still loaded.
 * @return OW_SUCCESS, or why not: OW_TEMP_UNKNOWN (the reading has gone
 *         stale), OW_TEMP_TOO_HIGH (at or above shutoff_C). The main loop
 *         stops a running sequence on either after each reading; a RESUME is
 *         refused with it in the OW_ERROR reply's reserved byte.
 */
uint8_t presets_shutoff_check(void);

#ifdef PRESET_STORE

/** Slots that hold nothing. */
uint8_t presets_free_slots(void);

/**
 * @brief  Take the next part of an upload for a slot: the slot's image, the
 *         PRS1 header then the blob. Offset 0 starts one; later parts must
 *         follow on from the last, for the same slot.
 * @return OW_SUCCESS, or OW_INVALID_PACKET, which also abandons the upload.
 */
uint8_t presets_stage(uint8_t slot, uint32_t offset, const uint8_t *data, uint16_t len);

/**
 * @brief  Store the uploaded slot image, replacing what was there.
 * Refused, with nothing changed, unless the upload is total_len bytes with
 * CRC-32 image_crc, starts with a PRS1 header whose size is the blob's, and
 * the blob is a well-formed PRESET_BLOB whose body matches its regs_crc. The
 * signature is not checked here (see above).
 * @return OW_SUCCESS, OW_INVALID_PACKET, OW_BAD_CRC, OW_PRESET_NO_ROOM (no
 *         preset slots, see above), OW_PRESET_BUSY, or OW_UNKNOWN_ERROR if the
 *         flash write failed (the slot is then empty).
 */
uint8_t presets_commit(uint8_t slot, uint32_t total_len, uint32_t image_crc);

/**
 * @brief  Empty a slot. Already empty is not an error.
 * @return OW_SUCCESS, OW_INVALID_PACKET, OW_PRESET_NO_ROOM (no preset slots),
 *         OW_PRESET_BUSY, or OW_UNKNOWN_ERROR if the erase failed.
 */
uint8_t presets_delete(uint8_t slot);

#endif /* PRESET_STORE */

#endif /* INC_PRESETS_H_ */
