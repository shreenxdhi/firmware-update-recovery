#ifndef FW_H
#define FW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PAGE_SIZE = 256, SLOT_SIZE = 4096, IMAGE_HEADER = 32,
    SLOT_A = 512, SLOT_B = SLOT_A + SLOT_SIZE,
    FLASH_SIZE = SLOT_B + SLOT_SIZE, META_BYTES = 64
};
#define TARGET_ID UINT32_C(0x4d334142)
#define NO_SLOT UINT32_MAX
#define IMAGE_MAGIC UINT32_C(0x50555746)
#define META_MAGIC UINT32_C(0x544f4f42)
#define META_COMMIT UINT32_C(0x54494d43)

typedef struct {
    int fd;
    uint32_t operations, cut_at, fail_at, tear_bytes;
} flash_t;
typedef struct { uint32_t version, length, crc; } image_t;
typedef struct {
    uint64_t generation;
    uint32_t active, pending, attempted;
    image_t confirmed, candidate;
    unsigned page;
} metadata_t;

uint32_t crc32_bytes(const uint8_t *data, size_t length);
uint32_t get32(const uint8_t *p);
void put32(uint8_t *p, uint32_t value);
bool flash_create(const char *path);
bool flash_open(flash_t *f, const char *path, uint32_t cut_at, uint32_t tear);
void flash_close(flash_t *f);
bool flash_read(const flash_t *f, uint32_t offset, void *out, size_t length);
bool flash_program(flash_t *f, uint32_t offset, const uint8_t word[4]);
bool flash_erase(flash_t *f, uint32_t page);
bool image_decode(const uint8_t header[IMAGE_HEADER], image_t *out);
bool image_valid(const flash_t *f, uint32_t slot, image_t *out);
bool image_equal(const image_t *a, const image_t *b);
bool metadata_decode(const uint8_t raw[META_BYTES], metadata_t *out);
void metadata_encode(const metadata_t *m, uint8_t raw[META_BYTES]);
bool metadata_load(const flash_t *f, metadata_t *out);
bool metadata_save(flash_t *f, metadata_t *m, bool initial);
uint32_t slot_offset(uint32_t slot);
#endif
