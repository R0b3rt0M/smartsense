#!/usr/bin/env python3
"""
SmartSense Lab - Zigbee OTA Image Builder

Genera un container Zigbee OTA con:
    [OTA Header: 56 byte]
    [Upgrade Image Element Header: 6 byte]
    [Firmware binary]

Specifica di riferimento:
    Zigbee OTA Upgrade Cluster - Document 09-5499

Uso:
    python tools/build_ota.py firmware.bin firmware.ota
    python tools/build_ota.py firmware.bin firmware.ota 3

Nota:
    Il firmware sorgente deve essere il binario atteso dal bootloader
    e dal client OTA del dispositivo destinatario. La generazione del
    container non garantisce, da sola, la compatibilità del payload.
"""

import argparse
import hashlib
import os
import struct
import sys
import tempfile
from pathlib import Path


# ============================================================
# SmartSense Lab - Identificativi Zigbee
# ============================================================

MANUFACTURER_CODE = 0x1234
IMAGE_TYPE = 0x0001

OTA_FILE_IDENTIFIER = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_HEADER_LENGTH = 56
OTA_FIELD_CONTROL = 0x0000
ZIGBEE_STACK_VERSION = 0x0002

UPGRADE_IMAGE_TAG = 0x0000
HEADER_STRING = b"SmartSense_v1.0.0".ljust(32, b"\x00")

# Little-endian: tutti i campi previsti dall'header OTA standard.
OTA_HEADER_FORMAT = "<IHHHHHIH32sI"
OTA_ELEMENT_FORMAT = "<HI"

UINT16_MAX = 0xFFFF
UINT32_MAX = 0xFFFFFFFF


def validate_configuration(file_version: int) -> None:
    """Verifica i parametri che devono essere rappresentabili nell'header."""

    if not 1 <= file_version <= UINT32_MAX:
        raise ValueError(
            f"Versione OTA non valida: {file_version}. "
            f"Usare un intero tra 1 e {UINT32_MAX}."
        )

    if not 0 <= MANUFACTURER_CODE <= UINT16_MAX:
        raise ValueError("MANUFACTURER_CODE deve essere un intero a 16 bit.")

    if not 0 <= IMAGE_TYPE <= UINT16_MAX:
        raise ValueError("IMAGE_TYPE deve essere un intero a 16 bit.")

    if len(HEADER_STRING) != 32:
        raise ValueError("HEADER_STRING deve essere esattamente di 32 byte.")

    actual_header_size = struct.calcsize(OTA_HEADER_FORMAT)
    if actual_header_size != OTA_HEADER_LENGTH:
        raise RuntimeError(
            f"Formato header non valido: {actual_header_size} byte "
            f"invece di {OTA_HEADER_LENGTH}."
        )

    if struct.calcsize(OTA_ELEMENT_FORMAT) != 6:
        raise RuntimeError("Il formato dell'element header deve essere di 6 byte.")


def build_ota_header(file_version: int, total_file_length: int) -> bytes:
    """Costruisce l'header Zigbee OTA standard di 56 byte."""

    return struct.pack(
        OTA_HEADER_FORMAT,
        OTA_FILE_IDENTIFIER,
        OTA_HEADER_VERSION,
        OTA_HEADER_LENGTH,
        OTA_FIELD_CONTROL,
        MANUFACTURER_CODE,
        IMAGE_TYPE,
        file_version,
        ZIGBEE_STACK_VERSION,
        HEADER_STRING,
        total_file_length,
    )


def make_zigbee_ota(
    input_bin_path: str,
    output_ota_path: str,
    file_version: int = 2,
) -> None:
    """Genera e verifica il container OTA."""

    input_path = Path(input_bin_path).expanduser().resolve()
    output_path = Path(output_ota_path).expanduser().resolve()

    validate_configuration(file_version)

    if not input_path.is_file():
        raise FileNotFoundError(f"Firmware sorgente non trovato: {input_path}")

    if input_path == output_path:
        raise ValueError("Il file OTA non può sovrascrivere il firmware sorgente.")

    with input_path.open("rb") as firmware_file:
        firmware_bytes = firmware_file.read()

    firmware_length = len(firmware_bytes)

    if firmware_length == 0:
        raise ValueError("Il firmware sorgente è vuoto.")

    if firmware_length > UINT32_MAX:
        raise ValueError("Il firmware supera la dimensione massima del campo OTA.")

    element_header = struct.pack(
        OTA_ELEMENT_FORMAT,
        UPGRADE_IMAGE_TAG,
        firmware_length,
    )

    total_file_length = OTA_HEADER_LENGTH + len(element_header) + firmware_length

    if total_file_length > UINT32_MAX:
        raise ValueError("Il container OTA supera la dimensione rappresentabile.")

    ota_header = build_ota_header(file_version, total_file_length)

    if len(ota_header) != OTA_HEADER_LENGTH:
        raise RuntimeError("Dimensione dell'header OTA non valida.")

    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Scrittura temporanea e sostituzione finale per evitare di lasciare
    # un container parziale se la scrittura viene interrotta.
    temp_path = None

    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            dir=str(output_path.parent),
            delete=False,
        ) as ota_file:
            temp_path = Path(ota_file.name)
            ota_file.write(ota_header)
            ota_file.write(element_header)
            ota_file.write(firmware_bytes)
            ota_file.flush()
            os.fsync(ota_file.fileno())

        # Verifica strutturale del file appena scritto.
        with temp_path.open("rb") as ota_file:
            written_header = ota_file.read(OTA_HEADER_LENGTH)
            written_element = ota_file.read(6)
            written_firmware = ota_file.read()

        unpacked_header = struct.unpack(OTA_HEADER_FORMAT, written_header)
        unpacked_element = struct.unpack(OTA_ELEMENT_FORMAT, written_element)

        if unpacked_header[0] != OTA_FILE_IDENTIFIER:
            raise RuntimeError("Magic number OTA non valido.")

        if unpacked_header[2] != OTA_HEADER_LENGTH:
            raise RuntimeError("Header Length non valido.")

        if unpacked_header[6] != file_version:
            raise RuntimeError("Versione OTA non coerente.")

        if unpacked_header[9] != total_file_length:
            raise RuntimeError("Total Image Size non coerente.")

        if unpacked_element[0] != UPGRADE_IMAGE_TAG:
            raise RuntimeError("Upgrade Image tag non valido.")

        if unpacked_element[1] != firmware_length:
            raise RuntimeError("Lunghezza dell'elemento non coerente.")

        if written_firmware != firmware_bytes:
            raise RuntimeError("Il payload scritto non coincide con il sorgente.")

        if temp_path.stat().st_size != total_file_length:
            raise RuntimeError("Dimensione finale del container non coerente.")

        os.replace(temp_path, output_path)
        temp_path = None

    finally:
        if temp_path is not None:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass

    payload_sha512 = hashlib.sha512(firmware_bytes).hexdigest()
    ota_sha512 = hashlib.sha512(output_path.read_bytes()).hexdigest()

    print()
    print("=" * 58)
    print(" SmartSense Lab - Zigbee OTA Image Builder")
    print("=" * 58)
    print(f"Firmware sorgente : {input_path}")
    print(f"Container OTA     : {output_path}")
    print(f"Dimensione header : {OTA_HEADER_LENGTH} byte")
    print(f"Element header    : {len(element_header)} byte")
    print(f"Dimensione payload: {firmware_length} byte")
    print(f"Dimensione totale : {output_path.stat().st_size} byte")
    print(f"Manufacturer Code : 0x{MANUFACTURER_CODE:04X}")
    print(f"Image Type        : 0x{IMAGE_TYPE:04X}")
    print(f"File Version      : {file_version}")
    print(f"SHA-512 firmware  : {payload_sha512}")
    print(f"SHA-512 container : {ota_sha512}")
    print("Verifica struttura: OK")
    print("=" * 58)
    print()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Genera un container Zigbee OTA per SmartSense Lab."
    )
    parser.add_argument("firmware_bin", help="Percorso del firmware binario")
    parser.add_argument("output_ota", help="Percorso del container OTA da generare")
    parser.add_argument(
        "file_version",
        nargs="?",
        type=int,
        default=2,
        help="Versione OTA numerica (predefinita: 2; incrementare a ogni release)",
    )

    args = parser.parse_args()

    try:
        make_zigbee_ota(
            args.firmware_bin,
            args.output_ota,
            args.file_version,
        )
    except (OSError, ValueError, struct.error, RuntimeError) as exc:
        print(f"ERRORE: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())