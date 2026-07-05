#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nascom_nasbugt2_rom.h"

constexpr uint16_t NASCOM1_ROM_BASE = 0x0000;

// Base monitor ROM compiled from Nasbugt2 source.
constexpr uint16_t NASCOM1_ROM_SIZE = NASCOM1_NASBUGT2_ROM_SIZE;

// Built-in Z80 test ROM: writes a banner into NASCOM VRAM, then idles.
constexpr uint16_t NASCOM1_TEST_ROM_SIZE = 1024;
static const uint8_t NASCOM1_TEST_ROM[NASCOM1_TEST_ROM_SIZE] = {
  // 0000: LD HL,0849h   ; top visible row, column 0 in current renderer mapping
  0x21, 0x49, 0x08,
  // 0003: LD DE,0011h   ; message source pointer
  0x11, 0x11, 0x00,
  // 0006: LD B,23       ; bytes to copy
  0x06, 0x17,
  // 0008: copy_loop: LD A,(DE) / LD (HL),A / INC HL / INC DE / DJNZ copy_loop
  0x1A, 0x77, 0x23, 0x13, 0x10, 0xFA,
  // 000E: JP 000Eh      ; idle forever after writing test text
  0xC3, 0x0E, 0x00,

  // 0011: ASCII message (23 bytes)
  'E', 'S', 'P', 'V', 'G', 'A', ' ', 'Z', '8', '0', ' ',
  'T', 'E', 'S', 'T', ' ', 'R', 'O', 'M', ' ', 'O', 'K', '!'
};


struct RomImage {
  const char * name;
  uint16_t base;
  uint16_t size;
  const uint8_t * data;
};


static const RomImage ROM_LIBRARY[] = {
  { "NASBUG T2 (compiled)", NASCOM1_ROM_BASE, NASCOM1_ROM_SIZE, NASCOM1_NASBUGT2_ROM },
  { "NASCOM test ROM (writes VRAM)", NASCOM1_ROM_BASE, NASCOM1_TEST_ROM_SIZE, NASCOM1_TEST_ROM },
};

constexpr size_t ROM_LIBRARY_COUNT = sizeof(ROM_LIBRARY) / sizeof(ROM_LIBRARY[0]);


inline const RomImage * getDefaultROM() {
  return ROM_LIBRARY_COUNT > 0 ? &ROM_LIBRARY[0] : nullptr;
}


inline const RomImage * findROMByName(const char * name) {
  if (name == nullptr)
    return nullptr;

  for (size_t i = 0; i < ROM_LIBRARY_COUNT; ++i)
    if (strcmp(ROM_LIBRARY[i].name, name) == 0)
      return &ROM_LIBRARY[i];

  return nullptr;
}
