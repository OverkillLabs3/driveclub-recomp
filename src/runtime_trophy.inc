/* Local trophy state, separate from the game's own save files. */
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define TROPHY_BAD_CONTEXT ((int32_t)0x80551609)
#define TROPHY_BAD_ID ((int32_t)0x8055160A)
#define TROPHY_BROKEN_DATA ((int32_t)0x80551611)
#define TROPHY_IO_ERROR ((int32_t)0x80551600)

typedef struct { int32_t id; char path[1024]; uint64_t ticks[128]; } TrophyContext;
typedef struct { uint64_t size; int32_t id; uint8_t unlocked, reserved[3]; uint64_t timestamp; } TrophyData;
_Static_assert(sizeof(TrophyData)==24,"OrbisNpTrophyData layout");
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static TrophyContext trophy_contexts[8];

static TrophyContext *trophy_find(int32_t id) {
    for (unsigned i=0;i<8;++i) if (trophy_contexts[i].id==id && id) return &trophy_contexts[i];
    return NULL;
}
static int32_t trophy_load(TrophyContext *ctx) {
    FILE *f=fopen(ctx->path,"rb");
    if (!f) return errno==ENOENT ? 0 : TROPHY_IO_ERROR;
    char magic[8];
    int ok=fread(magic,1,8,f)==8 && !memcmp(magic,"BBTRPHY1",8) &&
           fread(ctx->ticks,sizeof(ctx->ticks),1,f)==1 && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    return ok ? 0 : TROPHY_BROKEN_DATA;
}
static int32_t trophy_save(TrophyContext *ctx) {
    char tmp[1032];
    snprintf(tmp,sizeof(tmp),"%s.new",ctx->path);
    for (char *p=tmp+1;*p;++p) if (*p=='/' || *p=='\\') {
        char slash=*p; *p=0;
        int failed=mkdir(tmp,0755) && errno!=EEXIST;
        *p=slash;
        if (failed) return TROPHY_IO_ERROR;
    }
    FILE *f=fopen(tmp,"wb");
    if (!f) return TROPHY_IO_ERROR;
    int ok=fwrite("BBTRPHY1",1,8,f)==8 && fwrite(ctx->ticks,sizeof(ctx->ticks),1,f)==1 &&
           !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok=0;
#ifdef _WIN32
    if (ok) ok=MoveFileExA(tmp,ctx->path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    if (ok) ok=rename(tmp,ctx->path)==0;
#endif
    if (!ok) { remove(tmp); return TROPHY_IO_ERROR; }
    return 0;
}
static ABI int32_t trophy_context(int32_t *id,int32_t user,uint32_t label,uint64_t options) {
    if (!id || options) return TROPHY_INVALID;
    pthread_mutex_lock(&trophy_lock);
    TrophyContext candidate={0};
    int n=snprintf(candidate.path,sizeof(candidate.path),"%s/trophies/%d/%s/%u.bin",
                   runtime_file_user_dir(),user,runtime_savedata_title(),label);
    int32_t result=n<0 || (size_t)n>=sizeof(candidate.path) ? TROPHY_IO_ERROR : 0;
    TrophyContext *slot=NULL;
    for (unsigned i=0;!result && i<8;++i) {
        if (trophy_contexts[i].id && !strcmp(trophy_contexts[i].path,candidate.path)) result=(int32_t)0x80551613;
        if (!trophy_contexts[i].id) slot=&trophy_contexts[i];
    }
    if (!result && !slot) result=(int32_t)0x80551623;
    if (!result) result=trophy_load(&candidate);
    if (!result) { candidate.id=new_id(); *slot=candidate; *id=candidate.id; }
    pthread_mutex_unlock(&trophy_lock);
    return result;
}
static ABI int32_t trophy_destroy_context(int32_t id) {
    pthread_mutex_lock(&trophy_lock);
    TrophyContext *ctx=trophy_find(id);
    int32_t result=ctx ? 0 : TROPHY_BAD_CONTEXT;
    if (ctx) memset(ctx,0,sizeof(*ctx));
    pthread_mutex_unlock(&trophy_lock);
    return result;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_destroy_handle(int32_t handle) { (void)handle; return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { (void)ctx; (void)handle; (void)options; return 0; }
static ABI int32_t trophy_unlock(int32_t id,int32_t handle,int32_t trophy,int32_t *platinum) {
    (void)handle;
    if (platinum) *platinum=-1;
    if (trophy<0 || trophy>=128) return TROPHY_BAD_ID;
    pthread_mutex_lock(&trophy_lock);
    TrophyContext *ctx=trophy_find(id);
    int32_t result=ctx ? 0 : TROPHY_BAD_CONTEXT;
    if (ctx && ctx->ticks[trophy]) result=(int32_t)0x8055160C;
    if (!result) {
        ctx->ticks[trophy]=UINT64_C(0xdcbffeff2bc000)+(uint64_t)time(NULL)*1000000;
        result=trophy_save(ctx);
        if (result) ctx->ticks[trophy]=0;
        else printf("Runtime: trophy %d unlocked (saved locally)\n",trophy);
    }
    pthread_mutex_unlock(&trophy_lock);
    return result;
}
static ABI int32_t trophy_unlock_state(int32_t id,int32_t handle,uint32_t *flags,uint32_t *count) {
    (void)handle;
    if (!flags || !count) return TROPHY_INVALID;
    pthread_mutex_lock(&trophy_lock);
    TrophyContext *ctx=trophy_find(id);
    if (ctx) {
        /* count describes the trophy ID range, not the number unlocked. Without the .trp
         * metadata, expose all 128 supported IDs so sparse DLC awards are still queried. */
        memset(flags,0,16); *count=128;
        for (unsigned i=0;i<128;++i) if (ctx->ticks[i]) flags[i/32]|=1u<<(i%32);
    }
    pthread_mutex_unlock(&trophy_lock);
    return ctx ? 0 : TROPHY_BAD_CONTEXT;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    /* Trophy names, grades and the full title count still require the encrypted .trp metadata. */
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t id,int32_t handle,int32_t trophy,void *details,void *data) {
    (void)handle;
    if (trophy<0 || trophy>=128) return TROPHY_BAD_ID;
    pthread_mutex_lock(&trophy_lock);
    TrophyContext *ctx=trophy_find(id);
    int32_t result=ctx ? 0 : TROPHY_BAD_CONTEXT;
    if (!result && data) {
        uint64_t size; memcpy(&size,data,8);
        if (size!=sizeof(TrophyData)) result=TROPHY_INVALID;
        else { TrophyData value={.size=size,.id=trophy,.unlocked=ctx->ticks[trophy]!=0,.timestamp=ctx->ticks[trophy]}; memcpy(data,&value,sizeof(value)); }
    }
    if (!result && details) {
        uint64_t size; memcpy(&size,details,8);
        if (size<12 || size>=4096) result=TROPHY_INVALID;
        else { trophy_game_info(id,handle,details,NULL); memcpy((char *)details+8,&trophy,4); }
    }
    pthread_mutex_unlock(&trophy_lock);
    return result;
}
