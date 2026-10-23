import struct
import zlib

TARGET_ID = 0x4D334142
PAGE_SIZE, SLOT_SIZE, IMAGE_HEADER = 256, 4096, 32
SLOT_OFFSETS = (512, 4608)
FLASH_SIZE = 8704


def package(payload, version, target=TARGET_ID):
    if not 0 < version <= 0xFFFFFFFF or not 0 < len(payload) <= SLOT_SIZE - IMAGE_HEADER:
        raise ValueError("invalid version or payload length")
    head = struct.pack("<6I", 0x50555746, 1, target, version, len(payload), zlib.crc32(payload))
    return head + struct.pack("<2I", zlib.crc32(head), 0) + payload


def parse(data):
    if len(data) < IMAGE_HEADER:
        raise ValueError("short header")
    magic, fmt, target, version, size, crc, header_crc, reserved = struct.unpack("<8I", data[:32])
    if magic != 0x50555746 or fmt != 1 or target != TARGET_ID or reserved:
        raise ValueError("invalid header")
    if not version or not 0 < size <= SLOT_SIZE - IMAGE_HEADER:
        raise ValueError("invalid version/length")
    if zlib.crc32(data[:24]) != header_crc or len(data) < IMAGE_HEADER + size:
        raise ValueError("header CRC or bounds")
    payload = data[32:32 + size]
    if zlib.crc32(payload) != crc:
        raise ValueError("payload CRC")
    return {"version": version, "length": size, "payload_crc": crc}, payload
