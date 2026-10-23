# Storage format

All serialized multi-byte integers are little endian. Files contain explicit
bytes, not native C structures. CRC is CRC-32/ISO-HDLC: reflected polynomial
0xEDB88320, initial/final XOR 0xFFFFFFFF, check value 0xCBF43926 for `123456789`.
Python uses zlib.crc32 as an independent implementation.

## Flash map

| Byte range | Size | Use |
|---|---|---|
| 0..255 | 256 | Metadata page 0 |
| 256..511 | 256 | Metadata page 1 |
| 512..4607 | 4,096 | Image slot A, index 0 |
| 4608..8703 | 4,096 | Image slot B, index 1 |

Erase page size is 256 bytes. Program unit is four bytes and must be aligned.
Erased bytes are 0xFF. Reprogramming cannot change a zero bit back to one.

## Image header

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | Magic 0x50555746, `FWUP` |
| 4 | 4 | Format 1 |
| 8 | 4 | Target ID 0x4D334142 |
| 12 | 4 | Nonzero image version |
| 16 | 4 | Payload length, 1..4,064 |
| 20 | 4 | Payload CRC |
| 24 | 4 | Header CRC over bytes 0..23 |
| 28 | 4 | Reserved, zero |
| 32 | length | Payload |

Package file length must be exactly 32 + payload length. Slot programming pads
the final payload word with 0xFF; padding is not part of payload CRC. The full slot
is erased before every installation so a shorter image cannot expose a previous
tail as part of the new payload.

The tuple (version, length, payload CRC) binds the metadata reference to the
stored image. CRC identity detects accidental mismatch; it is not a collision-
resistant cryptographic identity. Both header and payload are verified before
staging metadata is committed and again before selection/confirmation.

## Metadata record

The record occupies the first 64 bytes of its page; unused page bytes remain
erased. The CRC covers bytes 0..51.

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | Magic 0x544F4F42, `BOOT` |
| 4 | 4 | Format 1 |
| 8 | 8 | Nonzero generation |
| 16 | 4 | Confirmed slot, 0 or 1 |
| 20 | 4 | Confirmed version |
| 24 | 4 | Confirmed payload CRC |
| 28 | 4 | Confirmed payload length |
| 32 | 4 | Pending slot; 0xFFFFFFFF means none |
| 36 | 4 | Pending version |
| 40 | 4 | Pending payload CRC |
| 44 | 4 | Pending payload length |
| 48 | 4 | Attempted flag, 0 or 1 |
| 52 | 4 | Record CRC |
| 56 | 4 | Commit word 0x54494D43, `CMIT` |
| 60 | 4 | Reserved, 0xFFFFFFFF |

With no pending slot, pending identity and attempted must be zero. A pending slot
must differ from the confirmed slot, have valid bounds, and carry a newer version.
CRC-valid but semantically invalid records are rejected.

## Command output

Selection responses contain result, decision, reason, slot, version, length,
payload_crc, generation, and operations. Recovery has no selected slot. `status`
reports metadata validity, generation, active/pending state and each slot's
independent image validity. It does not mutate storage.

The operations count covers successful modeled mutation attempts within a
command. Validation failures return zero operations. A cut command exits before
producing a normal result; its operation number and tear prefix are supplied by
the harness and retained in the campaign report.
