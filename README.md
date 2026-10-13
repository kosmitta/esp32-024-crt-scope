# ESP32-024 CRT Scope — retro phosphor oscilloscope for the CYD 2.4" boards

A green-phosphor, old-school CRT-style oscilloscope that runs on the **TZT L1435 2.4" / ESP32-024**
Cheap-Yellow-Display family boards (ESP32 + ST7789 240x320 + XPT2046 resistive touch, micro-USB + USB-C).

> **Came here from a search like “ESP32-024 XPT2046 touch not working”, “XPT2046 MISO always 0”
> or “CYD 2.4 touch dead on shared SPI”?** Skip straight to
> [The ESP32-024 touch trap](#-the-esp32-024-touch-trap-read-this-before-anything) —
> that is very likely your problem, and the fix is one line.

![scope](https://img.shields.io/badge/display-ST7789_240x320-green) ![touch](https://img.shields.io/badge/touch-XPT2046_shared_HSPI-orange) ![license](https://img.shields.io/badge/license-MIT-blue)

## Identify your board

![the board](doc/board.jpg)

The board this project was developed and verified on: **ESP32-024 / TZT L1435 2.4"** —
ESP32-WROOM-32 module, 2.4" 240x320 display (ST7789), resistive touch (XPT2046),
**both micro-USB (CH340, flashing) and USB-C (power-only on most units)**, microSD slot on the back edge.
Check the silkscreen marking on your PCB before flashing anything — the CYD family has many
siblings with different controllers and pin maps (see the
[witnessmenow CYD repo](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) for the family tree).

Features: edge-triggered sweep, frequency + Vpp readout, decaying phosphor trail,
5 demo waveforms (when no probe signal is present), full resistive **touch UI** with 3-point
calibration, persistent settings (NVS), BOOT-button fallback navigation.

## ⚠️ The ESP32-024 touch trap (read this before anything)

On these boards the XPT2046 touchscreen shares the SPI bus with the display — but there is a
silicon-level gotcha that wastes days if you don't know it:

> **Touch reads only worked here when the bus was driven by the HSPI peripheral (SPI2, native
> IO_MUX routing on GPIO 12/13/14). Driving the same pins through VSPI (SPI3) remapped via the
> GPIO matrix made every XPT2046 read return 0 (or 0xFFF) — MISO is simply never seen by the
> peripheral.**
>
> *Behavior observed on the tested ESP32-024 unit. If you can reproduce (or refute) it on other
> units, please open an issue — confirming reports are welcome.*

Symptoms: the display works fine (writes don't care about the routing), `Z1/X/Y` reads are dead,
everything looks correctly wired. If your touch code is built on `TFT_eSPI`'s default bus
(global `SPI` = VSPI), it will silently fail on these boards. Initialize a dedicated
`SPIClass bus(HSPI); bus.begin(14, 12, 13, 15);` and run display + touch on it
(this project uses Adafruit_ST7789 on HSPI for exactly this reason).

Two more board-specific must-knows:

* **Hold SD_CS (GPIO 5) HIGH during every touch transaction.** The microSD socket shares the bus;
  a floating/low SD_CS lets the card controller fight the XPT2046 on MISO.
* The panel needs color inversion ON — `Adafruit_ST7789::init()` already sends it, so **use plain
  RGB565 colors**. (If you port this to `TFT_eSPI`, which sends no inversion command by default,
  you must software-negate every color or send INVON twice.)

## How this was found (so you can recognise the same failure)

The touch on the development unit appeared completely dead. In hindsight, every test pointed at
the bus routing, not the hardware:

| Attempt | Result |
|---|---|
| XPT2046 on the TFT_eSPI bus (VSPI, matrix-routed), CS on 15 different pins | reads always 0 / 0xFFF |
| Classic CYD pinout (CLK 25 / MOSI 32 / MISO 39, CS 33) on a hardware HSPI + reference libraries | dead — that pinout does not exist on ESP32-024 |
| I²C touch (CST816/FT6336/GT911) scan over all GPIO pairs, incl. reset sequencing | no devices — no I²C touch on this board |
| PENIRQ (GPIO 36) reacting to touch while bus quiet | chip alive — proof the controller works |
| **Same wiring, same commands, bus moved to HSPI (`SPIClass bus(HSPI)` + `bus.begin(14,12,13,15)`, SD_CS HIGH)** | **touch works instantly, stable coordinates** |

If your board shows the first row's symptom, the fix costs one line.

## ⚠️ Probe input safety

The scope input is **GPIO 34, 0–3.3 V only** (ADC, 11 dB attenuation). It is **not** rated for
mains voltage, negative voltages, 5 V logic or any signal outside 0–3.3 V — connecting such
signals can destroy the pin or the ESP32. Use a suitable divider/probe front-end for anything
beyond a 3.3 V logic signal. A real oscilloscope this is not.

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

## Flashing

### Option A — prebuilt binary (no toolchain needed)

Grab `scope_full_4m.bin` from [Releases](../../releases) — a merged 4 MB image
(bootloader + partition table + firmware), verified on the ESP32-024.

* with esptool: `esptool --chip esp32 --port <PORT> --baud 921600 write_flash 0x0 scope_full_4m.bin`
* or drag & drop the same file in a Chromium browser at [web.esptool.io](https://web.esptool.io)
  (connect the micro-USB port, choose 4 MB flash size, offset 0x0)

### Option B — build from source

```bash
pio run -t upload          # PlatformIO, board: esp32dev
pio device monitor         # 115200 — raw touch events, telemetry
```

Dependencies (auto-installed): Adafruit GFX + Adafruit ST7735/ST7789.

## Using it

* **Scope mode**: green phosphor trace on a graticule, `ms/DIV` derived from the real sample rate,
  top bar shows volts/div + timebase, bottom bar shows input mode, frequency, Vpp and TRIG'D blink.
* **Probe**: connect a **0–3.3 V** signal to **GPIO 34** — the scope auto-switches from DEMO to
  probe mode (see the safety note above).
* **Touch anywhere** → settings window (timebase, phosphor persistence, demo waveform, grid).
  First use runs a 3-point calibration (touch the crosshairs); it is stored in NVS.
* **BOOT button** mirrors menu navigation (short = next row, long = change) as a fallback.

## Credits

* Touch sequence + ESP32-024 pin facts: [CheshirCa/CYD-Display-Library-for-PureBasic](https://github.com/CheshirCa/CYD-Display-Library-for-PureBasic)
* Variant documentation: [Keralots/BambuHelper](https://github.com/Keralots/BambuHelper),
  [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

## License

MIT — see [LICENSE](LICENSE).
