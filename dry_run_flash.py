import os
import time

# Configuration — relative to project root
FIRMWARE_PATH = os.path.join(os.path.dirname(__file__), ".pio", "build", "esp32cam", "firmware.bin")
CHUNK_SIZE = 4096  # 4KB chunk size as discussed in discussion.md
APP0_OFFSET = 0x10000

def mock_iram_flash_process(file_path):
    print(f"[*] Starting Dry Run for Firmware: {file_path}")
    
    if not os.path.exists(file_path):
        print(f"[!] Error: File not found at {file_path}")
        return

    total_size = os.path.getsize(file_path)
    print(f"[*] Total Size: {total_size} bytes")
    print(f"[*] App Partition Start Address: 0x{APP0_OFFSET:X}")
    print("-" * 50)

    with open(file_path, "rb") as f:
        bytes_written = 0
        chunk_index = 0
        
        while True:
            # 1. Simulate reading from PSRAM (File) -> SRAM (Buffer)
            # In the real scenario, PSRAM is accessible here because cache is ON
            print(f"[Loop {chunk_index}] Cache Status: ENABLED")
            print(f"   -> Reading 4KB chunk from PSRAM (File offset {bytes_written})...")
            
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            
            current_chunk_size = len(chunk)
            
            # 2. Simulate Disabling Cache
            # Real code: Cache_Read_Disable(0);
            print(f"   -> [CRITICAL] Disabling Flash Cache...")
            
            # 3. Simulate Flashing using ROM functions
            # Real code: esp_rom_spiflash_erase_sector(...) / esp_rom_spiflash_write(...)
            target_address = APP0_OFFSET + bytes_written
            print(f"   -> [ROM] Erasing/Writing {current_chunk_size} bytes to Flash Address 0x{target_address:X}")
            
            # 4. Simulate Enabling Cache
            # Real code: Cache_Read_Enable(0);
            print(f"   -> [CRITICAL] Re-enabling Flash Cache...")
            
            bytes_written += current_chunk_size
            chunk_index += 1
            
            # Limit output for brevity if file is huge, but here it is small enough (250KB / 4KB ~ 62 chunks)
            # Let's just print a progress bar vibe every 10 chunks to not spam too much if it gets larger
            if chunk_index % 10 == 0:
                 print(f"   ... Progress: {bytes_written}/{total_size} bytes ({ (bytes_written/total_size)*100:.1f}%)")

            print("-" * 20)

    print(f"[*] Flashing Complete. Total bytes written: {bytes_written}")

if __name__ == "__main__":
    mock_iram_flash_process(FIRMWARE_PATH)
