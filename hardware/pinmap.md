# Pin Map — Feather ESP32‑S2 + 2.13" mono e‑ink FeatherWing (250×122)

- **SPI**: `SCK`, `MOSI`, `MISO` → Feather hardware SPI
- **E‑ink control**: `EINK_CS=D9`, `EINK_DC=D10`
- **Optional**: `EINK_BUSY=D7` (for precise waits; otherwise library delays)
- **RST**: handled by the Wing’s auto‑reset (also tied to Feather `RESET`), typically no GPIO needed
- **SRAM_CS=D6** (present on Wing; unused initially)
- **SD_CS=D5** (on Wing; microSD, shares the SPI bus with the panel)
- **I²C**: `SDA`, `SCL` → sensors

| Signal     | Pin  | Notes                                 |
|------------|------|---------------------------------------|
| EINK_CS    | D9   | Panel chip select                     |
| EINK_DC    | D10  | Panel data/command                    |
| EINK_BUSY  | D7   | Busy (input), optional                |
| EINK_RST   | —    | Not required (Wing auto‑reset)        |
| SRAM_CS    | D6   | On‑Wing SRAM; not used for v1         |
| SD_CS      | D5   | On‑Wing microSD (`SD_CS_PIN`)         |
| SPI_SCK    | SCK  | SPI clock                             |
| SPI_MOSI   | MOSI | SPI data out                          |
| SPI_MISO   | MISO | Not used by panel                     |
| I2C_SDA    | SDA  | Sensors                               |
| I2C_SCL    | SCL  | Sensors                               |

Wing power note: pulling the Wing’s `EN` pin low cuts power to the panel/SRAM/SD for true low sleep; you must re‑init the display after wake.

microSD note: the card and the panel share the Feather’s hardware SPI bus and are
distinguished only by chip select (panel D9, card D5). Both drivers use SPI
transactions so they coexist, but the card must be mounted after the bus is up —
see `sd_begin()` in `firmware/arduino/src/sd_store.cpp` and
`docs/ALWAYS_ON_AND_OTA.md`.

## GxEPD2 driver class (SSD1680 122×250)

This Wing uses a 2.13" 122×250 panel driven by SSD1680. Start with:

- `GxEPD2_213_B74` (GDEM0213B74)
- or `GxEPD2_213_DEPG0213BN`

Both are 122×250 SSD1680 families. If needed, try other 2.13" 122×250 entries listed in `GxEPD2_display_selection.h` until the HelloWorld example renders correctly (no mirroring, correct resolution). Record which class worked in `README.md`.

Adjust in firmware if your stack or Wing revision differs.
