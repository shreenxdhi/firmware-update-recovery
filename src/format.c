#include "fw.h"
#include <string.h>

uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
uint32_t crc32_bytes(const uint8_t *data, size_t length) {
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<length;++i) {
        crc^=data[i];
        for (unsigned j=0;j<8;++j) crc=(crc>>1)^((0u-(crc&1u))&UINT32_C(0xedb88320));
    }
    return ~crc;
}
uint32_t slot_offset(uint32_t slot) { return slot==0 ? SLOT_A : SLOT_B; }
bool image_equal(const image_t *a, const image_t *b) {
    return a->version==b->version && a->length==b->length && a->crc==b->crc;
}
bool image_decode(const uint8_t h[IMAGE_HEADER], image_t *out) {
    if (get32(h)!=IMAGE_MAGIC || get32(h+4)!=1 || get32(h+8)!=TARGET_ID ||
        get32(h+24)!=crc32_bytes(h,24) || get32(h+28)!=0) return false;
    image_t image={get32(h+12),get32(h+16),get32(h+20)};
    if (!image.version || !image.length || image.length>SLOT_SIZE-IMAGE_HEADER) return false;
    *out=image; return true;
}
bool image_valid(const flash_t *f, uint32_t slot, image_t *out) {
    if (slot>1) return false;
    uint8_t data[SLOT_SIZE]; image_t image;
    if (!flash_read(f,slot_offset(slot),data,sizeof data) || !image_decode(data,&image)) return false;
    if (crc32_bytes(data+IMAGE_HEADER,image.length)!=image.crc) return false;
    *out=image; return true;
}
static bool image_info_valid(const image_t *i) {
    return i->version && i->length && i->length<=SLOT_SIZE-IMAGE_HEADER;
}
void metadata_encode(const metadata_t *m, uint8_t raw[META_BYTES]) {
    memset(raw,0xff,META_BYTES);
    put32(raw,META_MAGIC);put32(raw+4,1);
    put32(raw+8,(uint32_t)m->generation);put32(raw+12,(uint32_t)(m->generation>>32));
    put32(raw+16,m->active);put32(raw+20,m->confirmed.version);
    put32(raw+24,m->confirmed.crc);put32(raw+28,m->confirmed.length);
    put32(raw+32,m->pending);put32(raw+36,m->candidate.version);
    put32(raw+40,m->candidate.crc);put32(raw+44,m->candidate.length);
    put32(raw+48,m->attempted);put32(raw+52,crc32_bytes(raw,52));
    put32(raw+56,META_COMMIT);
}
bool metadata_decode(const uint8_t raw[META_BYTES], metadata_t *out) {
    if (get32(raw)!=META_MAGIC || get32(raw+4)!=1 || get32(raw+56)!=META_COMMIT ||
        get32(raw+52)!=crc32_bytes(raw,52) || get32(raw+60)!=UINT32_MAX) return false;
    metadata_t m={0};
    m.generation=(uint64_t)get32(raw+8)|((uint64_t)get32(raw+12)<<32);
    m.active=get32(raw+16);m.confirmed=(image_t){get32(raw+20),get32(raw+28),get32(raw+24)};
    m.pending=get32(raw+32);m.candidate=(image_t){get32(raw+36),get32(raw+44),get32(raw+40)};
    m.attempted=get32(raw+48);
    if (!m.generation || m.active>1 || !image_info_valid(&m.confirmed) || m.attempted>1) return false;
    if (m.pending==NO_SLOT) {
        if (m.attempted || m.candidate.version || m.candidate.length || m.candidate.crc) return false;
    } else if (m.pending>1 || m.pending==m.active || !image_info_valid(&m.candidate) ||
               m.candidate.version<=m.confirmed.version) return false;
    *out=m;return true;
}
bool metadata_load(const flash_t *f, metadata_t *out) {
    metadata_t m[2];bool valid[2];uint8_t raw[2][META_BYTES];
    for (unsigned p=0;p<2;++p) {
        if (!flash_read(f,p*PAGE_SIZE,raw[p],META_BYTES)) return false;
        valid[p]=metadata_decode(raw[p],&m[p]);m[p].page=p;
    }
    if (!valid[0] && !valid[1]) return false;
    if (valid[0] && valid[1] && m[0].generation==m[1].generation) return false;
    unsigned chosen=valid[1] && (!valid[0] || m[1].generation>m[0].generation) ? 1u:0u;
    *out=m[chosen];return true;
}
bool metadata_save(flash_t *f, metadata_t *m, bool initial) {
    if (m->generation==UINT64_MAX) return false;
    metadata_t next=*m;
    next.generation++;next.page=initial ? 0u:1u-m->page;
    uint8_t raw[META_BYTES];metadata_encode(&next,raw);
    if (!flash_erase(f,next.page)) return false;
    uint32_t base=next.page*PAGE_SIZE;
    for (uint32_t i=0;i<56;i+=4) if (!flash_program(f,base+i,raw+i)) return false;
    if (!flash_program(f,base+56,raw+56)) return false;
    metadata_t readback;
    if (!metadata_load(f,&readback) || readback.generation!=next.generation || readback.page!=next.page) return false;
    *m=readback;return true;
}
