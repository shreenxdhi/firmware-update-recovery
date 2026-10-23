#define _POSIX_C_SOURCE 200809L
#include "fw.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool write_all(int fd, uint32_t offset, const uint8_t *data, size_t n) {
    size_t done=0;
    while (done<n) {
        ssize_t wrote=pwrite(fd,data+done,n-done,(off_t)offset+(off_t)done);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) return false;
        done+=(size_t)wrote;
    }
    return true;
}
bool flash_create(const char *path) {
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
    if (fd<0) return false;
    uint8_t page[PAGE_SIZE];memset(page,0xff,sizeof page);
    bool ok=true;
    for (uint32_t offset=0;offset<FLASH_SIZE && ok;offset+=PAGE_SIZE)
        ok=write_all(fd,offset,page,sizeof page);
    if (ok) ok=fsync(fd)==0;
    if (close(fd)!=0) ok=false;
    return ok;
}
bool flash_open(flash_t *f, const char *path, uint32_t cut_at, uint32_t tear) {
    *f=(flash_t){.fd=-1,.cut_at=cut_at,.tear_bytes=tear};
    int fd=open(path,O_RDWR);
    if (fd<0) return false;
    struct stat s;
    struct flock lock={0};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;
    if (fstat(fd,&s)!=0 || !S_ISREG(s.st_mode) || s.st_size!=FLASH_SIZE ||
        fcntl(fd,F_SETLK,&lock)!=0) { close(fd);return false; }
    f->fd=fd;return true;
}
void flash_close(flash_t *f) { if (f->fd>=0) close(f->fd);f->fd=-1; }
bool flash_read(const flash_t *f,uint32_t offset,void *out,size_t n) {
    if (offset>FLASH_SIZE || n>(size_t)(FLASH_SIZE-offset)) return false;
    size_t done=0;uint8_t *p=out;
    while (done<n) {
        ssize_t got=pread(f->fd,p+done,n-done,(off_t)offset+(off_t)done);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return false;
        done+=(size_t)got;
    }
    return true;
}
static bool mutate(flash_t *f,uint32_t offset,const uint8_t *data,size_t n) {
    f->operations++;
    bool cut=f->cut_at && f->operations==f->cut_at;
    bool failed=f->fail_at && f->operations==f->fail_at;
    size_t amount=(cut || failed) && f->tear_bytes<n ? f->tear_bytes:n;
    if (!write_all(f->fd,offset,data,amount) || fsync(f->fd)!=0) return false;
    if (cut) _exit(86);
    if (failed) { errno=EIO;return false; }
    return true;
}
bool flash_program(flash_t *f,uint32_t offset,const uint8_t word[4]) {
    uint8_t prior[4];
    if (offset%4 || !flash_read(f,offset,prior,sizeof prior)) return false;
    for (unsigned i=0;i<4;++i) if ((prior[i]&word[i])!=word[i]) return false;
    return mutate(f,offset,word,4);
}
bool flash_erase(flash_t *f,uint32_t page) {
    if (page>=FLASH_SIZE/PAGE_SIZE) return false;
    uint8_t erased[PAGE_SIZE];memset(erased,0xff,sizeof erased);
    return mutate(f,page*PAGE_SIZE,erased,sizeof erased);
}
