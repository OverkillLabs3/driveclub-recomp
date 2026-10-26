#include "../gpu/shim/bbport_diagnostics.h"
/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
/* 64-bit sizes and times; directories are never opened as CRT descriptors. */
typedef struct _stat64 HostStat;
#define host_stat(path,s) _stat64(path,s)
#define host_fstat(fd,s) _fstat64(fd,s)
#define host_lseek _lseeki64
#define O_DIRECTORY 0x40000000 /* bbport marker, removed before _open */
#else
typedef struct stat HostStat;
#define host_stat(path,s) stat(path,s)
#define host_fstat(fd,s) fstat(fd,s)
#define host_lseek lseek
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
typedef struct { int used, host; Listing *dir; size_t position; char path[512]; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

int runtime_file_mount(const char *guest,const char *host) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        pthread_mutex_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { pthread_mutex_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_file_unmount(const char *guest) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    pthread_mutex_unlock(&lock);
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    snprintf(user_root,sizeof(user_root),"%s",user);
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    const char *writable[]={"temp0","download0","data"};
    mkdir(user,0755);
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
}
/* A game file that does not exist is looked for again and again: each path is logged once. */
static void log_missing(const char *guest) {
    static unsigned long long seen[512]; static int count, busy;
    unsigned long long h=1469598103934665603ULL;
    for (const char *p=guest; *p; ++p) h=(h^(unsigned char)*p)*1099511628211ULL;
    while (__sync_lock_test_and_set(&busy,1)) {}
    int known=0;
    for (int i=0;i<count;++i) if (seen[i]==h) { known=1; break; }
    if (!known && count<512) seen[count++]=h;
    __sync_lock_release(&busy);
    if (!known) printf("Runtime: open(%s) -> not found\n",guest);
    /* gameNNN.dat are the archive volumes: a missing one means an incomplete dump and a crash soon after. */
    if (!known && !strncmp(guest,"/app0/game",10) && guest[10]>='0' && guest[10]<='9' && strstr(guest,".dat"))
        printf("Runtime: WARNING archive volume %s is missing from the game folders (incomplete dump?)\n",guest+6);
}
/* A file operation that failed for a reason other than "not found" (a locked file, no permission, a full disk). */
static void log_io_error(const char *what, const char *guest, int err) {
    static volatile long reported;
    if (__sync_fetch_and_add(&reported, 1) < 12)
        printf("Runtime: %s%s%s failed: %s (errno %d)\n", what, guest ? " " : "", guest ? guest : "", strerror(err), err);
}
/* The game looks for loose files that only exist inside its archives (gameNNN.dat): camera and crowd settings,
 * world and livery packs. The PS4 answers those from memory; here each miss cost ~6 host lookups (both mods
 * folders, the update and base folders, a stat and an open), ~0.45 ms on this laptop's NVMe SSD, and loading a
 * car or a menu screen misses hundreds in a row: a run of 354 is a 150 ms frame, a 17-minute session had 13,453
 * misses, and its 30 s windows with misses were the ones with frames over 50 ms. The game folders hold ~140
 * loose files and are read-only, so a path found missing (or a folder found in none of them) stays missing:
 * both are remembered and answered without touching the disk. BB_FILE_MISS_CACHE=0 turns this off. */
#define MISS_SLOTS 16384 /* power of two; a long session misses ~4,000 distinct files in ~250 folders */
enum { MISS_NONE, MISS_ABSENT, MISS_FOLDER };  /* MISS_ABSENT: no file or folder there; MISS_FOLDER: a folder */
typedef struct { uint64_t hash; uint32_t offset, length; uint8_t kind; } MissSlot;
static MissSlot miss_slots[MISS_SLOTS];
static char *miss_names;
static size_t miss_names_size, miss_names_cap, miss_used;
static uint64_t miss_slow, miss_fast, miss_slow_us, folders_absent;
static int miss_cache_on(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("BB_FILE_MISS_CACHE"); v=!(e && e[0]=='0'); }
    return v;
}
static uint64_t miss_hash(const char *key,size_t length) {
    uint64_t h=1469598103934665603ULL;
    for (size_t i=0;i<length;++i) h=(h^(unsigned char)key[i])*1099511628211ULL;
    return h;
}
/* The two below run under `lock`. */
static int miss_find(const char *key,size_t length) {
    const uint64_t h=miss_hash(key,length);
    for (size_t i=h&(MISS_SLOTS-1);miss_slots[i].kind;i=(i+1)&(MISS_SLOTS-1)) {
        const MissSlot *s=&miss_slots[i];
        if (s->hash==h && s->length==length && !memcmp(miss_names+s->offset,key,length)) return s->kind;
    }
    return MISS_NONE;
}
static void miss_add(const char *key,size_t length,int kind) {
    if (miss_used>=MISS_SLOTS/2 || miss_find(key,length)) return;  /* full: the rest just costs disk lookups */
    if (miss_names_size+length>miss_names_cap) {
        size_t cap=miss_names_cap ? miss_names_cap*2 : 65536;
        while (cap<miss_names_size+length) cap*=2;
        char *grown=realloc(miss_names,cap);
        if (!grown) return;
        miss_names=grown; miss_names_cap=cap;
    }
    const uint64_t h=miss_hash(key,length);
    size_t i=h&(MISS_SLOTS-1);
    while (miss_slots[i].kind) i=(i+1)&(MISS_SLOTS-1);
    memcpy(miss_names+miss_names_size,key,length);
    miss_slots[i]=(MissSlot){h,(uint32_t)miss_names_size,(uint32_t)length,(uint8_t)kind};
    miss_names_size+=length; ++miss_used;
}
static void miss_reset(void) {
    pthread_mutex_lock(&lock);
    memset(miss_slots,0,sizeof(miss_slots)); miss_names_size=miss_used=0;
    pthread_mutex_unlock(&lock);
}
static uint64_t file_now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
/* Guest path as translate sees it: relative paths are under /app0. */
static void guest_key(const char *guest,char *buffer,size_t size) {
    if (guest[0]!='/') snprintf(buffer,size,"/app0/%s",guest);
    else snprintf(buffer,size,"%s",guest);
}
/* A game path that does not exist in any game folder (open, stat or access said ENOENT). */
static void note_missing(const char *guest,uint64_t started_us) {
    __atomic_add_fetch(&miss_slow,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&miss_slow_us,file_now_us()-started_us,__ATOMIC_RELAXED);
    if (!guest || !*guest || !miss_cache_on()) return;
    char key[1024];
    guest_key(guest,key,sizeof(key));
    if (strncmp(key,"/app0/",6)) return;
    pthread_mutex_lock(&lock);
    miss_add(key,strlen(key),MISS_ABSENT);
    pthread_mutex_unlock(&lock);
}
void runtime_file_miss_stats(uint64_t *slow,uint64_t *fast,uint64_t *slow_us) {
    *slow=__atomic_load_n(&miss_slow,__ATOMIC_RELAXED);
    *fast=__atomic_load_n(&miss_fast,__ATOMIC_RELAXED);
    *slow_us=__atomic_load_n(&miss_slow_us,__ATOMIC_RELAXED);
}
static char app0_base[600];
void runtime_file_set_base(const char *dir) { snprintf(app0_base,sizeof(app0_base),"%s",dir ? dir : ""); miss_reset(); }
/* Files the player puts in this folder replace the game's own files of the same path (read only). */
#define MODS_MAX 8 /* unlock, graphics detail, car switching, the player's mods folder, benchmark: 4 was too few */
static char mods_dirs[MODS_MAX][600];
static int mods_count;
void runtime_file_add_mods(const char *dir) {
    if (dir && *dir && mods_count<MODS_MAX) snprintf(mods_dirs[mods_count++],sizeof(mods_dirs[0]),"%s",dir);
    else if (dir && *dir) printf("Runtime: too many mods folders, %s ignored\n",dir);
    miss_reset();
}
/* Probe /app0 folder `key` (length bytes) in every overlay. Only ENOENT proves absence:
 * an existing file or another stat error must not poison that path as missing.
 * Runs under `lock`; `host` is the /app0 mount's host folder. */
static int folder_present(const char *key,size_t length,const char *host) {
    const char *roots[MODS_MAX+2]; int n=0;
    int unknown=0;
    for (int i=0;i<mods_count;++i) roots[n++]=mods_dirs[i];
    roots[n++]=host;
    if (app0_base[0]) roots[n++]=app0_base;
    for (int i=0;i<n;++i) {
        char path[1100]; HostStat s;
        if ((size_t)snprintf(path,sizeof(path),"%s%.*s",roots[i],(int)(length-5),key+5)>=sizeof(path)) {
            unknown=1;
        } else if (!host_stat(path,&s)) {
            if (S_ISDIR(s.st_mode)) return MISS_FOLDER;
            unknown=1;
        } else if (errno!=ENOENT) {
            unknown=1;
        }
    }
    return unknown ? MISS_NONE : MISS_ABSENT;
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    guest_key(guest,buffer,sizeof(buffer));
    for (const char *p=buffer;(p=strstr(p,".."));p+=2)
        if ((p==buffer || p[-1]=='/') && (p[2]==0 || p[2]=='/')) return EACCES;
    if (runtime_benchmark_marker(buffer)) return ENOENT; /* benchmark mode progress (runtime_benchmark.c) */
    pthread_mutex_lock(&lock);
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    int result=0;
    if (!m) result=ENOENT;
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    if (!result && best==5 && !strncmp(buffer,"/app0/",6) && miss_cache_on()) {
        /* Known missing, or under a folder no game folder has (see MISS_SLOTS). */
        const size_t length=strlen(buffer);
        int kind=miss_find(buffer,length);
        const char *slash=strrchr(buffer,'/');
        const size_t folder=(size_t)(slash-buffer);
        if (kind!=MISS_ABSENT && folder>5) {
            int above=miss_find(buffer,folder);
            if (!above) {
                above=folder_present(buffer,folder,m->host);
                if (above!=MISS_NONE) miss_add(buffer,folder,above);
                if (above==MISS_ABSENT && folders_absent++<24)
                    printf("Runtime: game folder %.*s is in no game folder on disk (only in the archives): its files are answered from memory\n",
                           (int)folder,buffer);
            }
            if (above==MISS_ABSENT) kind=MISS_ABSENT;
        }
        if (kind==MISS_ABSENT) {
            pthread_mutex_unlock(&lock);
            __atomic_add_fetch(&miss_fast,1,__ATOMIC_RELAXED);
            return ENOENT;
        }
    }
    if (!result && mods_count && !strncmp(buffer,"/app0/",6)) {
        for (int i=0;i<mods_count;++i) {
            char alt[1024];
            if ((size_t)snprintf(alt,sizeof(alt),"%s%s",mods_dirs[i],buffer+5)<sizeof(alt) && access(alt,F_OK)==0) {
                static unsigned used;
                if (used++<40) printf("Runtime: replaced by mods folder: %s\n",buffer);
                snprintf(out,size,"%s",alt);
                pthread_mutex_unlock(&lock);
                return 0;
            }
        }
    }
    /* Files the update does not carry come from the base game folder (runtime_file_set_base). */
    if (!result && app0_base[0] && !strncmp(buffer,"/app0/",6) && access(out,F_OK)!=0) {
        char alt[1024];
        if ((size_t)snprintf(alt,sizeof(alt),"%s%s",app0_base,buffer+5)<sizeof(alt) && access(alt,F_OK)==0)
            snprintf(out,size,"%s",alt);
    }
    pthread_mutex_unlock(&lock);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    return result;
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
#ifdef _WIN32
    r|=O_BINARY;
#endif
    return r|O_CLOEXEC;
}
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
#ifdef _WIN32
        /* MinGW's dirent has no d_type. */
        char child[1100]; HostStat entry;
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        l->types[l->count]=host_stat(child,&entry) ? 0 : S_ISDIR(entry.st_mode) ? 4 : S_ISREG(entry.st_mode) ? 8 : 0;
#else
        unsigned char type=e->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d),e->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
#endif
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const HostStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    /* FreeBSD mode bits: directories rwxr-xr-x, files rw-r--r--. */
    g->mode=(uint16_t)(S_ISDIR(s->st_mode) ? 0040755 : 0100644);
    g->blocks=(s->st_size+511)/512; g->blksize=4096;
    g->atime=(GuestTimespec){s->st_atime,0};
    g->mtime=(GuestTimespec){s->st_mtime,0};
    g->ctime=(GuestTimespec){s->st_ctime,0};
#else
    g->mode=(uint16_t)s->st_mode;
    g->blocks=s->st_blocks; g->blksize=(uint32_t)s->st_blksize;
    g->atime=(GuestTimespec){s->st_atim.tv_sec,s->st_atim.tv_nsec};
    g->mtime=(GuestTimespec){s->st_mtim.tv_sec,s->st_mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->st_ctim.tv_sec,s->st_ctim.tv_nsec};
#endif
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* BB_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=BB_DIAGNOSTIC_ENV("BB_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char path[1024];
    const uint64_t started=file_now_us();
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
#ifdef _WIN32
    /* Windows cannot open a directory as a CRT descriptor: directories are listings only. */
    (void)mode;
    HostStat s;
    Listing *dir=NULL;
    int host=-1;
    const int stat_result=host_stat(path,&s);
    const int stat_error=stat_result ? errno : 0;
    if (!stat_result && S_ISDIR(s.st_mode)) {
        if (flags&3) return -EISDIR;
        if (!(dir=list_directory(path))) return -EACCES;
    } else if (flags&0x20000) {
        e=stat_result && stat_error==ENOENT ? ENOENT : ENOTDIR;
        if (e==ENOENT) { ++missing; log_missing(guest); note_missing(guest,started); }
        return -e;
    } else {
        host=open(path,host_flags(flags)&~O_DIRECTORY,_S_IREAD|_S_IWRITE);
        if (host<0) {
            e=errno;
            if (e==ENOENT) { ++missing; log_missing(guest); note_missing(guest,started); }
            else log_io_error("open",guest,e);
            return -e;
        }
        if (host_fstat(host,&s)) memset(&s,0,sizeof(s));
    }
#else
    int host=open(path,host_flags(flags),mode ? mode : 0644);
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; log_missing(guest); note_missing(guest,started); }
        return -e;
    }
    struct stat s;
    Listing *dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode)) dir=list_directory(path);
#endif
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) { pthread_mutex_unlock(&lock); if (host>=0) close(host); free_listing(dir); return -EMFILE; }
    files[fd]=(File){.used=1,.host=host,.dir=dir};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    if (opens<=300) printf("Runtime: open(%s) -> fd %d\n",guest,fd);
    if (audio_trace() && strstr(guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.st_size);
    const char *mod_trace=BB_DIAGNOSTIC_ENV("BB_MOD_TRACE"), *mod_root=mod_trace ? getenv("BB_MODS_DIR") : NULL;
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (realpath(path,actual) && realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && (actual[n]=='/' || actual[n]=='\\') &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return -EBADF; }
    if (f->host>=0) close(f->host);
    free_listing(f->dir);
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    return 0;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
#ifdef _WIN32
/* The CRT reads and writes at most INT_MAX bytes per call. */
static ssize_t host_read(int h,void *buffer,uint64_t size) { return read(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
static ssize_t host_write(int h,const void *buffer,uint64_t size) { return write(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
#else
#define host_read read
#define host_write write
#endif
/* Pages of the destination may be write-protected for GPU tracking: the kernel's copy then
 * fails with EFAULT instead of faulting to our handler. A user-mode write to each page first
 * goes through the handler, which unprotects it (and records the upcoming write). */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    touch_for_write(buffer,size);
    ssize_t n=host_read(h,buffer,size);
    { static unsigned dat_reads; if (n>=0 && !strncmp(files[fd].path,"/app0/game",10) && (dat_reads++<60 || dat_reads%2000==0))
        printf("Runtime: read %s size %llu -> %lld (#%u)\n",files[fd].path,(unsigned long long)size,(long long)n,dat_reads); }
    if (n==0 && size>0 && !strncmp(files[fd].path,"/app0/game",10)) {
        /* A volume of the game archive that ends early (an incomplete copy of the dump): the game keeps asking. */
        static char warned[4][64]; static unsigned warned_count;
        int seen=0;
        for (unsigned i=0;i<warned_count && i<4;++i) if (!strcmp(warned[i],files[fd].path)) seen=1;
        if (!seen && warned_count<4) {
            snprintf(warned[warned_count++],sizeof warned[0],"%s",files[fd].path);
            printf("Runtime: WARNING %s ends earlier than the game expects: the file is incomplete or damaged. Copy the game files again.\n",files[fd].path);
        }
    }
    if (n<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",fd,(unsigned long long)size,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    touch_for_write(buffer,size);
    ssize_t n=pread(h,buffer,size,offset);
    { static unsigned dat_reads; if (n>=0 && !strncmp(files[fd].path,"/app0/game",10) && (dat_reads++<60 || dat_reads%2000==0))
        printf("Runtime: pread %s @%lld size %llu -> %lld (#%u)\n",files[fd].path,(long long)offset,(unsigned long long)size,(long long)n,dat_reads); }
    if (n<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",fd,(unsigned long long)size,(long long)offset,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=host_write(h,buffer,size);
    if (n<0) { const int e=errno; log_io_error("write",NULL,e); return -e; }
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=pwrite(h,buffer,size,offset);
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    int64_t r=host_lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    int h=host_fd(fd);
    HostStat s;
#ifdef _WIN32
    if (h<0) { /* directory: a listing only, stat the path it was opened with */
        File *f=get(fd); char path[1024];
        if (!f || !f->dir) return -EBADF;
        if (!out) return -EFAULT;
        if (translate(f->path,path,sizeof(path)) || host_stat(path,&s)) return -EBADF;
        convert_stat(&s,out); return 0;
    }
#else
    if (h<0) return -EBADF;
#endif
    if (!out) return -EFAULT;
    if (host_fstat(h,&s)) return -errno;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; HostStat s;
    const uint64_t started=file_now_us();
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (!out) return -EFAULT;
    if (host_stat(path,&s)) {
        e=errno;
        if (e==ENOENT) note_missing(guest,started);
        return -e;
    }
    convert_stat(&s,out); return 0;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
/* sceKernelGetdents (three arguments, no basep) uses the same 8-byte header layout as
   getdirentries: d_fileno (u32), d_reclen (u16), d_type, d_namlen, then the name. The game walks
   the buffer by d_reclen, so any other layout makes it loop forever on the first entry. */
static int64_t do_getdents_orbis(int fd,char *buffer,uint64_t size) {
    return do_getdents(fd,buffer,size,NULL);
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? mkdir(path,mode ? mode : 0755) : op==1 ? rmdir(path) : unlink(path);
    return r ? -errno : 0;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
#ifdef _WIN32
    /* POSIX rename replaces an existing target; the CRT's fails. */
    return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING) ? 0 : -compat_errno_from_win32(GetLastError());
#else
    return rename(a,b) ? -errno : 0;
#endif
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
#ifdef _WIN32
    return _chsize_s(h,length) ? -errno : 0;
#else
    return ftruncate(h,length) ? -errno : 0;
#endif
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
#ifdef _WIN32
    int h=open(path,O_RDWR|O_BINARY);
    if (h<0) return -errno;
    int r=_chsize_s(h,length) ? -errno : 0;
    close(h);
    return r;
#else
    return truncate(path,length) ? -errno : 0;
#endif
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    const uint64_t started=file_now_us();
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
#ifdef _WIN32
    if (!access(path,mode&6)) return 0; /* X_OK is invalid for the CRT */
#else
    if (!access(path,mode&7)) return 0;
#endif
    e=errno;
    if (e==ENOENT) note_missing(guest,started);
    return -e;
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_getdents_orbis(int fd,char *b,uint64_t n) { return sce(do_getdents_orbis(fd,b,n)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_getdents_orbis},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
}

