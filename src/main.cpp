#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_task_wdt.h"
#include "esp_ipc.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "rom/ets_sys.h"
#include "rom/spi_flash.h"
#include "rom/cache.h"
#include "esp_wifi.h"       

// Define Watchdog Key if not available
#ifndef TIMG_WDT_WKEY_VALUE
#define TIMG_WDT_WKEY_VALUE 0x50D83AA1
#endif

// WiFi Credentials
#define WIFI_SSID ""
#define WIFI_PASS ""
// TODO: UPDATE THIS IP ADDRESS TO YOUR PC'S IP
#define FIRMWARE_URL "http://YOUR_SERVER_IP:8000/firmware.bin" 

// Global Buffer in PSRAM
uint8_t *firmware_buffer = NULL;
size_t firmware_len = 0;

// Internal RAM buffer for chunking (4KB sector size)
// specific alignment might be needed for ROM functions
static uint32_t dram_buffer[1024] DRAM_ATTR; 

// External declarations for ROM functions
extern "C" {
    void Cache_Read_Disable_rom(int cpu_num);
    void Cache_Read_Enable_rom(int cpu_num);
    esp_rom_spiflash_result_t esp_rom_spiflash_write(uint32_t dest_addr, const uint32_t *src, int32_t len);
    esp_rom_spiflash_result_t esp_rom_spiflash_erase_sector(uint32_t sector_num);
}

// Pointer to Timer Group 0 & 1
volatile timg_dev_t *timer_group0 = (timg_dev_t *)DR_REG_TIMERGROUP0_BASE;
volatile timg_dev_t *timer_group1 = (timg_dev_t *)DR_REG_TIMERGROUP1_BASE;

volatile bool other_core_parked = false;

static void IRAM_ATTR park_other_core_fn(void *arg) {
    // Disable interrupts on this core (the "other" core)
    portDISABLE_INTERRUPTS();
    other_core_parked = true;
    while (true) { }
}

void IRAM_ATTR iram_flash_operation() {
    volatile timg_dev_t *tg0 = (timg_dev_t *)DR_REG_TIMERGROUP0_BASE;
    volatile timg_dev_t *tg1 = (timg_dev_t *)DR_REG_TIMERGROUP1_BASE;

    // Verify Function Address
    ets_printf("Address of esp_rom_spiflash_erase_sector: %p\n", esp_rom_spiflash_erase_sector);

    if (firmware_buffer == NULL || firmware_len == 0) {
        ets_printf("Error: No firmware buffer or empty!\n");
        return;
    }

    // 1. Configure Timer 0 in Group 0 (Keep alive/debug)
    tg0->hw_timer[0].config.enable = 0;
    tg0->hw_timer[0].config.divider = 80;
    tg0->hw_timer[0].config.increase = 1;
    tg0->hw_timer[0].load_high = 0;
    tg0->hw_timer[0].load_low = 0;
    tg0->hw_timer[0].reload = 1;
    tg0->hw_timer[0].config.enable = 1;

    // Disable both Watchdogs
    tg0->wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    tg0->wdt_config0.en = 0;
    tg0->wdt_wprotect = 0;
    tg1->wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    tg1->wdt_config0.en = 0;
    tg1->wdt_wprotect = 0;

    ets_printf("Entering IRAM flash operation...\n");
    int current_core = xPortGetCoreID();
    
    // Define format strings in DRAM
    static const char msg_start[] DRAM_ATTR = "Starting Flash Write. Size: %u bytes\n";
    static const char msg_prog[] DRAM_ATTR = "Processed %u / %u bytes...\n";
    static const char msg_err_erase[] DRAM_ATTR = "Erase failed at sector %u. Res: %d\n";
    static const char msg_err_write[] DRAM_ATTR = "Write failed at addr %u. Res: %d\n";
    static const char msg_done[] DRAM_ATTR = "Flash Operation Complete! Rebooting...\n";

    ets_printf(msg_start, firmware_len);

    uint32_t start_addr = 0x10000;
    uint32_t offset = 0;
    
    while (offset < firmware_len) {
        // Cache is ENABLED here by default (re-enabled at end of loop)
        
        uint32_t chunk_size = firmware_len - offset;
        if (chunk_size > 4096) chunk_size = 4096;
        
        // Copy to dram_buffer (padded 0xFF)
        // Manual copy to avoid using library memcpy which might be in flash
        uint8_t* dst = (uint8_t*)dram_buffer;
        uint8_t* src = (uint8_t*)(firmware_buffer + offset);
        
        for(uint32_t k=0; k<chunk_size; k++) {
            dst[k] = src[k];
        }
        // Pad rest if needed
        for(uint32_t k=chunk_size; k<4096; k++) {
            dst[k] = 0xFF;
        }
        
        // Disable Cache
        Cache_Read_Disable_rom(current_core);
        
        // Erase Sector
        uint32_t sector_addr = start_addr + offset;
        uint32_t sector_num = sector_addr / 4096;
        int res = esp_rom_spiflash_erase_sector(sector_num);
        if(res != 0) { 
             Cache_Read_Enable_rom(current_core);
             ets_printf(msg_err_erase, sector_num, res);
             break; 
        }

        // Write Sector (always 4096 bytes)
        res = esp_rom_spiflash_write(sector_addr, (uint32_t*)dram_buffer, 4096);
        
        // Enable Cache
        Cache_Read_Enable_rom(current_core);
        
        if(res != 0) { 
            ets_printf(msg_err_write, sector_addr, res);
            break; 
        }
        
        // Progress update every 64KB approx
        if ((offset % 65536) == 0) {
            ets_printf(msg_prog, offset + chunk_size, firmware_len);
        }
        
        offset += 4096; // Always advance by sector size
    }
    
    ets_printf(msg_done);

    // Restart via WDT
    // We use the Timer Group 0 Watchdog to force a System Reset from IRAM
    tg0->wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    tg0->wdt_config0.en = 0;
    
    tg0->wdt_config0.stg0 = 3;             // 3 = System Reset
    tg0->wdt_config0.sys_reset_length = 7; // 3.2us
    tg0->wdt_config0.cpu_reset_length = 7; // 3.2us
    
    tg0->wdt_config1.clk_prescale = 80;    // 1 us ticks (assuming 80MHz APB)
    tg0->wdt_config2 = 100;                // 100 ticks timeout
    
    tg0->wdt_config0.en = 1;
    tg0->wdt_wprotect = 0;

    while(true);
}

void setup(void)
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup started...");

  // 1. Initialize PSRAM
  if (psramInit()) {
      Serial.println("PSRAM Initialized.");
  } else {
      Serial.println("PSRAM Initialization Failed!");
      return; 
  }

  // 2. Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // 3. Download Firmware
  HTTPClient http;
  Serial.printf("Downloading firmware from %s\n", FIRMWARE_URL);
  
  if (http.begin(FIRMWARE_URL)) {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
          int len = http.getSize();
          Serial.printf("Firmware Size: %d bytes\n", len);
          
          if (len > 0) {
              // Allocate buffer in PSRAM
              firmware_buffer = (uint8_t*)ps_malloc(len);
              if (firmware_buffer) {
                   // Read data
                   WiFiClient *stream = http.getStreamPtr();
                   size_t readBytes = stream->readBytes(firmware_buffer, len);
                   firmware_len = readBytes;
                   Serial.printf("Downloaded %d bytes to PSRAM.\n", readBytes);
              } else {
                   Serial.println("Failed to allocate PSRAM buffer!");
              }
          }
      } else {
          Serial.printf("HTTP GET failed, code: %d\n", httpCode);
      }
      http.end();
  } else {
      Serial.println("Unable to connect to server!");
  }

  // Check if download successful
  if (firmware_len == 0 || firmware_buffer == NULL) {
      Serial.println("Download failed. Halted.");
      while(1) delay(100);
  }

  // 4. Disable Radios
  Serial.println("Disabling WiFi and Radios...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  // esp_bt_controller_disable(); // Bluetooth not used/init
  delay(100);

  Serial.println("Preparing for IRAM Flash Operation...");


  // 5. Disable Task WDT
  esp_task_wdt_delete(NULL);

  // 6. Park other core
  int my_core = xPortGetCoreID();
  int other_core = (my_core == 0) ? 1 : 0;
  esp_err_t ipc_ret = esp_ipc_call(other_core, park_other_core_fn, NULL);
  
  uint32_t start_wait = millis();
  while (!other_core_parked && (millis() - start_wait < 100)) {
      delay(1);
  }
  
  // 7. Suspend Scheduler
  vTaskSuspendAll();
  
  // 8. Disable Interrupts
  portDISABLE_INTERRUPTS();

  // 9. Jump to IRAM
  iram_flash_operation();
}

void loop(void)
{
  // Should not be reached
}
