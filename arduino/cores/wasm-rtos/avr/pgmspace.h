/* Program memory is ordinary memory in wasm: PROGMEM and its readers are plain accesses. */
#ifndef WASM_RTOS_ARDUINO_PGMSPACE_H
#define WASM_RTOS_ARDUINO_PGMSPACE_H

#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PGM_P const char*
#define PGM_VOID_P const void*
#define PSTR(s) (s)
typedef char prog_char;
typedef uint8_t prog_uchar;

#define pgm_read_byte(address) (*(const uint8_t*)(address))
#define pgm_read_word(address) (*(const uint16_t*)(address))
#define pgm_read_dword(address) (*(const uint32_t*)(address))
#define pgm_read_float(address) (*(const float*)(address))
#define pgm_read_ptr(address) (*(void* const*)(address))
#define pgm_read_byte_near(address) pgm_read_byte(address)
#define pgm_read_word_near(address) pgm_read_word(address)
#define pgm_read_dword_near(address) pgm_read_dword(address)
#define pgm_read_byte_far(address) pgm_read_byte(address)
#define pgm_read_word_far(address) pgm_read_word(address)

#define memcpy_P memcpy
#define memcmp_P memcmp
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcat_P strcat
#define strcmp_P strcmp
#define strncmp_P strncmp
#define strlen_P strlen
#define strnlen_P strnlen
#define sprintf_P sprintf
#define snprintf_P snprintf
#define printf_P printf

#endif
