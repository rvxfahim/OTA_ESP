# OTA_ESP — Single-Partition Self-Overwriting OTA for ESP32

A custom **Over-The-Air (OTA)** firmware update system for the **ESP32-CAM**
that uses a **single `factory` partition (~4/8/16 MB)** instead of the standard dual OTA-partition scheme. The firmware downloads a new binary over WiFi, stores it in PSRAM, then overwrites its own flash partition from an IRAM-only routine — no secondary `ota_1` partition, and no wasted flash.

---

## How It Works

```
┌──────────────┐    WiFi (HTTP)      ┌─────────────────┐
│  HTTP server │ ──── firmware.bin ─→│   ESP32-CAM     │
│  (Python)    │                     │                 │
└──────────────┘                     │ 1. Download     │
                                     │    to PSRAM     │
                                     │ 2. Disable      │
                                     │    cache/WiFi   │
                                     │ 3. IRAM loop    │
                                     │    (4 KB chunks)│
                                     │ 4. Reboot       │
                                     └─────────────────┘
```

1. **WiFi Download** — The new firmware binary is fetched via HTTP and buffered
   entirely in **PSRAM** (external 4 MB / 8 MB RAM).
2. **System Teardown** — WiFi radios are stopped, the other CPU core is parked
   in an IRAM-safe loop, the scheduler is suspended, and watchdogs are disabled.
3. **IRAM Flash Routine** — Running entirely from IRAM (Internal RAM), the
   routine copies 4 KB chunks from PSRAM into a DRAM buffer, disables the
   flash cache, erases/writes a sector via **ESP32 silicon-ROM SPI flash
   functions**, re-enables the cache, and repeats.
4. **Reboot** — The watchdog timer is configured for an immediate system reset,
   booting into the newly written firmware.

> ROM functions like `esp_rom_spiflash_erase_sector()` and
> `esp_rom_spiflash_write()` are burned into the ESP32 silicon — they are
> always accessible, even when the external flash cache is disabled.

---

## Project Structure

```
OTA_ESP/
├── src/
│   └── main.cpp                  # Main firmware: WiFi download + IRAM flash routine
├── include/                       # Project headers (currently unused)
├── lib/                           # Private libraries (currently unused)
├── test/                          # Unit-test directory + served firmware target
│   └── firmware.bin              # Place your compiled .bin here for serving
├── max_app_4MB.csv               # Custom single-partition table (~4 MB app)
├── platformio.ini                 # PlatformIO config (esp32cam board)
├── serve_firmware.py             # Minimal HTTP server for OTA delivery
├── dry_run_flash.py              # Simulator — walks through the flash process
├── verify_binary_header.py       # Validates firmware.bin magic byte (0xE9)
├── IRAM_FLASH.md                 # Deep-dive: IRAM approach, risks, constraints
├── LICENSE                       # GNU AGPLv3
└── README.md                     # This file
```

---

## Getting Started

### Prerequisites

- [PlatformIO IDE](https://platformio.org/install) (VS Code extension or CLI)
- An **ESP32-CAM** board (AI-Thinker or compatible)
- Python 3.7+ (for the helper scripts)

### 1. Clone the Repo

```bash
git clone https://github.com/rvxfahim/OTA_ESP.git
cd OTA_ESP
```

### 2. Configure WiFi & Server IP

Edit `src/main.cpp` and update:

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define FIRMWARE_URL "http://<YOUR_PC_IP>:8000/firmware.bin"
```

### 3. Build the Firmware

```bash
pio run
```

The compiled binary will be at `.pio/build/esp32cam/firmware.bin`.

### 4. Flash the Initial Firmware (USB)

```bash
pio run --target upload
```

### 5. Verify the Binary (Optional)

```bash
python verify_binary_header.py
```

### 6. Dry-Run Simulation (Optional)

```bash
python dry_run_flash.py
```

### 7. Serve a New Firmware Over-the-Air

Copy your **updated** `.pio/build/esp32cam/firmware.bin` into the `test/`
directory, then start the HTTP server:

```bash
python serve_firmware.py
```

Power-cycle or reset the ESP32-CAM — it will download and flash the new
firmware automatically.

---

## Partition Table

| Name   | Type | SubType | Offset   | Size     |
|--------|------|---------|----------|----------|
| `nvs`  | data | nvs     | `0x9000` | `0x5000` (20 KB) |
| `app0` | app  | factory | `0x10000`| `0x3F0000` (~4 MB) |

This single-partition layout dedicates the entire remaining flash to the
application, unlike the standard ESP32 OTA scheme that requires `ota_0`,
`ota_1`, and `otadata` partitions.

---

## Python Scripts

| Script | Purpose |
|--------|---------|
| `serve_firmware.py` | Serves `test/firmware.bin` on port 8000 via HTTP |
| `dry_run_flash.py` | Simulates the chunked IRAM flash loop (no hardware needed) |
| `verify_binary_header.py` | Checks the magic byte (`0xE9`) to confirm a valid ESP32 app image |

---

## ⚠️ Risks & Caveats

- **No rollback.** If the flash write is interrupted, the device is bricked.
- **Single partition.** `ota_0`/`ota_1`/`otadata` are not used; the firmware overwrites itself directly.
- **IRAM constraints.** The flash routine runs with interrupts disabled,
  scheduler suspended, and the other core parked. Standard `Serial.print()`
  and `String` are unavailable; only `ets_printf()` and `DRAM_ATTR` strings
  work inside the critical section.
- **PSRAM dependency.** The device must have PSRAM enabled and functional.

For a deeper breakdown, read [`IRAM_FLASH.md`](IRAM_FLASH.md).

---

> ⚠️ A failed write (power loss, crash) will require a reflash via USB. 
> Read `IRAM_FLASH.md` for the full technical rationale and risks.