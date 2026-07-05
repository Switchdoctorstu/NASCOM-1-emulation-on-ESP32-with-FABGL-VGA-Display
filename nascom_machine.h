#pragma once

#include "fabgl.h"
#include "emudevs/Z80.h"
#include "nascom_roms.h"

constexpr int Z80_RAM_SIZE = 65536;
constexpr uint32_t EMULATED_CPU_HZ = 2000000;  // 2 MHz baseline for Nascom-era timing

// NASCOM 1 video RAM model: 1K RAM arranged as 16 rows x 64-byte stride.
// Typically only first 48 columns are visible on the original display timing.
constexpr uint16_t NASCOM1_VRAM_BASE = 0x0800;
constexpr uint16_t NASCOM1_VRAM_SIZE = 1024;
constexpr uint8_t  NASCOM1_VRAM_ROWS = 16;
constexpr uint8_t  NASCOM1_VRAM_STRIDE = 64;
constexpr uint8_t  NASCOM1_VISIBLE_COLS = 48;
// NASBUG display text starts at +10 in each 64-byte row.
constexpr uint8_t  NASCOM1_HIDDEN_LEFT_COLS = 10;


class NascomMetaBus {
public:
  using IOReadHandler = int (*)(void * context, uint8_t port);
  using IOWriteHandler = void (*)(void * context, uint8_t port, uint8_t value);

  struct IOPortMeta {
    const char * name;
    IOReadHandler read;
    IOWriteHandler write;
    void * context;
  };

  NascomMetaBus() {
    clearAll();
  }

  void clearAll() {
    memset(m_memory, 0x00, sizeof(m_memory));
    m_hasROM = false;
    m_hasUnmapped = false;

    for (int p = 0; p < 256; ++p) {
      m_ports[p].name = "UNMAPPED";
      m_ports[p].read = nullptr;
      m_ports[p].write = nullptr;
      m_ports[p].context = nullptr;
    }
  }

  void mapAllRAM() {
    m_hasROM = false;
    m_hasUnmapped = false;
  }

  void mapROM(uint16_t base, uint16_t size) {
    m_romBase = base;
    m_romSize = size;
    m_hasROM = (size != 0);
  }

  void mapUnmapped(uint16_t base, uint16_t size) {
    m_unmappedBase = base;
    m_unmappedSize = size;
    m_hasUnmapped = (size != 0);
  }

  bool loadROM(uint16_t base, const uint8_t * data, size_t size) {
    if (data == nullptr || size == 0 || (base + size) > 65536)
      return false;

    for (size_t i = 0; i < size; ++i)
      m_memory[base + i] = data[i];

    mapROM(base, (uint16_t) size);
    return true;
  }

  int readByte(uint16_t addr) const {
    if (isInRange(addr, m_hasUnmapped, m_unmappedBase, m_unmappedSize))
      return 0xFF;
    return m_memory[addr];
  }

  void writeByte(uint16_t addr, uint8_t value) {
    if (isInRange(addr, m_hasUnmapped, m_unmappedBase, m_unmappedSize))
      return;

    if (isInRange(addr, m_hasROM, m_romBase, m_romSize))
      return;

    m_memory[addr] = value;
  }

  void installPort(uint8_t port,
                   const char * name,
                   IOReadHandler read,
                   IOWriteHandler write,
                   void * context) {
    m_ports[port].name = name ? name : "UNNAMED";
    m_ports[port].read = read;
    m_ports[port].write = write;
    m_ports[port].context = context;
  }

  int ioRead(uint8_t port) {
    auto const & p = m_ports[port];
    if (p.read)
      return p.read(p.context, port) & 0xFF;
    return 0xFF;
  }

  void ioWrite(uint8_t port, uint8_t value) {
    auto const & p = m_ports[port];
    if (p.write)
      p.write(p.context, port, value);
  }

  void printMapSummary() const {
    int romCount = 0;
    int ramCount = 0;
    int unmappedCount = 0;

    for (int i = 0; i < Z80_RAM_SIZE; ++i) {
      uint16_t addr = (uint16_t) i;
      if (isInRange(addr, m_hasUnmapped, m_unmappedBase, m_unmappedSize))
        ++unmappedCount;
      else if (isInRange(addr, m_hasROM, m_romBase, m_romSize))
        ++romCount;
      else
        ++ramCount;
    }

    Serial.printf("Memory map: ROM=%d RAM=%d UNMAPPED=%d\\n", romCount, ramCount, unmappedCount);
  }

private:
  static bool isInRange(uint16_t addr, bool enabled, uint16_t base, uint16_t size) {
    if (!enabled || size == 0)
      return false;

    uint32_t end = (uint32_t) base + size;
    return addr >= base && addr < end;
  }

  uint8_t     m_memory[Z80_RAM_SIZE];
  uint16_t    m_romBase = 0;
  uint16_t    m_romSize = 0;
  bool        m_hasROM = false;
  uint16_t    m_unmappedBase = 0;
  uint16_t    m_unmappedSize = 0;
  bool        m_hasUnmapped = false;
  IOPortMeta  m_ports[256];
};


class NascomMachine {
public:
  using PIOReadHook = uint8_t (*)(void * context, uint8_t channel, bool isControlReg);
  using PIOWriteHook = void (*)(void * context, uint8_t channel, bool isControlReg, uint8_t value);

  static constexpr uint8_t NASBUG_CHAR_BS = 0x1D;
  static constexpr uint8_t NASBUG_CHAR_FF = 0x1E;
  static constexpr uint8_t NASBUG_CHAR_CR = 0x1F;

  NascomMachine() {
    m_cpu.setCallbacks(this, readByte, writeByte, readWord, writeWord, readIO, writeIO);
  }

  void begin(uint16_t startAddress) {
    configureNascom1Map();

    m_cpu.reset();
    m_cpu.setPC(startAddress);
    m_lastMicros = micros();
  }

  bool loadROM(uint16_t address, const uint8_t * data, size_t length) {
    return m_bus.loadROM(address, data, length);
  }

  void setUARTStream(Stream * stream) {
    m_uartStream = stream;
  }

  void setUARTTextTranslation(bool enabled) {
    m_uartTranslateText = enabled;
    m_uartLastRxWasCR = false;
  }

  uint8_t peekMemory(uint16_t address) const {
    return (uint8_t) m_bus.readByte(address);
  }

  void pokeMemory(uint16_t address, uint8_t value) {
    m_bus.writeByte(address, value);
  }

  void setPIOHooks(PIOReadHook readHook, PIOWriteHook writeHook, void * hookContext) {
    m_pioReadHook = readHook;
    m_pioWriteHook = writeHook;
    m_pioHookContext = hookContext;
  }

  void configureNascom1Map() {
    m_bus.clearAll();

    // Start with full 64K RAM, then overlay ROM and optional holes.
    m_bus.mapAllRAM();
    m_bus.mapROM(NASCOM1_ROM_BASE, NASCOM1_ROM_SIZE);

    // Reserve one IO debug hole example in memory map for future hardware windows.
    m_bus.mapUnmapped(0xF800, 0x0800);

    // NASBUG T2 monitor-facing IO map:
    // - 00h: keyboard matrix read and general output latch (scan/cassette/motor bits)
    // - 01h: UART data (TTY)
    // - 02h: UART status (read) / control (write)
    // - 03h: reserved in this model
    // - 04h..07h: PIO channel A/B data+control framework
    m_bus.installPort(0x00, "NASCOM KBD/PORT0", ioReadPort0, ioWritePort0, this);
    m_bus.installPort(0x01, "NASCOM UART DATA", ioReadUARTData, ioWriteUARTData, this);
    m_bus.installPort(0x02, "NASCOM UART STATUS/CTRL", ioReadUARTStatus, ioWriteUARTControl, this);
    m_bus.installPort(0x04, "NASCOM PIO A DATA", ioReadPIOAData, ioWritePIOAData, this);
    m_bus.installPort(0x05, "NASCOM PIO A CTRL", ioReadPIOACtrl, ioWritePIOACtrl, this);
    m_bus.installPort(0x06, "NASCOM PIO B DATA", ioReadPIOBData, ioWritePIOBData, this);
    m_bus.installPort(0x07, "NASCOM PIO B CTRL", ioReadPIOBCtrl, ioWritePIOBCtrl, this);

    m_bus.printMapSummary();
  }

  void runForElapsedTime() {
    pollPeripheralState();

    uint32_t now = micros();
    uint32_t elapsedUs = now - m_lastMicros;
    if (elapsedUs == 0)
      return;

    m_lastMicros = now;

    // Run roughly elapsedUs * (Hz / 1_000_000) cycles, then return control to Arduino loop().
    int32_t cycleBudget = (int32_t) ((uint64_t) elapsedUs * EMULATED_CPU_HZ / 1000000ULL);
    while (cycleBudget > 0) {
      int usedCycles = m_cpu.step();
      cycleBudget -= usedCycles;

      if (m_cpu.getStatus() == fabgl::Z80_STATUS_HALT)
        break;
    }
  }

private:
  static constexpr uint8_t UART_STATUS_RX_READY = 0x80;
  static constexpr uint8_t UART_STATUS_TX_READY = 0x40;

  struct UARTState {
    uint8_t status = UART_STATUS_TX_READY;
    uint8_t control = 0x00;
  };

  struct KeyboardState {
    uint8_t port0Latch = 0x00;
  };

  struct PIOChannelState {
    uint8_t data = 0x00;
    uint8_t control = 0x00;
  };

  void pollPeripheralState() {
    // Expose monitor-compatible UART status bits:
    // bit 7 = RX data available, bit 6 = TX ready.
    m_uart.status = UART_STATUS_TX_READY;
    if (m_uartStream && m_uartStream->available() > 0)
      m_uart.status |= UART_STATUS_RX_READY;
  }

  uint8_t readUARTData() {
    while (m_uartStream && m_uartStream->available() > 0) {
      int raw = m_uartStream->read();
      if (raw < 0)
        break;

      uint8_t value = (uint8_t) raw;
      if (!m_uartTranslateText) {
        pollPeripheralState();
        return value;
      }

      // Map typical terminal controls into NASBUG monitor controls.
      if (value == '\r') {
        m_uartLastRxWasCR = true;
        pollPeripheralState();
        return NASBUG_CHAR_CR;
      }

      if (value == '\n') {
        if (m_uartLastRxWasCR) {
          m_uartLastRxWasCR = false;
          continue;  // suppress LF in CRLF sequences
        }
        pollPeripheralState();
        return NASBUG_CHAR_CR;
      }

      m_uartLastRxWasCR = false;

      if (value == 0x08 || value == 0x7F)
        value = NASBUG_CHAR_BS;

      pollPeripheralState();
      return value;
    }

    pollPeripheralState();
    return 0x00;
  }

  void writeUARTData(uint8_t value) {
    if (!m_uartStream) {
      pollPeripheralState();
      return;
    }

    if (!m_uartTranslateText) {
      m_uartStream->write(value);
      pollPeripheralState();
      return;
    }

    // Map NASBUG controls for common USB serial terminals.
    if (value == NASBUG_CHAR_CR) {
      m_uartStream->write('\r');
      m_uartStream->write('\n');
    } else if (value == NASBUG_CHAR_BS) {
      // NASBUG uses BS in dump formatting to overstrike checksum columns.
      // Ignore it on USB serial so hex lines remain readable in modern terminals.
    } else if (value == NASBUG_CHAR_FF) {
      m_uartStream->write('\f');
    } else {
      m_uartStream->write(value);
    }

    pollPeripheralState();
  }

  uint8_t readUARTStatus() {
    pollPeripheralState();
    return m_uart.status;
  }

  void writeUARTControl(uint8_t value) {
    // Keep the control value latched for future mode/baud emulation.
    m_uart.control = value;
  }

  uint8_t readPort0() const {
    // Matrix scanner stub: no keys pressed (active-low matrix => all 1s).
    // NASBUG then CPLs this value, so idle scan result becomes 0x00.
    return 0xFF;
  }

  void writePort0(uint8_t value) {
    // NASBUG flip/flpflp maintains bit state in RAM and writes to OUT (0).
    // Latch the full output byte so keyboard/cassette/motor semantics can be added later.
    m_keyboard.port0Latch = value;
  }

  uint8_t readPIO(uint8_t channel, bool isControlReg) {
    if (channel > 1)
      return 0xFF;

    if (m_pioReadHook)
      return m_pioReadHook(m_pioHookContext, channel, isControlReg);

    return isControlReg ? m_pio[channel].control : m_pio[channel].data;
  }

  void writePIO(uint8_t channel, bool isControlReg, uint8_t value) {
    if (channel > 1)
      return;

    if (isControlReg)
      m_pio[channel].control = value;
    else
      m_pio[channel].data = value;

    if (m_pioWriteHook)
      m_pioWriteHook(m_pioHookContext, channel, isControlReg, value);
  }

  static int readByte(void * context, int addr) {
    auto m = (NascomMachine *) context;
    return m->m_bus.readByte(addr & 0xFFFF);
  }

  static void writeByte(void * context, int addr, int value) {
    auto m = (NascomMachine *) context;
    m->m_bus.writeByte(addr & 0xFFFF, value & 0xFF);
  }

  static int readWord(void * context, int addr) {
    int lo = readByte(context, addr);
    int hi = readByte(context, addr + 1);
    return lo | (hi << 8);
  }

  static void writeWord(void * context, int addr, int value) {
    writeByte(context, addr, value & 0xFF);
    writeByte(context, addr + 1, (value >> 8) & 0xFF);
  }

  static int readIO(void * context, int addr) {
    auto m = (NascomMachine *) context;
    return m->m_bus.ioRead(addr & 0xFF);
  }

  static void writeIO(void * context, int addr, int value) {
    auto m = (NascomMachine *) context;
    m->m_bus.ioWrite(addr & 0xFF, value & 0xFF);
  }

  static int ioReadPort0(void * context, uint8_t) {
    return ((NascomMachine *) context)->readPort0();
  }

  static void ioWritePort0(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writePort0(value);
  }

  static int ioReadUARTData(void * context, uint8_t) {
    return ((NascomMachine *) context)->readUARTData();
  }

  static void ioWriteUARTData(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writeUARTData(value);
  }

  static int ioReadUARTStatus(void * context, uint8_t) {
    return ((NascomMachine *) context)->readUARTStatus();
  }

  static void ioWriteUARTControl(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writeUARTControl(value);
  }

  static int ioReadPIOAData(void * context, uint8_t) {
    return ((NascomMachine *) context)->readPIO(0, false);
  }

  static void ioWritePIOAData(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writePIO(0, false, value);
  }

  static int ioReadPIOACtrl(void * context, uint8_t) {
    return ((NascomMachine *) context)->readPIO(0, true);
  }

  static void ioWritePIOACtrl(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writePIO(0, true, value);
  }

  static int ioReadPIOBData(void * context, uint8_t) {
    return ((NascomMachine *) context)->readPIO(1, false);
  }

  static void ioWritePIOBData(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writePIO(1, false, value);
  }

  static int ioReadPIOBCtrl(void * context, uint8_t) {
    return ((NascomMachine *) context)->readPIO(1, true);
  }

  static void ioWritePIOBCtrl(void * context, uint8_t, uint8_t value) {
    ((NascomMachine *) context)->writePIO(1, true, value);
  }

  fabgl::Z80 m_cpu;
  NascomMetaBus m_bus;
  UARTState m_uart;
  Stream * m_uartStream = &Serial;
  bool m_uartTranslateText = true;
  bool m_uartLastRxWasCR = false;
  KeyboardState m_keyboard;
  PIOChannelState m_pio[2];
  PIOReadHook m_pioReadHook = nullptr;
  PIOWriteHook m_pioWriteHook = nullptr;
  void * m_pioHookContext = nullptr;
  uint32_t   m_lastMicros = 0;
};
