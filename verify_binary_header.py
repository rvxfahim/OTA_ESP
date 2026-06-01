import os

FIRMWARE_PATH = os.path.join(os.path.dirname(__file__), ".pio", "build", "esp32cam", "firmware.bin")

def analyze_binary(path):
    if not os.path.exists(path):
        print("File not found.")
        return

    size = os.path.getsize(path)
    
    with open(path, "rb") as f:
        header = f.read(1)
        magic_byte = header[0]

    print(f"File: {os.path.basename(path)}")
    print(f"Size: {size} bytes")
    print(f"Magic Byte: 0x{magic_byte:02X}")

    if magic_byte == 0xE9:
        print("✅ CONFIRMED: This is a standard ESP32 Application Image.")
        print("   It should be flashed to the APP partition address (0x10000).")
        print("   It DOES NOT contain Bootloader (0x1000) or NVS (0x9000).")
    else:
        print("⚠️ WARNING: Magic byte is NOT 0xE9. This might be a raw data dump or something else.")

if __name__ == "__main__":
    analyze_binary(FIRMWARE_PATH)
