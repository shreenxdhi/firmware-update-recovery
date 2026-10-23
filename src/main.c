#define _POSIX_C_SOURCE 200809L
#include "fw.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *reason,uint32_t operations) {
    printf("{\"result\":\"error\",\"reason\":\"%s\",\"operations\":%" PRIu32 "}\n",reason,operations);
    return 2;
}
static bool number(const char *s,uint32_t *out) {
    if (s[0]<'0' || s[0]>'9') return false;
    errno=0;char *end;unsigned long n=strtoul(s,&end,10);
    if (errno || *end || n>UINT32_MAX) return false;
    *out=(uint32_t)n;return true;
}
static bool package_read(const char *path,uint8_t data[SLOT_SIZE],image_t *image) {
    FILE *file=fopen(path,"rb");if (!file) return false;
    memset(data,0xff,SLOT_SIZE);
    size_t n=fread(data,1,SLOT_SIZE,file);
    bool ok=!ferror(file);
    if (ok && !feof(file)) ok=fgetc(file)==EOF && !ferror(file);
    if (fclose(file)!=0) ok=false;
    return ok && n>=IMAGE_HEADER && image_decode(data,image) &&
           n==IMAGE_HEADER+image->length && crc32_bytes(data+IMAGE_HEADER,image->length)==image->crc;
}
static bool write_image(flash_t *f,uint32_t slot,const uint8_t *data,const image_t *image) {
    uint32_t base=slot_offset(slot);
    for (uint32_t p=0;p<SLOT_SIZE/PAGE_SIZE;++p)
        if (!flash_erase(f,base/PAGE_SIZE+p)) return false;
    uint32_t padded=(image->length+3u)&~UINT32_C(3);
    for (uint32_t i=0;i<padded;i+=4)
        if (!flash_program(f,base+IMAGE_HEADER+i,data+IMAGE_HEADER+i)) return false;
    for (uint32_t i=0;i<IMAGE_HEADER;i+=4)
        if (!flash_program(f,base+i,data+i)) return false;
    image_t check;
    return image_valid(f,slot,&check) && image_equal(&check,image);
}
static bool confirmed_valid(const flash_t *f,const metadata_t *m) {
    image_t image;
    return image_valid(f,m->active,&image) && image_equal(&image,&m->confirmed);
}
static bool candidate_valid(const flash_t *f,const metadata_t *m) {
    image_t image;
    return m->pending!=NO_SLOT && image_valid(f,m->pending,&image) && image_equal(&image,&m->candidate);
}
static void clear_candidate(metadata_t *m) {
    m->pending=NO_SLOT;m->attempted=0;m->candidate=(image_t){0};
}
static void decision(const char *kind,const char *reason,uint32_t slot,
                     const image_t *image,const metadata_t *m,const flash_t *f) {
    printf("{\"result\":\"ok\",\"decision\":\"%s\",\"reason\":\"%s\","
           "\"slot\":%" PRIu32 ",\"version\":%" PRIu32 ",\"length\":%" PRIu32 ","
           "\"payload_crc\":%" PRIu32 ",\"generation\":%" PRIu64 ",\"operations\":%" PRIu32 "}\n",
           kind,reason,slot,image->version,image->length,image->crc,m->generation,f->operations);
}
static int recovery(const char *reason) {
    printf("{\"result\":\"ok\",\"decision\":\"recovery\",\"reason\":\"%s\"}\n",reason);return 0;
}
static int boot(flash_t *f) {
    metadata_t m;
    if (!metadata_load(f,&m)) return recovery("no_valid_metadata");
    if (!confirmed_valid(f,&m)) return recovery("confirmed_image_invalid");
    if (m.pending==NO_SLOT) { decision("confirmed","stable",m.active,&m.confirmed,&m,f);return 0; }
    if (!m.attempted && candidate_valid(f,&m)) {
        m.attempted=1;
        if (!metadata_save(f,&m,false)) return fail("trial_record_failed",f->operations);
        decision("trial","candidate_ready",m.pending,&m.candidate,&m,f);return 0;
    }
    const char *reason=m.attempted ? "unconfirmed_trial":"candidate_invalid";
    clear_candidate(&m);
    if (!metadata_save(f,&m,false)) return fail("rollback_record_failed",f->operations);
    decision("confirmed",reason,m.active,&m.confirmed,&m,f);return 0;
}
static int status(const flash_t *f) {
    metadata_t m;bool found=metadata_load(f,&m);
    printf("{\"result\":\"ok\",\"metadata_valid\":%s",found?"true":"false");
    if (found) {
        printf(",\"generation\":%" PRIu64 ",\"metadata_page\":%u,\"active\":%" PRIu32
               ",\"active_version\":%" PRIu32 ",\"pending\":",m.generation,m.page,m.active,m.confirmed.version);
        if (m.pending==NO_SLOT) printf("null");else printf("%" PRIu32,m.pending);
        printf(",\"pending_version\":%" PRIu32 ",\"attempted\":%s,\"confirmed_valid\":%s,\"candidate_valid\":%s",
               m.candidate.version,m.attempted?"true":"false",confirmed_valid(f,&m)?"true":"false",
               candidate_valid(f,&m)?"true":"false");
    }
    printf(",\"slots\":[");
    for (uint32_t s=0;s<2;++s) {
        image_t image;bool valid=image_valid(f,s,&image);
        printf("%s{\"slot\":%" PRIu32 ",\"valid\":%s",s?",":"",s,valid?"true":"false");
        if (valid) printf(",\"version\":%" PRIu32 ",\"length\":%" PRIu32 ",\"payload_crc\":%" PRIu32,
                          image.version,image.length,image.crc);
        printf("}");
    }
    printf("]}\n");return 0;
}
static bool blank(const flash_t *f) {
    uint8_t page[PAGE_SIZE];
    for (uint32_t offset=0;offset<FLASH_SIZE;offset+=PAGE_SIZE) {
        if (!flash_read(f,offset,page,sizeof page)) return false;
        for (unsigned i=0;i<PAGE_SIZE;++i) if (page[i]!=0xff) return false;
    }
    return true;
}
static int install(flash_t *f,const char *path,bool provision) {
    uint8_t data[SLOT_SIZE];image_t image;metadata_t m={0};
    if (!package_read(path,data,&image)) return fail("invalid_package",f->operations);
    if (provision) {
        if (!blank(f)) return fail("provision_requires_blank_flash",f->operations);
        m.active=0;m.confirmed=image;clear_candidate(&m);
        if (!write_image(f,0,data,&image) || !metadata_save(f,&m,true)) return fail("provision_failed",f->operations);
        decision("confirmed","provisioned",0,&m.confirmed,&m,f);return 0;
    }
    if (!metadata_load(f,&m) || !confirmed_valid(f,&m)) return fail("no_confirmed_image",f->operations);
    if (m.pending!=NO_SLOT) return fail("pending_update_exists",f->operations);
    if (image.version<=m.confirmed.version) return fail("version_not_newer",f->operations);
    if (m.generation==UINT64_MAX) return fail("generation_exhausted",f->operations);
    uint32_t target=1u-m.active;
    if (!write_image(f,target,data,&image)) return fail("image_write_failed",f->operations);
    m.pending=target;m.candidate=image;m.attempted=0;
    if (!metadata_save(f,&m,false)) return fail("stage_record_failed",f->operations);
    decision("staged","candidate_verified",target,&image,&m,f);return 0;
}
static int confirm(flash_t *f,uint32_t slot,uint32_t version) {
    metadata_t m;
    if (!metadata_load(f,&m) || !confirmed_valid(f,&m) || m.pending!=slot ||
        !m.attempted || m.candidate.version!=version || !candidate_valid(f,&m))
        return fail("confirmation_mismatch",f->operations);
    m.active=m.pending;m.confirmed=m.candidate;clear_candidate(&m);
    if (!metadata_save(f,&m,false)) return fail("confirmation_record_failed",f->operations);
    decision("confirmed","application_confirmed",m.active,&m.confirmed,&m,f);return 0;
}
static void usage(void) {
    fputs("fwctl init FLASH\nfwctl provision FLASH IMAGE\nfwctl stage FLASH IMAGE\n"
          "fwctl boot FLASH\nfwctl confirm FLASH SLOT VERSION\nfwctl status FLASH\n"
          "Mutation commands accept --cut-at N or --fail-at N, with optional --tear BYTES; power loss exits 86, I/O failure exits 2.\n",stderr);
}
int main(int argc,char **argv) {
    char *args[8];int count=0;uint32_t cut=0,failed=0,tear=UINT32_MAX;bool has_tear=false;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--cut-at") || !strcmp(argv[i],"--fail-at") || !strcmp(argv[i],"--tear")) {
            bool iscut=!strcmp(argv[i],"--cut-at"), isfail=!strcmp(argv[i],"--fail-at");uint32_t n;
            if (++i>=argc || !number(argv[i],&n) || ((iscut || isfail) && !n)) return fail("invalid_fault_option",0);
            if (iscut) cut=n;else if (isfail) failed=n;else { tear=n;has_tear=true; }
        } else {
            if (count>=8) return fail("too_many_arguments",0);
            args[count++]=argv[i];
        }
    }
    if (count<2 || (has_tear && !cut && !failed) || (cut && failed)) { usage();return 2; }
    const char *command=args[0],*file=args[1];
    if (!strcmp(command,"init") && count==2) {
        if (cut || failed || has_tear) return fail("init_not_fault_injectable",0);
        if (!flash_create(file)) return fail("flash_create_failed",0);
        printf("{\"result\":\"ok\",\"flash_size\":%u}\n",FLASH_SIZE);return 0;
    }
    bool isboot=!strcmp(command,"boot") && count==2;
    bool isstatus=!strcmp(command,"status") && count==2;
    bool isstage=!strcmp(command,"stage") && count==3;
    bool isprovision=!strcmp(command,"provision") && count==3;
    bool isconfirm=!strcmp(command,"confirm") && count==4;
    uint32_t slot=0,version=0;
    if (!(isboot || isstatus || isstage || isprovision || isconfirm) ||
        (isconfirm && (!number(args[2],&slot) || !number(args[3],&version))) || (isstatus && (cut || failed))) {
        usage();return fail("invalid_arguments",0);
    }
    flash_t flash;
    if (!flash_open(&flash,file,cut,tear)) return fail("flash_open_failed",0);
    flash.fail_at=failed;
    int result=isboot ? boot(&flash):isstatus ? status(&flash):
        isstage ? install(&flash,args[2],false):isprovision ? install(&flash,args[2],true):confirm(&flash,slot,version);
    flash_close(&flash);return result;
}
