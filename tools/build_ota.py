#!/usr/bin/env python3
import struct
import sys
import hashlib
import os

# Identificativi ufficiali SmartSense Lab
MANUFACTURER_CODE = 0x1234  # ID Vendor SmartSense Lab
IMAGE_TYPE = 0x0001         # ID Modello MultiSensor-ZR

def make_zigbee_ota(input_bin_path, output_ota_path, file_version):
    with open(input_bin_path, 'rb') as f:
        firmware_bytes = f.read()

    file_length = len(firmware_bytes) + 56  # Header ZCL OTA di 56 byte
    sha512 = hashlib.sha512(firmware_bytes).hexdigest()

    # Header Zigbee OTA binario conforme a ZCL
    header = struct.pack(
        '<IHHHHIHIB32sI',
        0x0beef11e,          # Magic Identifier OTA (Upgrade File Identifier)
        0x0100,              # Header Version
        56,                  # Header Length
        0x0000,              # Field Control
        MANUFACTURER_CODE,   # Manufacturer Code (0x1234 -> 4660 dec)
        IMAGE_TYPE,          # Image Type (0x0001)
        file_version,        # File Version progressiva
        0x0000,              # Zigbee Stack Version
        b'SmartSense_v0.5.0\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00',
        file_length          # Total Image Size
    )

    with open(output_ota_path, 'wb') as f:
        f.write(header)
        f.write(firmware_bytes)

    print("\n========================================")
    print(f"File OTA generato: {output_ota_path}")
    print(f"File Size: {os.path.getsize(output_ota_path)} bytes")
    print(f"SHA512: {sha512}")
    print(f"Manufacturer Code: {MANUFACTURER_CODE} (dec: {int(MANUFACTURER_CODE)})")
    print(f"Image Type: {IMAGE_TYPE}")
    print(f"File Version: {file_version}")
    print("========================================\n")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Uso: python build_ota.py <firmware.bin> <output.ota> <file_version_int>")
        print("Esempio: python build_ota.py firmware.bin MultiSensor-ZR_v0.5.0.ota 2")
        sys.exit(1)
    make_zigbee_ota(sys.argv[1], sys.argv[2], int(sys.argv[3]))