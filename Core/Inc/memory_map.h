/*
 * memory_map.h
 *
 *  Created on: Mar 4, 2024
 *      Author: gvigelet
 */

#ifndef INC_MEMORY_MAP_H_
#define INC_MEMORY_MAP_H_

#include <stdint.h>

#ifdef __cplusplus
 extern "C" {
#endif
 /* Base address of the Flash pages */
 #define ADDR_FLASH_PAGE_0     ((uint32_t)0x08000000) /* Base @ of Page 0, 2 Kbytes */
 #define ADDR_FLASH_PAGE_1     ((uint32_t)0x08000800) /* Base @ of Page 1, 2 Kbytes */
 #define ADDR_FLASH_PAGE_2     ((uint32_t)0x08001000) /* Base @ of Page 2, 2 Kbytes */
 #define ADDR_FLASH_PAGE_3     ((uint32_t)0x08001800) /* Base @ of Page 3, 2 Kbytes */
 #define ADDR_FLASH_PAGE_4     ((uint32_t)0x08002000) /* Base @ of Page 4, 2 Kbytes */
 #define ADDR_FLASH_PAGE_5     ((uint32_t)0x08002800) /* Base @ of Page 5, 2 Kbytes */
 #define ADDR_FLASH_PAGE_6     ((uint32_t)0x08003000) /* Base @ of Page 6, 2 Kbytes */
 #define ADDR_FLASH_PAGE_7     ((uint32_t)0x08003800) /* Base @ of Page 7, 2 Kbytes */
 #define ADDR_FLASH_PAGE_8     ((uint32_t)0x08004000) /* Base @ of Page 8, 2 Kbytes */
 #define ADDR_FLASH_PAGE_9     ((uint32_t)0x08004800) /* Base @ of Page 9, 2 Kbytes */
 #define ADDR_FLASH_PAGE_10    ((uint32_t)0x08005000) /* Base @ of Page 10, 2 Kbytes */
 #define ADDR_FLASH_PAGE_11    ((uint32_t)0x08005800) /* Base @ of Page 11, 2 Kbytes */
 #define ADDR_FLASH_PAGE_12    ((uint32_t)0x08006000) /* Base @ of Page 12, 2 Kbytes */
 #define ADDR_FLASH_PAGE_13    ((uint32_t)0x08006800) /* Base @ of Page 13, 2 Kbytes */
 #define ADDR_FLASH_PAGE_14    ((uint32_t)0x08007000) /* Base @ of Page 14, 2 Kbytes */
 #define ADDR_FLASH_PAGE_15    ((uint32_t)0x08007800) /* Base @ of Page 15, 2 Kbytes */
 #define ADDR_FLASH_PAGE_16    ((uint32_t)0x08008000) /* Base @ of Page 16, 2 Kbytes */
 #define ADDR_FLASH_PAGE_17    ((uint32_t)0x08008800) /* Base @ of Page 17, 2 Kbytes */
 #define ADDR_FLASH_PAGE_18    ((uint32_t)0x08009000) /* Base @ of Page 18, 2 Kbytes */
 #define ADDR_FLASH_PAGE_19    ((uint32_t)0x08009800) /* Base @ of Page 19, 2 Kbytes */
 #define ADDR_FLASH_PAGE_20    ((uint32_t)0x0800A000) /* Base @ of Page 20, 2 Kbytes */
 #define ADDR_FLASH_PAGE_21    ((uint32_t)0x0800A800) /* Base @ of Page 21, 2 Kbytes  */
 #define ADDR_FLASH_PAGE_22    ((uint32_t)0x0800B000) /* Base @ of Page 22, 2 Kbytes  */
 #define ADDR_FLASH_PAGE_23    ((uint32_t)0x0800B800) /* Base @ of Page 23, 2 Kbytes */
 #define ADDR_FLASH_PAGE_24    ((uint32_t)0x0800C000) /* Base @ of Page 24, 2 Kbytes */
 #define ADDR_FLASH_PAGE_25    ((uint32_t)0x0800C800) /* Base @ of Page 25, 2 Kbytes */
 #define ADDR_FLASH_PAGE_26    ((uint32_t)0x0800D000) /* Base @ of Page 26, 2 Kbytes */
 #define ADDR_FLASH_PAGE_27    ((uint32_t)0x0800D800) /* Base @ of Page 27, 2 Kbytes */
 #define ADDR_FLASH_PAGE_28    ((uint32_t)0x0800E000) /* Base @ of Page 28, 2 Kbytes */
 #define ADDR_FLASH_PAGE_29    ((uint32_t)0x0800E800) /* Base @ of Page 29, 2 Kbytes */
 #define ADDR_FLASH_PAGE_30    ((uint32_t)0x0800F000) /* Base @ of Page 30, 2 Kbytes */
 #define ADDR_FLASH_PAGE_31    ((uint32_t)0x0800F800) /* Base @ of Page 31, 2 Kbytes */
 #define ADDR_FLASH_PAGE_32    ((uint32_t)0x08010000) /* Base @ of Page 32, 2 Kbytes */
 #define ADDR_FLASH_PAGE_33    ((uint32_t)0x08010800) /* Base @ of Page 33, 2 Kbytes */
 #define ADDR_FLASH_PAGE_34    ((uint32_t)0x08011000) /* Base @ of Page 34, 2 Kbytes */
 #define ADDR_FLASH_PAGE_35    ((uint32_t)0x08011800) /* Base @ of Page 35, 2 Kbytes */
 #define ADDR_FLASH_PAGE_36    ((uint32_t)0x08012000) /* Base @ of Page 36, 2 Kbytes */
 #define ADDR_FLASH_PAGE_37    ((uint32_t)0x08012800) /* Base @ of Page 37, 2 Kbytes */
 #define ADDR_FLASH_PAGE_38    ((uint32_t)0x08013000) /* Base @ of Page 38, 2 Kbytes */
 #define ADDR_FLASH_PAGE_39    ((uint32_t)0x08013800) /* Base @ of Page 39, 2 Kbytes */
 #define ADDR_FLASH_PAGE_40    ((uint32_t)0x08014000) /* Base @ of Page 40, 2 Kbytes */
 #define ADDR_FLASH_PAGE_41    ((uint32_t)0x08014800) /* Base @ of Page 41, 2 Kbytes */
 #define ADDR_FLASH_PAGE_42    ((uint32_t)0x08015000) /* Base @ of Page 42, 2 Kbytes */
 #define ADDR_FLASH_PAGE_43    ((uint32_t)0x08015800) /* Base @ of Page 43, 2 Kbytes */
 #define ADDR_FLASH_PAGE_44    ((uint32_t)0x08016000) /* Base @ of Page 44, 2 Kbytes */
 #define ADDR_FLASH_PAGE_45    ((uint32_t)0x08016800) /* Base @ of Page 45, 2 Kbytes */
 #define ADDR_FLASH_PAGE_46    ((uint32_t)0x08017000) /* Base @ of Page 46, 2 Kbytes */
 #define ADDR_FLASH_PAGE_47    ((uint32_t)0x08017800) /* Base @ of Page 47, 2 Kbytes */
 #define ADDR_FLASH_PAGE_48    ((uint32_t)0x08018000) /* Base @ of Page 48, 2 Kbytes */
 #define ADDR_FLASH_PAGE_49    ((uint32_t)0x08018800) /* Base @ of Page 49, 2 Kbytes */
 #define ADDR_FLASH_PAGE_50    ((uint32_t)0x08019000) /* Base @ of Page 50, 2 Kbytes */
 #define ADDR_FLASH_PAGE_51    ((uint32_t)0x08019800) /* Base @ of Page 51, 2 Kbytes */
 #define ADDR_FLASH_PAGE_52    ((uint32_t)0x0801A000) /* Base @ of Page 52, 2 Kbytes */
 #define ADDR_FLASH_PAGE_53    ((uint32_t)0x0801A800) /* Base @ of Page 53, 2 Kbytes */
 #define ADDR_FLASH_PAGE_54    ((uint32_t)0x0801B000) /* Base @ of Page 54, 2 Kbytes */
 #define ADDR_FLASH_PAGE_55    ((uint32_t)0x0801B800) /* Base @ of Page 55, 2 Kbytes */
 #define ADDR_FLASH_PAGE_56    ((uint32_t)0x0801C000) /* Base @ of Page 56, 2 Kbytes */
 #define ADDR_FLASH_PAGE_57    ((uint32_t)0x0801C800) /* Base @ of Page 57, 2 Kbytes */
 #define ADDR_FLASH_PAGE_58    ((uint32_t)0x0801D000) /* Base @ of Page 58, 2 Kbytes */
 #define ADDR_FLASH_PAGE_59    ((uint32_t)0x0801D800) /* Base @ of Page 59, 2 Kbytes */
 #define ADDR_FLASH_PAGE_60    ((uint32_t)0x0801E000) /* Base @ of Page 60, 2 Kbytes */
 #define ADDR_FLASH_PAGE_61    ((uint32_t)0x0801E800) /* Base @ of Page 61, 2 Kbytes */
 #define ADDR_FLASH_PAGE_62    ((uint32_t)0x0801F000) /* Base @ of Page 62, 2 Kbytes */
 #define ADDR_FLASH_PAGE_63    ((uint32_t)0x0801F800) /* Base @ of Page 63, 2 Kbytes */
 #define ADDR_FLASH_PAGE_64    ((uint32_t)0x08020000) /* Base @ of Page 64, 2 Kbytes */
 #define ADDR_FLASH_PAGE_65    ((uint32_t)0x08020800) /* Base @ of Page 65, 2 Kbytes */
 #define ADDR_FLASH_PAGE_66    ((uint32_t)0x08021000) /* Base @ of Page 66, 2 Kbytes */
 #define ADDR_FLASH_PAGE_67    ((uint32_t)0x08021800) /* Base @ of Page 67, 2 Kbytes */
 #define ADDR_FLASH_PAGE_68    ((uint32_t)0x08022000) /* Base @ of Page 68, 2 Kbytes */
 #define ADDR_FLASH_PAGE_69    ((uint32_t)0x08022800) /* Base @ of Page 69, 2 Kbytes */
 #define ADDR_FLASH_PAGE_70    ((uint32_t)0x08023000) /* Base @ of Page 70, 2 Kbytes */
 #define ADDR_FLASH_PAGE_71    ((uint32_t)0x08023800) /* Base @ of Page 71, 2 Kbytes */
 #define ADDR_FLASH_PAGE_72    ((uint32_t)0x08024000) /* Base @ of Page 72, 2 Kbytes */
 #define ADDR_FLASH_PAGE_73    ((uint32_t)0x08024800) /* Base @ of Page 73, 2 Kbytes */
 #define ADDR_FLASH_PAGE_74    ((uint32_t)0x08025000) /* Base @ of Page 74, 2 Kbytes */
 #define ADDR_FLASH_PAGE_75    ((uint32_t)0x08025800) /* Base @ of Page 75, 2 Kbytes */
 #define ADDR_FLASH_PAGE_76    ((uint32_t)0x08026000) /* Base @ of Page 76, 2 Kbytes */
 #define ADDR_FLASH_PAGE_77    ((uint32_t)0x08026800) /* Base @ of Page 77, 2 Kbytes */
 #define ADDR_FLASH_PAGE_78    ((uint32_t)0x08027000) /* Base @ of Page 78, 2 Kbytes */
 #define ADDR_FLASH_PAGE_79    ((uint32_t)0x08027800) /* Base @ of Page 79, 2 Kbytes */
 #define ADDR_FLASH_PAGE_80    ((uint32_t)0x08028000) /* Base @ of Page 80, 2 Kbytes */
 #define ADDR_FLASH_PAGE_81    ((uint32_t)0x08028800) /* Base @ of Page 81, 2 Kbytes */
 #define ADDR_FLASH_PAGE_82    ((uint32_t)0x08029000) /* Base @ of Page 82, 2 Kbytes */
 #define ADDR_FLASH_PAGE_83    ((uint32_t)0x08029800) /* Base @ of Page 83, 2 Kbytes */
 #define ADDR_FLASH_PAGE_84    ((uint32_t)0x0802A000) /* Base @ of Page 84, 2 Kbytes */
 #define ADDR_FLASH_PAGE_85    ((uint32_t)0x0802A800) /* Base @ of Page 85, 2 Kbytes */
 #define ADDR_FLASH_PAGE_86    ((uint32_t)0x0802B000) /* Base @ of Page 86, 2 Kbytes */
 #define ADDR_FLASH_PAGE_87    ((uint32_t)0x0802B800) /* Base @ of Page 87, 2 Kbytes */
 #define ADDR_FLASH_PAGE_88    ((uint32_t)0x0802C000) /* Base @ of Page 88, 2 Kbytes */
 #define ADDR_FLASH_PAGE_89    ((uint32_t)0x0802C800) /* Base @ of Page 89, 2 Kbytes */
 #define ADDR_FLASH_PAGE_90    ((uint32_t)0x0802D000) /* Base @ of Page 90, 2 Kbytes */
 #define ADDR_FLASH_PAGE_91    ((uint32_t)0x0802D800) /* Base @ of Page 91, 2 Kbytes */
 #define ADDR_FLASH_PAGE_92    ((uint32_t)0x0802E000) /* Base @ of Page 92, 2 Kbytes */
 #define ADDR_FLASH_PAGE_93    ((uint32_t)0x0802E800) /* Base @ of Page 93, 2 Kbytes */
 #define ADDR_FLASH_PAGE_94    ((uint32_t)0x0802F000) /* Base @ of Page 94, 2 Kbytes */
 #define ADDR_FLASH_PAGE_95    ((uint32_t)0x0802F800) /* Base @ of Page 95, 2 Kbytes */
 #define ADDR_FLASH_PAGE_96    ((uint32_t)0x08030000) /* Base @ of Page 96, 2 Kbytes */
 #define ADDR_FLASH_PAGE_97    ((uint32_t)0x08030800) /* Base @ of Page 97, 2 Kbytes */
 #define ADDR_FLASH_PAGE_98    ((uint32_t)0x08031000) /* Base @ of Page 98, 2 Kbytes */
 #define ADDR_FLASH_PAGE_99    ((uint32_t)0x08031800) /* Base @ of Page 99, 2 Kbytes */
 #define ADDR_FLASH_PAGE_100   ((uint32_t)0x08032000) /* Base @ of Page 100, 2 Kbytes */
 #define ADDR_FLASH_PAGE_101   ((uint32_t)0x08032800) /* Base @ of Page 101, 2 Kbytes */
 #define ADDR_FLASH_PAGE_102   ((uint32_t)0x08033000) /* Base @ of Page 102, 2 Kbytes */
 #define ADDR_FLASH_PAGE_103   ((uint32_t)0x08033800) /* Base @ of Page 103, 2 Kbytes */
 #define ADDR_FLASH_PAGE_104   ((uint32_t)0x08034000) /* Base @ of Page 104, 2 Kbytes */
 #define ADDR_FLASH_PAGE_105   ((uint32_t)0x08034800) /* Base @ of Page 105, 2 Kbytes */
 #define ADDR_FLASH_PAGE_106   ((uint32_t)0x08035000) /* Base @ of Page 106, 2 Kbytes */
 #define ADDR_FLASH_PAGE_107   ((uint32_t)0x08035800) /* Base @ of Page 107, 2 Kbytes */
 #define ADDR_FLASH_PAGE_108   ((uint32_t)0x08036000) /* Base @ of Page 108, 2 Kbytes */
 #define ADDR_FLASH_PAGE_109   ((uint32_t)0x08036800) /* Base @ of Page 109, 2 Kbytes */
 #define ADDR_FLASH_PAGE_110   ((uint32_t)0x08037000) /* Base @ of Page 110, 2 Kbytes */
 #define ADDR_FLASH_PAGE_111   ((uint32_t)0x08037800) /* Base @ of Page 111, 2 Kbytes */
 #define ADDR_FLASH_PAGE_112   ((uint32_t)0x08038000) /* Base @ of Page 112, 2 Kbytes */
 #define ADDR_FLASH_PAGE_113   ((uint32_t)0x08038800) /* Base @ of Page 113, 2 Kbytes */
 #define ADDR_FLASH_PAGE_114   ((uint32_t)0x08039000) /* Base @ of Page 114, 2 Kbytes */
 #define ADDR_FLASH_PAGE_115   ((uint32_t)0x08039800) /* Base @ of Page 115, 2 Kbytes */
 #define ADDR_FLASH_PAGE_116   ((uint32_t)0x0803A000) /* Base @ of Page 116, 2 Kbytes */
 #define ADDR_FLASH_PAGE_117   ((uint32_t)0x0803A800) /* Base @ of Page 117, 2 Kbytes */
 #define ADDR_FLASH_PAGE_118   ((uint32_t)0x0803B000) /* Base @ of Page 118, 2 Kbytes */
 #define ADDR_FLASH_PAGE_119   ((uint32_t)0x0803B800) /* Base @ of Page 119, 2 Kbytes */
 #define ADDR_FLASH_PAGE_120   ((uint32_t)0x0803C000) /* Base @ of Page 120, 2 Kbytes */
 #define ADDR_FLASH_PAGE_121   ((uint32_t)0x0803C800) /* Base @ of Page 121, 2 Kbytes */
 #define ADDR_FLASH_PAGE_122   ((uint32_t)0x0803D000) /* Base @ of Page 122, 2 Kbytes */
 #define ADDR_FLASH_PAGE_123   ((uint32_t)0x0803D800) /* Base @ of Page 123, 2 Kbytes */
 #define ADDR_FLASH_PAGE_124   ((uint32_t)0x0803E000) /* Base @ of Page 124, 2 Kbytes */
 #define ADDR_FLASH_PAGE_125   ((uint32_t)0x0803E800) /* Base @ of Page 125, 2 Kbytes */
 #define ADDR_FLASH_PAGE_126   ((uint32_t)0x0803F000) /* Base @ of Page 126, 2 Kbytes */
 #define ADDR_FLASH_PAGE_127   ((uint32_t)0x0803F800) /* Base @ of Page 127, 2 Kbytes */
 #define ADDR_FLASH_END_ADDRESS ((uint32_t)0x08040000) /* END OF FLASH */

/*
 * Flash layout (2 KB pages). The same in the bootloader's memory_map.h
 * (open-lifu-transmitter-bl): change both together.
 *
 *   pages   0- 31  0x08000000  secure bootloader (64 KB)
 *   pages  32-105  0x08010000  application slot (148 KB): signed header, then
 *                              the image at +0x400
 *   pages 106-125  0x08035000  presets (40 KB, presets.h): 10 slots of 4 KB
 *                              (two pages), preset N at 0x08035000 + N x 0x1000
 *   page      126  0x0803F000  the bootloader's anti-rollback version log
 *   page      127  0x0803F800  runtime config (lifu_config.h)
 *
 * The presets and the runtime config sit outside the slot: the bootloader
 * erases an image whose slot holds anything past the image, and DFU must not
 * reach them. Bootloaders before 1.1.0-rc.1 still check pages 32-125, so until a
 * unit has one, only a bare-metal image stores presets (PRESET_STORE,
 * presets.h).
 */
#define FLASH_START_ADDRESS                 ADDR_FLASH_PAGE_0
#define FLASH_END_ADDRESS                   ADDR_FLASH_END_ADDRESS
#define BOOTLOADER_ADDRESS                  ADDR_FLASH_PAGE_0
#define APPLICATION_ADDRESS                 ADDR_FLASH_PAGE_32 // slot start: signed header
#define APPLICATION_IMAGE_OFFSET            ((uint32_t)0x400)  // the image runs at the slot start + 0x400
#define APPLICATION_SLOT_SIZE               ((uint32_t)(ADDR_FLASH_PAGE_106 - ADDR_FLASH_PAGE_32))
#define APPLICATION_MAX_SIZE                (APPLICATION_SLOT_SIZE - APPLICATION_IMAGE_OFFSET)
#define PRESET_STORE_ADDRESS                ADDR_FLASH_PAGE_106
#define PRESET_STORE_SIZE                   ((uint32_t)(ADDR_FLASH_PAGE_126 - ADDR_FLASH_PAGE_106))
#define PRESET_SLOT_SIZE                    ((uint32_t)(ADDR_FLASH_PAGE_108 - ADDR_FLASH_PAGE_106)) // 4 KB
#define PRESET_SLOT_COUNT                   10u
#define PRESET_SLOT_ADDRESS(n)              (PRESET_STORE_ADDRESS + ((uint32_t)(n) * PRESET_SLOT_SIZE))
// Anti-rollback version log: the secure bootloader's (its MEM_ANTIROLLBACK_BASE).
// The application must never write or erase it.
#define BOOTLOADER_ANTIROLLBACK_ADDRESS     ADDR_FLASH_PAGE_126
#define BOOTLOADER_ANTIROLLBACK_SIZE        ((uint32_t)(ADDR_FLASH_PAGE_127 - ADDR_FLASH_PAGE_126))
// Runtime config (lifu_config.h): the application's own, never written by DFU.
#define APPLICATION_RUNTIME_CONFIG          ADDR_FLASH_PAGE_127
#define APPLICATION_RUNTIME_CONFIG_SIZE     ((uint32_t)(ADDR_FLASH_END_ADDRESS - ADDR_FLASH_PAGE_127))

/* The bootloader info block (open-lifu-transmitter-bl bl_info.c, >= 1.1.0-rc.1):
 * 32 bytes at a fixed address at the end of its vector area, the flash layout
 * and DFU capabilities it was built with, read as words:
 *   [0] magic  [1] layout (low half)  [2] slot base  [3] slot end
 *   [4] preset base  [5] preset size  [6] capabilities  [7] reserved
 * Older bootloaders have only fill bytes here, so the magic tells them apart. */
#define BL_INFO_ADDRESS                     ((uint32_t)0x080001E0)
#define BL_INFO_MAGIC                       ((uint32_t)0x4942574F) // 'OWBI'
#define BL_INFO_WORD_MAGIC                  0u
#define BL_INFO_WORD_PRESET_BASE            4u
#define BL_INFO_WORD_PRESET_SIZE            5u
#define BL_INFO_WORD_CAPS                   6u
#define BL_CAP_SIGNED_PRESET_WRITE          ((uint32_t)0x00000002) // preset slots take signed presets

_Static_assert(APPLICATION_ADDRESS + APPLICATION_SLOT_SIZE == PRESET_STORE_ADDRESS,
               "the presets sit right above the slot");
_Static_assert(PRESET_SLOT_COUNT * PRESET_SLOT_SIZE == PRESET_STORE_SIZE,
               "the preset slots fill the preset pages exactly");
_Static_assert(PRESET_STORE_ADDRESS + PRESET_STORE_SIZE == BOOTLOADER_ANTIROLLBACK_ADDRESS,
               "then the bootloader's anti-rollback page");
_Static_assert(BOOTLOADER_ANTIROLLBACK_ADDRESS + BOOTLOADER_ANTIROLLBACK_SIZE == APPLICATION_RUNTIME_CONFIG,
               "then the runtime config");
_Static_assert(APPLICATION_RUNTIME_CONFIG + APPLICATION_RUNTIME_CONFIG_SIZE == FLASH_END_ADDRESS,
               "which is the last page");

#ifdef __cplusplus
}
#endif

#endif /* INC_MEMORY_MAP_H_ */
