#define _POSIX_C_SOURCE 200809L
#include "fw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned passed;
static char path[]="build/unit-flash-XXXXXX";
static flash_t flash;
#define RUN(fn) do { fn();++passed;puts("PASS " #fn); } while (0)
static void crc_reference(void) {
    assert(crc32_bytes((const uint8_t *)"123456789",9)==UINT32_C(0xcbf43926));
    assert(crc32_bytes((const uint8_t *)"",0)==0);
}
static void endian_boundaries(void) {
    uint8_t p[4];const uint32_t values[]={0,1,UINT32_MAX,UINT32_C(0x12345678)};
    for (unsigned i=0;i<4;++i) { put32(p,values[i]);assert(get32(p)==values[i]); }
    put32(p,UINT32_C(0x12345678));assert(p[0]==0x78 && p[3]==0x12);
}
static void flash_rules(void) {
    uint8_t read[4],word[4]={0x0f,0xf0,0x55,0xaa},ones[4]={255,255,255,255};
    assert(flash_read(&flash,SLOT_A,read,4) && !memcmp(read,ones,4));
    assert(!flash_program(&flash,SLOT_A+1,word));
    assert(flash_program(&flash,SLOT_A,word));
    assert(!flash_program(&flash,SLOT_A,ones));
    assert(flash_read(&flash,SLOT_A,read,4) && !memcmp(read,word,4));
    assert(!flash_read(&flash,FLASH_SIZE-1,read,4));
    assert(!flash_program(&flash,FLASH_SIZE,word));
    assert(!flash_erase(&flash,FLASH_SIZE/PAGE_SIZE));
    assert(flash_erase(&flash,SLOT_A/PAGE_SIZE));
    assert(flash_read(&flash,SLOT_A,read,4) && !memcmp(read,ones,4));
}
static metadata_t example(void) {
    metadata_t m={0};m.generation=1;m.active=0;m.pending=NO_SLOT;
    m.confirmed=(image_t){1,4,UINT32_C(0x12345678)};return m;
}
static void metadata_roundtrip(void) {
    metadata_t m=example(),out;uint8_t data[META_BYTES];
    metadata_encode(&m,data);assert(metadata_decode(data,&out));
    assert(out.generation==1 && out.active==0 && out.pending==NO_SLOT);
    assert(image_equal(&m.confirmed,&out.confirmed));
    m.pending=1;m.candidate=(image_t){2,17,UINT32_C(0xabcdef01)};m.attempted=1;
    metadata_encode(&m,data);assert(metadata_decode(data,&out));
    assert(out.pending==1 && out.attempted==1 && image_equal(&out.candidate,&m.candidate));
}
static void every_metadata_bit_corruption(void) {
    metadata_t m=example(),out;uint8_t data[META_BYTES];metadata_encode(&m,data);
    for (unsigned i=0;i<META_BYTES;++i) for (unsigned bit=0;bit<8;++bit) {
        data[i]^=(uint8_t)(1u<<bit);assert(!metadata_decode(data,&out));data[i]^=(uint8_t)(1u<<bit);
    }
}
static void metadata_semantic_validation(void) {
    metadata_t m=example(),out;uint8_t data[META_BYTES];
    m.pending=0;m.candidate=(image_t){2,4,0};metadata_encode(&m,data);assert(!metadata_decode(data,&out));
    m.pending=1;m.candidate.version=1;metadata_encode(&m,data);assert(!metadata_decode(data,&out));
    m=example();m.attempted=1;metadata_encode(&m,data);assert(!metadata_decode(data,&out));
    m=example();m.generation=0;metadata_encode(&m,data);assert(!metadata_decode(data,&out));
}
static void metadata_commit_and_fallback(void) {
    metadata_t m=example(),out;m.generation=0;
    assert(metadata_save(&flash,&m,true) && m.page==0 && m.generation==1);
    m.pending=1;m.candidate=(image_t){2,4,42};
    assert(metadata_save(&flash,&m,false) && m.page==1 && m.generation==2);
    const uint8_t zero[4]={0};
    assert(flash_program(&flash,PAGE_SIZE+56,zero));
    assert(metadata_load(&flash,&out) && out.generation==1 && out.pending==NO_SLOT);
}
static void generation_exhaustion(void) {
    metadata_t m=example();m.generation=UINT64_MAX;
    uint32_t before=flash.operations;
    assert(!metadata_save(&flash,&m,false));assert(flash.operations==before);
}
static void header_validation(void) {
    uint8_t h[IMAGE_HEADER]={0};image_t image;
    put32(h,IMAGE_MAGIC);put32(h+4,1);put32(h+8,TARGET_ID);
    put32(h+12,1);put32(h+16,1);put32(h+20,42);put32(h+24,crc32_bytes(h,24));
    assert(image_decode(h,&image) && image.length==1);
    put32(h+16,SLOT_SIZE-IMAGE_HEADER);put32(h+24,crc32_bytes(h,24));assert(image_decode(h,&image));
    put32(h+16,SLOT_SIZE-IMAGE_HEADER+1);put32(h+24,crc32_bytes(h,24));assert(!image_decode(h,&image));
    put32(h+16,0);put32(h+24,crc32_bytes(h,24));assert(!image_decode(h,&image));
}
int main(void) {
    int fd=mkstemp(path);assert(fd>=0);assert(close(fd)==0);assert(unlink(path)==0);
    assert(flash_create(path));assert(!flash_create(path));
    assert(flash_open(&flash,path,0,UINT32_MAX));
    RUN(crc_reference);RUN(endian_boundaries);RUN(flash_rules);RUN(metadata_roundtrip);
    RUN(every_metadata_bit_corruption);RUN(metadata_semantic_validation);
    RUN(metadata_commit_and_fallback);RUN(generation_exhaustion);RUN(header_validation);
    flash_close(&flash);assert(unlink(path)==0);
    printf("%u C test groups passed.\n",passed);return 0;
}
