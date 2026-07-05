# ESPVGA NASCOM 1 Emulator

## Project Status

This project emulates a NASCOM 1 style Z80 machine on ESP32 using FabGL for VGA output.
The current target is practical monitor operation with NASBUG T2 ROM behavior, VGA text display, and USB serial terminal I/O through emulated UART ports.

## Machine Specification

### CPU

- CPU core: Z80 (FabGL emudevs/Z80)
- Emulated clock: 2.0 MHz nominal
- Reset PC: 0000h

### Address Space and Memory Map

- Address width: 16-bit (64 KiB)
- Total emulated memory array: 65536 bytes

Power-up mapping:

- 0000h to 03FFh: ROM (base monitor ROM, NASBUG T2 build)
- 0400h to F7FFh: RAM
- 0800h to 0BFFh: NASCOM video RAM window (within RAM)
- F800h to FFFFh: UNMAPPED (reads return FFh, writes ignored)

Notes:

- Emulator starts by mapping all 64 KiB as RAM, overlays ROM at base, then marks the top 2 KiB as unmapped.
- ROM writes are blocked by memory mode.

### ROM Configuration

ROM library currently includes:

1. NASBUG T2 (compiled) at 0000h, 1024 bytes (default)
2. NASCOM test ROM (writes VRAM) at 0000h, 1024 bytes

Default ROM selection is name-based in ESPVGA.ino.

### Video Model

#### Logical Video RAM

- Base address: 0800h
- Size: 1024 bytes
- Geometry in RAM: 16 rows x 64-byte stride
- Visible text region: 16 rows x 48 columns

#### Addressing Used by Renderer

For visible cell (row, col):

- memoryRow = (row + 1) mod 16
- memoryCol = 10 + col
- VRAM address = 0800h + memoryRow * 64 + memoryCol

This matches NASBUG text layout conventions used by the current monitor setup.

#### Raster Output

- VGA timing: 640x480 @ 60 Hz (FabGL preset)
- Cell size: 8x16 pixels
- Refresh pass: approximately every 16 ms (about 60 Hz loop pass)

#### Character Generator

- Character renderer path: MCM6576 glyph renderer enabled by default
- Embedded character ROM source: full 4 KiB dump imported into mcm6576_font.h
- Glyph table shape: 256 glyphs x 16 rows x 8 pixels
- Pixel bit order: MSB is leftmost pixel in each row byte

Fallback behavior:

- If a glyph has no set pixels, renderer falls back to ASCII drawText cell rendering.

### I/O Port Map

Monitor-facing I/O map implemented:

- Port 00h: keyboard/port0 latch
  - Read: returns FFh (keyboard stub, no key pressed)
  - Write: latches output byte for future keyboard/cassette/motor behavior
- Port 01h: UART data
  - Read: receive byte from bound stream
  - Write: transmit byte to bound stream
- Port 02h: UART status/control
  - Read: UART status
    - bit 7: RX ready
    - bit 6: TX ready
  - Write: control byte latched (reserved for future UART mode emulation)
- Port 03h: reserved (not installed in current model)
- Port 04h: PIO A data
- Port 05h: PIO A control
- Port 06h: PIO B data
- Port 07h: PIO B control

#### UART Bridge

Default UART stream is linked to USB serial.

- Arduino Serial speed: 115200
- UART text translation layer: enabled by default
  - RX CR or LF -> NASBUG CR (1Fh)
  - RX Backspace or DEL -> NASBUG BS (1Dh)
  - CRLF sequence de-duplicated to single NASBUG CR
  - TX NASBUG CR -> CRLF
  - TX NASBUG BS -> backspace
  - TX NASBUG FF -> form feed

### PIO Framework

PIO ports 04h to 07h support internal latched registers and optional external hooks.

Hook API:

- setPIOHooks(readHook, writeHook, context)

If no hook is installed:

- Reads return current latched register values.
- Writes update latches only.

### Keyboard State

Current status: stub only.

- No physical matrix scan implementation yet.
- Port 00h read always indicates idle (no key pressed).

### Startup Self-Test

When enabled, setup emits:

- Serial self-test lines confirming ROM load and UART bridge
- Two text lines directly written into emulated VRAM before normal monitor activity

This is a bring-up aid and can be disabled by constant in ESPVGA.ino.

## Quick Start

1. Open ESPVGA.ino in Arduino IDE.
2. Select your ESP32 board/port.
3. Build and upload.
4. Open Serial Monitor at 115200.
5. Confirm startup self-test lines and monitor activity.

## Build and Regeneration

### Rebuild NASBUG ROM From Source

Run in PowerShell from project root:

- .\build_nasbug_rom.ps1

Inputs and outputs:

- Input source: Nasbugt2.mac
- Binary output: nasbugt2.bin
- Generated header: nascom_nasbugt2_rom.h

### Switch ROM Images

ROM images are declared in nascom_roms.h and selected by name in ESPVGA.ino.

## Repository Files

- ESPVGA.ino: main sketch, VGA/render loop, startup wiring
- nascom_machine.h: machine core, memory map, I/O handlers
- nascom_roms.h: ROM library definitions
- nascom_nasbugt2_rom.h: generated monitor ROM bytes
- mcm6576_font.h: embedded character ROM glyph data
- Nasbugt2.mac: NASBUG source
- build_nasbug_rom.ps1: ROM build script

## Coding Agent Continuation Notes

These notes are intended for future contributors and coding agents.

### High-Impact Touch Points

- Video mapping and renderer path: ESPVGA.ino
- CPU/memory/ports and UART behavior: nascom_machine.h
- ROM selection and default image: nascom_roms.h
- Font authenticity work: mcm6576_font.h

### Safe Refactor Boundaries

- Do not change VRAM mapping constants unless also validating NASBUG cursor/screen behavior.
- Keep UART status bits (RX=bit7, TX=bit6) stable unless intentionally remapping monitor semantics.
- Preserve ROM base at 0000h unless redesigning boot assumptions.

### Current Technical Debt / Backlog

1. Implement true NASCOM keyboard matrix scanning on port 00h.
2. Model UART control register semantics to specific historical UART behavior.
3. Attach concrete peripherals to PIO hooks (04h to 07h).
4. Improve display authenticity beyond glyph shape (timing, scan, attributes if needed).

### Validation Checklist After Any Core Change

1. Build/upload succeeds.
2. Serial monitor opens at 115200 and receives startup lines.
3. NASBUG ROM loads and monitor remains interactive.
4. VGA text remains aligned (16x48 visible region) with no row/column drift.
5. Port polling loops do not stall due to status bit regressions.

## Known Differences vs Original Hardware

1. Keyboard matrix is not fully emulated yet.
2. UART control register behavior is latched but not modeled to specific original UART silicon timing/modes.
3. PIO device-level behavior is not attached by default; framework only.
4. Video output is VGA text-cell rendering from VRAM, not cycle-accurate original video circuitry emulation.

## Validation Snapshot

At time of this specification:

- NASBUG T2 ROM compiles successfully from source.
- Default ROM boots from 0000h mapping.
- UART monitor path is connected to USB serial.
- MCM6576 font dump is embedded and renderer is active.
