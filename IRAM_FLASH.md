# Custom OTA Flashing Strategy for ESP32 (Single Partition)

This document summarizes the technical discussion regarding implementing a custom "self-overwriting" OTA flashing function for the ESP32 to allow for a maximum application size (approx. 4MB).

## 1. Partition Table Constraints
The current partition scheme in [max_app_4MB.csv](max_app_4MB.csv) uses a single dynamic `factory` partition:
- **Restriction:** Standard libraries like `ElegantOTA` or the Arduino `Update` library will fail because they require at least two OTA partitions (`ota_0`, `ota_1`) and an `otadata` partition for safe rollback.

## 2. The "IRAM Overwrite" Approach
To overwrite the active running partition (`app0`), the flashing function must reside entirely in **IRAM** (Internal RAM).
- **Cache Conflict:** When Flash is being erased or written, the Flash Cache must be disabled. Any attempt by the CPU to fetch instructions from Flash during this time will cause an immediate crash.
- **Solution:** All logic, loops, and called functions must be decorated with `IRAM_ATTR`.

## 3. PSRAM as a Buffer
Using PSRAM to store the incoming binary blob before flashing:
- **The Catch:** PSRAM and Flash share the same MMU/Cache system. Disabling the Flash cache also makes PSRAM inaccessible.
- **Workflow:** 
    1. Download binary to PSRAM.
    2. Start IRAM loop.
    3. Copy a small chunk (e.g., 4KB) from PSRAM to **Internal SRAM** while cache is briefly ON.
    4. Disable Cache.
    5. Write the SRAM chunk to Flash using ROM functions.
    6. Enable Cache and repeat.

## 4. SPI Flash ROM Functions
Standard ESP-IDF or Arduino flash functions reside in Flash and cannot be used. Instead, **Internal ROM functions** (baked into the silicon) must be used:
- `esp_rom_spiflash_erase_sector()`
- `esp_rom_spiflash_write()`
- These functions are always accessible regardless of the Flash cache state.

## 5. Execution Environment Restrictions
Inside the IRAM flashing loop, standard framework features are unavailable:
- **No `Serial.print`:** The `HardwareSerial` code lives in Flash.
- **No `String` literals:** By default, strings are stored in Flash (RODATA).
- **The Alternative:** Use `ets_printf()` from ROM for debugging, and ensure any string constants are decorated with `DRAM_ATTR` to keep them in RAM.

## 6. Primary Risks
- **No Rollback:** If the power fails or the write is interrupted, the device is **bricked** because there is no secondary partition to boot from.
- **Complexity:** This requires low-level management of interrupts and cache, bypassing the safety features of the ESP-IDF OTA component.

## 7. Critical "Hidden" Features to Disable/Manage
Research into `spi_flash_disable_interrupts_caches_and_other_cpu` reveals that simply disabling interrupts is insufficient. The following specific subsystems must be managed:

1.  **Non-IRAM Interrupts**: 
    - Use `esp_intr_noniram_disable()` to mask only interrupts that reside in Flash.
    - **Risk:** Standard `portENTER_CRITICAL()` only masks interrupts on the *current* core and does not disable the cache safely.

2.  **Multi-core Parking (IPC)**:
    - **Requirement:** On dual-core chips, the *other* core shares the same Flash Cache hardware. It **must** be actively parked in an IRAM safe-loop.
    - **Solution:** Use `esp_ipc_call()` to dispatch a high-priority "wait task" to the other core before disabling cache.

3.  **Watchdogs (TWDT & IWDT)**:
    - **Task WDT:** The update loop will strictly block the scheduler (`vTaskSuspendAll`). The TWDT for the current task must be disabled via `esp_task_wdt_delete(NULL)` before starting.
    - **Interrupt WDT:** This hardware timer cannot be easily disabled. The heavy flash write loop must yield (re-enable interrupts/cache) every few milliseconds (e.g., after every 4KB sector) to allow the hardware watchdog to be fed.

4.  **WiFi / Radio Stack**:
    - Must be explicitly stopped via `esp_wifi_stop()` and `esp_bt_controller_disable()` before entering the update mode.
    - **Why:** High-frequency radio interrupts are dangerous, and simply masking them may lead to hardware buffer overflows or inconsistent states if the radio remains active in the background.

5.  **Flash Cache (Low Level)**:
    - **Function:** `Cache_Read_Disable(0)` (and `Cache_Read_Disable(1)` for dual core).
    - **Why:** The hardware cache must be explicitly disconnected from the SPI bus before sending write commands to avoid data corruption.
