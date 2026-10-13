# ESP32-024 CRT Scope — retro phosphor oscilloscope for the CYD 2.4" boards

A green-phosphor, old-school CRT-style oscilloscope that runs on the **TZT L1435 2.4" / ESP32-024**
Cheap-Yellow-Display family boards (ESP32 + ST7789 240x320 + XPT2046 resistive touch, micro-USB + USB-C).

Features: edge-triggered sweep, frequency + Vpp readout, decaying phosphor trail,
5 demo waveforms (when no probe signal), full resistive **touch UI** with 3-point calibration,
persistent settings (NVS), BOOT-button fallback navigation.

![scope](https://img.shields.io/badge/display-ST7789_240x320-green) ![touch](https://img.shields.io/badge/touch-XPT2046_shared_HSPI-orange)

---

## ⚠️ The ESP32-024 touch trap (read this before anything)

On these boards **the XPT2046 touchscreen shares the SPI bus with the display** — but there is a
silicon-level gotcha that wastes days if you don't know it:

> **Touch reads only work when the bus is driven by the HSPI peripheral (SPI2, native IO_MUX routing
> on GPIO 12/13/14). Driving the same pins through VSPI (SPI3) remapped via the GPIO matrix makes
> every XPT2046 read return 0 / 0xFFF — the MISO line is never seen by the peripheral.**

Symptoms: display works fine (writes don't care), `Z1/X/Y` reads are dead, everything *looks*
correctly wired. If your touch code is built on `TFT_eSPI`'s default bus (global `SPI` = VSPI),
it will silently fail on these clones. Initialize a dedicated `SPIClass bus(HSPI); bus.begin(14, 12, 13, 15);`
and run display + touch on it (this project uses Adafruit_ST7789 on HSPI for exactly this reason).

Two more board-specific must-knows:

* **Hold SD_CS (GPIO 5) HIGH during every touch transaction.** The microSD socket shares the bus;
  a floating/low SD_CS lets the card controller fight the XPT2046 on MISO.
* The panel needs color inversion ON — `Adafruit_ST7789::init()` already sends it, so **use plain
  RGB565 colors**. (If you port this to `TFT_eSPI`, which sends no inversion command by default,
  you must software-negate every color or send INVON twice.)

## Verified pinout (ESP32-024 / TZT L1435 2.4")

| Function | GPIO |
|---|---|
| SPI SCLK (shared LCD + touch) | 14 |
| SPI MISO  (shared LCD + touch) | 12 |
| SPI MOSI  (shared LCD + touch) | 13 |
| LCD CS | 15 |
| LCD DC | 2 |
| Backlight | 27 |
| **Touch CS** | **33** |
| **Touch IRQ (PENIRQ)** | **36** |
| SD card CS (keep HIGH!) | 5 |
| Scope probe input (ADC) | 34 |
| BOOT button (menu fallback) | 0 |

Cross-referenced with [CheshirCa/CYD-Display-Library-for-PureBasic](https://github.com/CheshirCa/CYD-Display-Library-for-PureBasic)
(ESP32-024 profile — source of the exact touch read sequence used here) and
[Keralots/BambuHelper](https://github.com/Keralots/BambuHelper) (`cyd_2432s024.ini`).

## The touch read sequence that works

```cpp
SPIClass bus(HSPI);                    // NOT the default VSPI!
// bus.begin(14, 12, 13, 15);          // SCLK, MISO, MOSI, SS

uint16_t read(uint8_t cmd) {
  digitalWrite(15, HIGH);              // LCD CS idle
  digitalWrite(5, HIGH);               // SD CS idle  <- critical
  bus.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(33, LOW);               // touch CS
  bus.transfer(cmd);
  uint16_t v = bus.transfer16(0) >> 3; // 1 dummy bit + 12 data bits
  digitalWrite(33, HIGH);
  bus.endTransaction();
  return v;
}

// pressure = z1 + 4095 - z2   (commands 0xB1 / 0xC1; X = 0x91, Y = 0xD1)
// touched  = (pressure >= 400) || (digitalRead(36) == LOW)
```

## Building

```bash
pio run -t upload          # PlatformIO, board: esp32dev
pio device monitor         # 115200 — raw touch events, telemetry
```

Dependencies (auto-installed): Adafruit GFX + Adafruit ST7735/ST7789.

## Using it

* **Scope mode**: green phosphor trace on a graticule, `ms/DIV` derived from the real sample rate,
  top bar shows volts/div + timebase, bottom bar shows input mode, frequency, Vpp and TRIG'D blink.
* **Probe**: connect a 0–3.3 V signal to **GPIO 34** — the scope auto-switches from DEMO to probe mode.
* **Touch anywhere** → settings window (timebase, phosphor persistence, demo waveform, grid).
  First use runs a 3-point calibration (touch the crosshairs); it is stored in NVS.
* **BOOT button** mirrors menu navigation (short = next row, long = change) if you ever need it.

## Credits

* Touch sequence + ESP32-024 pin facts: [CheshirCa/CYD-Display-Library-for-PureBasic](https://github.com/CheshirCa/CYD-Display-Library-for-PureBasic)
* Variant documentation: [Keralots/BambuHelper](https://github.com/Keralots/BambuHelper),
  [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

## License

MIT — see [LICENSE](LICENSE).
