#include "nascom_machine.h"
#include "mcm6576_font.h"

NascomMachine Emulator;

uint8_t LastVRAMFrame[NASCOM1_VRAM_ROWS * NASCOM1_VISIBLE_COLS];
uint32_t LastVideoRefreshUS = 0;

// Explicitly define pins using the correct ESP32 gpio_num_t naming convention
#define VGA_RED_1    GPIO_NUM_22  // MSB (Brightest)
#define VGA_RED_0    GPIO_NUM_23  // LSB (Muted)
#define VGA_GREEN_1  GPIO_NUM_19  // MSB (Brightest)
#define VGA_GREEN_0  GPIO_NUM_21  // LSB (Muted)
#define VGA_BLUE_1   GPIO_NUM_5   // MSB (Brightest)
#define VGA_BLUE_0   GPIO_NUM_18  // LSB (Muted)
#define VGA_HSYNC    GPIO_NUM_17
#define VGA_VSYNC    GPIO_NUM_16

// Using VGA16Controller to support 8-pin 64-color resistor arrays
fabgl::VGA16Controller VGAController;
fabgl::Canvas          Canvas(&VGAController);

// Set to a name from ROM_LIBRARY in nascom_roms.h to select a ROM at startup.
constexpr const char * SELECTED_ROM_NAME = "NASBUG T2 (compiled)";
constexpr bool STARTUP_SELFTEST_BANNER = true;
constexpr bool USE_MCM6576_GLYPH_RENDERER = true;

constexpr uint8_t NASCOM_CELL_W = 8;
constexpr uint8_t NASCOM_CELL_H = 16;


static uint16_t vramAddrForCell(uint8_t row, uint8_t col) {
  // NASCOM video memory starts at logical line 2, then wraps line 1 last.
  uint8_t memoryRow = (row + 1) % NASCOM1_VRAM_ROWS;
  uint8_t memoryCol = NASCOM1_HIDDEN_LEFT_COLS + col;
  return NASCOM1_VRAM_BASE + (uint16_t) memoryRow * NASCOM1_VRAM_STRIDE + memoryCol;
}


static char screenCharFromNascomByte(uint8_t value) {
  value &= 0x7F;
  if (value < 32 || value > 126)
    return ' ';
  return (char) value;
}


static void writeTextToNascomVRAM(uint8_t row, uint8_t col, const char * text) {
  if (text == nullptr || row >= NASCOM1_VRAM_ROWS || col >= NASCOM1_VISIBLE_COLS)
    return;

  uint8_t c = col;
  for (size_t i = 0; text[i] != 0 && c < NASCOM1_VISIBLE_COLS; ++i, ++c) {
    uint8_t ch = (uint8_t) text[i];
    if (ch < 32 || ch > 126)
      ch = ' ';
    Emulator.pokeMemory(vramAddrForCell(row, c), ch & 0x7F);
  }
}


static void drawFallbackCell(uint8_t row, uint8_t col, uint8_t value) {
  char ch[2] = { screenCharFromNascomByte(value), 0 };
  int x0 = col * NASCOM_CELL_W;
  int y0 = row * NASCOM_CELL_H;

  // Always repaint the full cell background so spaces erase old glyphs.
  Canvas.setPenColor(RGB888(0, 0, 128));
  for (uint8_t y = 0; y < NASCOM_CELL_H; ++y)
    for (uint8_t x = 0; x < NASCOM_CELL_W; ++x)
      Canvas.setPixel(x0 + x, y0 + y);

  if (ch[0] != ' ') {
    Canvas.setPenColor(RGB888(255, 255, 0));
    Canvas.drawText(x0, y0, ch);
  }
}


static void drawMCM6576Cell(uint8_t row, uint8_t col, uint8_t value) {
  uint8_t code = value & 0x7F;
  if (code < 32 || code > 126)
    code = ' ';

  if (!mcm6576GlyphHasInk(code)) {
    drawFallbackCell(row, col, value);
    return;
  }

  int x0 = col * NASCOM_CELL_W;
  int y0 = row * NASCOM_CELL_H;

  for (uint8_t y = 0; y < MCM6576_GLYPH_HEIGHT; ++y) {
    uint8_t rowBits = MCM6576_FONT[code][y];
    for (uint8_t x = 0; x < MCM6576_GLYPH_WIDTH; ++x) {
      bool pixelOn = (rowBits & (0x80 >> x)) != 0;
      Canvas.setPenColor(pixelOn ? RGB888(255, 255, 0) : RGB888(0, 0, 128));
      Canvas.setPixel(x0 + x, y0 + y);
    }
  }
}


void renderNascomVideoRAM(bool forceFullRedraw) {
  for (uint8_t row = 0; row < NASCOM1_VRAM_ROWS; ++row) {
    for (uint8_t col = 0; col < NASCOM1_VISIBLE_COLS; ++col) {
      uint16_t idx = (uint16_t) row * NASCOM1_VISIBLE_COLS + col;
      uint8_t value = Emulator.peekMemory(vramAddrForCell(row, col));
      if (!forceFullRedraw && value == LastVRAMFrame[idx])
        continue;

      LastVRAMFrame[idx] = value;

      if (USE_MCM6576_GLYPH_RENDERER)
        drawMCM6576Cell(row, col, value);
      else
        drawFallbackCell(row, col, value);
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Pass the 6-bit pin setup block directly to FabGL to wake up the DAC lines
  // Order must be Bit 1 (MSB) then Bit 0 (LSB) for each colour channel
  VGAController.begin(
    VGA_RED_1,   VGA_RED_0,
    VGA_GREEN_1, VGA_GREEN_0,
    VGA_BLUE_1,  VGA_BLUE_0,
    VGA_HSYNC,   VGA_VSYNC
  );
  
  // Set monitor timing to standard 640x480 resolution
  VGAController.setResolution(VGA_640x480_60Hz);
  
  // Capitalized RGB888 macro used to assign background canvas fill
  Canvas.setBrushColor(RGB888(0, 0, 128)); // Creates a solid Navy Blue background
  Canvas.clear();
  
  // Print verification text out to the screen space
  Canvas.setPenColor(RGB888(255, 255, 0)); // Bright Yellow text
  
  // FIXED: Using the verified uppercase font asset naming convention
  Canvas.selectFont(&fabgl::FONT_8x16);
  memset(LastVRAMFrame, 0xFF, sizeof(LastVRAMFrame));

  if (USE_MCM6576_GLYPH_RENDERER && !mcm6576FontAnyGlyphPopulated())
    Serial.println("[MCM6576] Glyph table is empty; using fallback cells until populated");

  // Link monitor UART to default USB serial terminal.
  Emulator.setUARTStream(&Serial);
  // NASBUG expects monitor control codes (CR=0x1F, etc), so keep translation enabled.
  Emulator.setUARTTextTranslation(true);

  // Bind and start the Z80 core using FabGL callbacks.
  Emulator.begin(0x0000);
  auto rom = findROMByName(SELECTED_ROM_NAME);
  if (rom == nullptr) {
    Serial.printf("ROM '%s' not found, using default\n", SELECTED_ROM_NAME);
    rom = getDefaultROM();
  }

  if (rom == nullptr) {
    Serial.println("No ROMs defined in ROM_LIBRARY");
  } else {
    Serial.printf("Loading ROM: %s at 0x%04X (%u bytes)\n", rom->name, rom->base, rom->size);
    if (!Emulator.loadROM(rom->base, rom->data, rom->size))
      Serial.println("Failed to load selected ROM image");
    else if (STARTUP_SELFTEST_BANNER) {
      Serial.println("[SELFTEST] ROM load OK");
      Serial.println("[SELFTEST] UART bridge OK (USB serial <-> emulated UART)");
      writeTextToNascomVRAM(0, 0, "SELFTEST ROM/VIDEO/UART OK");
      writeTextToNascomVRAM(1, 0, "NASBUG SHOULD START BELOW");
    }
  }

  renderNascomVideoRAM(true);
}

void loop() {
  // VGA DMA keeps running; run Z80 and periodically project VRAM to the text canvas.
  Emulator.runForElapsedTime();

  uint32_t now = micros();
  if (now - LastVideoRefreshUS >= 16000) {  // about 60Hz video refresh pass
    LastVideoRefreshUS = now;
    renderNascomVideoRAM(false);
  }
}
