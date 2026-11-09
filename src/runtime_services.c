/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET; /* the Winsock names the co-op poll below uses */
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#define USER_ID 1
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* Debug: BB_GUEST_PATCH="0xADDR=HEXBYTES,..." writes bytes into guest code before the game uses
 * it (applied when UserService initializes), to try out changes to the eboot without rebuilding. */
static void apply_guest_patches(void) {
    const char *spec=getenv("BB_GUEST_PATCH");
    if (!spec || !*spec) return;
    char buf[1024]; snprintf(buf,sizeof buf,"%s",spec);
    for (char *item=strtok(buf,","); item; item=strtok(NULL,",")) {
        char *eq=strchr(item,'=');
        if (!eq) continue;
        *eq=0;
        uintptr_t addr=(uintptr_t)strtoull(item,NULL,16);
        unsigned char bytes[64]; size_t n=0;
        for (const char *h=eq+1; h[0] && h[1] && n<sizeof bytes; h+=2) {
            char pair[3]={h[0],h[1],0}; bytes[n++]=(unsigned char)strtoul(pair,NULL,16);
        }
#ifdef _WIN32
        DWORD old;
        if (!VirtualProtect((void*)addr,n,PAGE_EXECUTE_READWRITE,&old)) { printf("Guest patch %#llx failed\n",(unsigned long long)addr); continue; }
        memcpy((void*)addr,bytes,n);
        VirtualProtect((void*)addr,n,old,&old);
        FlushInstructionCache(GetCurrentProcess(),(void*)addr,n);
#else
        memcpy((void*)addr,bytes,n);
#endif
        printf("Guest patch: %zu bytes at %#llx\n",n,(unsigned long long)addr);
    }
}

/* Debug: BB_GUEST_TRACE="0xADDR,0xADDR,..." puts an int3 at each guest address and logs the
 * registers whenever one is hit (at most BB_GUEST_TRACE_MAX times each, default 20), then lets the
 * instruction run: restore the byte, single-step it, put the int3 back. Windows only. */
#ifdef _WIN32
#define TRACE_MAX 32
static struct { uintptr_t addr; unsigned char orig; int hits; } traces[TRACE_MAX];
static int trace_count, trace_limit=20;
static __thread int trace_rearm=-1;
/* BB_GUEST_TRACE_DUMP="reg:len" also hex-dumps len bytes at that register on every hit. */
static void trace_dump(const CONTEXT *c) {
    const char *spec=getenv("BB_GUEST_TRACE_DUMP");
    if (!spec || !*spec) return;
    char reg[8]={0}; unsigned len=0;
    if (sscanf(spec,"%7[a-z0-9]:%i",reg,&len)!=2 || !len) return;
    const struct { const char *n; DWORD64 v; } regs[]={{"rax",c->Rax},{"rbx",c->Rbx},{"rcx",c->Rcx},
        {"rdx",c->Rdx},{"rsi",c->Rsi},{"rdi",c->Rdi},{"r12",c->R12},{"r13",c->R13},{"r14",c->R14},
        {"r15",c->R15}};
    DWORD64 at=0;
    for (unsigned i=0;i<sizeof regs/sizeof regs[0];++i) if (!strcmp(regs[i].n,reg)) at=regs[i].v;
    MEMORY_BASIC_INFORMATION mi;
    if (!at || !VirtualQuery((void*)at,&mi,sizeof mi) || mi.State!=MEM_COMMIT) return;
    const unsigned char *p=(const unsigned char*)at;
    for (unsigned o=0;o<len;o+=16) {
        printf("DUMP %#llx+%03x:",(unsigned long long)at,o);
        for (unsigned j=0;j<16 && o+j<len;++j) printf(" %02x",p[o+j]);
        printf("\n");
    }
}
static LONG CALLBACK trace_handler(EXCEPTION_POINTERS *e) {
    CONTEXT *c=e->ContextRecord;
    DWORD code=e->ExceptionRecord->ExceptionCode;
    if (code==EXCEPTION_SINGLE_STEP && trace_rearm>=0) {
        DWORD old;
        unsigned char *p=(unsigned char*)traces[trace_rearm].addr;
        VirtualProtect(p,1,PAGE_EXECUTE_READWRITE,&old); *p=0xCC; VirtualProtect(p,1,old,&old);
        FlushInstructionCache(GetCurrentProcess(),p,1);
        trace_rearm=-1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code!=EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at=(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    for (int i=0;i<trace_count;++i) {
        if (traces[i].addr!=at) continue;
        if (traces[i].hits++<trace_limit) {
            printf("TRACE %#llx #%d rdi=%#llx rsi=%#llx rdx=%#llx rcx=%#llx rax=%#llx rbx=%#llx "
                   "r12=%#llx r13=%#llx r14=%#llx r15=%#llx rsp=%#llx ret=%#llx\n",
                   (unsigned long long)at,traces[i].hits,(unsigned long long)c->Rdi,
                   (unsigned long long)c->Rsi,(unsigned long long)c->Rdx,(unsigned long long)c->Rcx,
                   (unsigned long long)c->Rax,(unsigned long long)c->Rbx,(unsigned long long)c->R12,
                   (unsigned long long)c->R13,(unsigned long long)c->R14,(unsigned long long)c->R15,
                   (unsigned long long)c->Rsp,(unsigned long long)*(uint64_t*)c->Rsp);
            trace_dump(c);
            fflush(stdout);
        }
        DWORD old;
        unsigned char *p=(unsigned char*)at;
        VirtualProtect(p,1,PAGE_EXECUTE_READWRITE,&old); *p=traces[i].orig; VirtualProtect(p,1,old,&old);
        FlushInstructionCache(GetCurrentProcess(),p,1);
        c->Rip=at;
        if (traces[i].hits<trace_limit) { c->EFlags|=0x100; trace_rearm=i; } /* trap flag: re-arm after one step */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void install_guest_traces(void) {
    const char *spec=getenv("BB_GUEST_TRACE");
    if (!spec || !*spec) return;
    if (getenv("BB_GUEST_TRACE_MAX")) trace_limit=atoi(getenv("BB_GUEST_TRACE_MAX"));
    AddVectoredExceptionHandler(1,trace_handler);
    char buf[1024]; snprintf(buf,sizeof buf,"%s",spec);
    for (char *item=strtok(buf,","); item && trace_count<TRACE_MAX; item=strtok(NULL,",")) {
        uintptr_t addr=(uintptr_t)strtoull(item,NULL,16);
        DWORD old;
        unsigned char *p=(unsigned char*)addr;
        if (!VirtualProtect(p,1,PAGE_EXECUTE_READWRITE,&old)) continue;
        traces[trace_count].addr=addr; traces[trace_count].orig=*p; traces[trace_count].hits=0;
        *p=0xCC;
        VirtualProtect(p,1,old,&old);
        FlushInstructionCache(GetCurrentProcess(),p,1);
        printf("Guest trace at %#llx\n",(unsigned long long)addr);
        ++trace_count;
    }
}
/* Debug: BB_GUEST_SAMPLE=1 samples every game thread 20 times a second (suspend, read rip and the
 * stack) and every 10 s prints the guest code addresses seen most, as rip and as return addresses
 * on the stack: shows where the game waits. */
#include <tlhelp32.h>
#define GUEST_LO 0x800000000ull
#define GUEST_HI 0x801551ab8ull
#define SAMPLE_SLOTS 4096
static struct { uint64_t addr; unsigned count; } sample_rip[SAMPLE_SLOTS], sample_ret[SAMPLE_SLOTS];
static void sample_add(uint64_t a, int ret) {
    __typeof__(sample_rip[0]) *t=ret ? sample_ret : sample_rip;
    unsigned h=(unsigned)((a>>2)*2654435761u)%SAMPLE_SLOTS;
    for (unsigned i=0;i<SAMPLE_SLOTS;++i) {
        unsigned s=(h+i)%SAMPLE_SLOTS;
        if (t[s].addr==a) { t[s].count++; return; }
        if (!t[s].addr) { t[s].addr=a; t[s].count=1; return; }
    }
}
static void sample_print(int ret) {
    __typeof__(sample_rip[0]) *t=ret ? sample_ret : sample_rip;
    printf("SAMPLE %s top:",ret ? "stack" : "rip");
    for (int k=0;k<25;++k) {
        int best=-1;
        for (int i=0;i<SAMPLE_SLOTS;++i) if (t[i].addr && (best<0 || t[i].count>t[best].count)) best=i;
        if (best<0) break;
        printf(" %#llx:%u",(unsigned long long)t[best].addr,t[best].count);
        t[best].count=0; t[best].addr=0;
    }
    printf("\n");
    memset(t,0,sizeof sample_rip);
    fflush(stdout);
}
static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    const DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    for (int tick=1;;++tick) {
        Sleep(50);
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if (snap==INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te; te.dwSize=sizeof te;
        for (BOOL ok=Thread32First(snap,&te); ok; ok=Thread32Next(snap,&te)) {
            if (te.th32OwnerProcessID!=pid || te.th32ThreadID==self) continue;
            HANDLE th=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th)!=(DWORD)-1) {
                CONTEXT c; c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
                if (GetThreadContext(th,&c)) {
                    if (c.Rip>=GUEST_LO && c.Rip<GUEST_HI) sample_add(c.Rip,0);
                    /* return addresses: guest code pointers in the top of the stack */
                    MEMORY_BASIC_INFORMATION mi;
                    if (VirtualQuery((void*)c.Rsp,&mi,sizeof mi) && mi.State==MEM_COMMIT) {
                        uint64_t end=(uint64_t)mi.BaseAddress+mi.RegionSize;
                        for (uint64_t p=c.Rsp;p+8<=end && p<c.Rsp+0x400;p+=8) {
                            uint64_t v=*(uint64_t*)p;
                            if (v>=GUEST_LO && v<GUEST_HI) sample_add(v,1);
                        }
                    }
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        if (tick%200==0) { sample_print(0); sample_print(1); }
    }
    return 0;
}
static void start_guest_sampler(void) {
    if (!getenv("BB_GUEST_SAMPLE")) return;
    CreateThread(NULL,0,sampler,NULL,0,NULL);
    printf("Guest sampler started\n");
}
#else
static void install_guest_traces(void) {}
static void start_guest_sampler(void) {}
#endif

/* ---- UserService ---- */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) {
    (void)params;
    static int once;
    if (!once) { once=1; apply_guest_patches(); install_guest_traces(); start_guest_sampler(); }
    note("UserService initialized (user 1 logged in)");
    return 0;
}
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *value=getenv("BB_USER_NAME") ? getenv("BB_USER_NAME") : "Player";
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("BB_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
/* Pending system events (co-op: game custom data from friends). A SceSystemServiceEvent is the
 * 32-bit type followed by 8192 bytes of parameters. The game asks for events only while
 * GetStatus reports some. */
#define SYSTEM_EVENT_SIZE (4+8192)
#define SYSTEM_EVENT_MAX 8
static unsigned char system_events[SYSTEM_EVENT_MAX][SYSTEM_EVENT_SIZE];
static int system_event_count;
void runtime_push_system_event(uint32_t type,const void *param,size_t size) {
    pthread_mutex_lock(&lock);
    if (system_event_count<SYSTEM_EVENT_MAX) {
        unsigned char *e=system_events[system_event_count++];
        memset(e,0,SYSTEM_EVENT_SIZE);
        memcpy(e,&type,4);
        memcpy(e+4,param,size<8192 ? size : 8192);
    }
    pthread_mutex_unlock(&lock);
    printf("SystemService: event %#x queued\n",type);
}

/* Co-op: game custom data (private lobby invitations, challenges) that friends sent this player.
 * A console's system announces each item with a GAME_CUSTOM_DATA event; here a thread asks the
 * community server (BB_COOP_SERVER, default 127.0.0.1:8080) every 3 s for new items addressed to
 * BB_NP_LOCAL and raises the event. The game's handler (guest 0x8001583e0) copies the 32-byte
 * parameter: item id (u64), online id (16 chars, terminator, 3 pad), user id. */
#define GAME_CUSTOM_DATA_EVENT 0x10000004u
static int http_get_local(const char *host,int port,const char *path,char *out,size_t size) {
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (s==INVALID_SOCKET) return -1;
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons((u_short)port);
    if (inet_pton(AF_INET,host,&a.sin_addr)!=1 || connect(s,(struct sockaddr*)&a,sizeof a)) { closesocket(s); return -1; }
    char req[512];
    int n=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);
    send(s,req,n,0);
    size_t got=0; int r;
    while (got+1<size && (r=recv(s,out+got,(int)(size-1-got),0))>0) got+=(size_t)r;
    out[got]=0;
    closesocket(s);
    char *body=strstr(out,"\r\n\r\n");
    if (!body) return -1;
    memmove(out,body+4,strlen(body+4)+1);
    return 0;
}
static void *custom_data_poll(void *arg) {
    (void)arg;
    const char *me=getenv("BB_NP_LOCAL");
    const char *server=getenv("BB_COOP_SERVER") ? getenv("BB_COOP_SERVER") : "127.0.0.1:8080";
    char host[64]; int port=8080;
    snprintf(host,sizeof host,"%s",server);
    char *colon=strchr(host,':');
    if (colon) { *colon=0; port=atoi(colon+1); }
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);
#endif
    char path[160], body[8192];
    snprintf(path,sizeof path,"/dccoop/pending/%s",me);
    printf("Co-op: polling %s:%d for game custom data to '%s'\n",host,port,me);
    for (;;) {
#ifdef _WIN32
        Sleep(3000);
#else
        sleep(3);
#endif
        if (http_get_local(host,port,path,body,sizeof body)) continue;
        /* [{"from":"Name","itemId":1000},...] (keys in any order) */
        for (char *obj=strchr(body,'{'); obj; obj=strchr(obj+1,'{')) {
            char *close=strchr(obj,'}');
            if (!close) break;
            *close=0;
            char *p=strstr(obj,"\"itemId\":");
            if (!p) { *close='}'; continue; }
            unsigned char param[32]; memset(param,0,sizeof param);
            uint64_t item=strtoull(p+9,NULL,10);
            memcpy(param,&item,8);
            char from[17]={0};
            char *f=strstr(obj,"\"from\":\"");
            if (f) { f+=8; size_t i=0; while (f[i] && f[i]!='"' && i<16) { from[i]=f[i]; ++i; } }
            *close='}';
            memcpy(param+8,from,16);
            int32_t user=USER_ID; memcpy(param+28,&user,4);
            printf("Co-op: game custom data item %llu from '%s'\n",(unsigned long long)item,from);
            runtime_push_system_event(GAME_CUSTOM_DATA_EVENT,param,sizeof param);
        }
    }
    return NULL;
}
static void start_custom_data_poll(void) {
    static int started;
    if (started) return;
    started=1;
    const char *online=getenv("BB_ONLINE"), *me=getenv("BB_NP_LOCAL");
    if (!online || online[0]!='1' || !me || !*me) return;
    pthread_t t;
    if (!pthread_create(&t,NULL,custom_data_poll,NULL)) pthread_detach(t);
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* no overlay, foreground, normal CPU mode */
    start_custom_data_poll();
    pthread_mutex_lock(&lock);
    int32_t n=system_event_count;
    pthread_mutex_unlock(&lock);
    memcpy(status,&n,4); /* event_num */
    return 0;
}
static ABI int32_t system_event(void *event) {
    if (!event) return SYSTEM_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!system_event_count) { pthread_mutex_unlock(&lock); return SYSTEM_NO_EVENT; }
    memcpy(event,system_events[0],SYSTEM_EVENT_SIZE);
    memmove(system_events[0],system_events[1],(size_t)(--system_event_count)*SYSTEM_EVENT_SIZE);
    pthread_mutex_unlock(&lock);
    printf("SystemService: event %#x delivered (caller %p)\n",*(uint32_t*)event,__builtin_return_address(0));
    return 0;
}
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0},{"MsgDialog",0,0},{"SaveDataDialog",0,0},
                         {"NpProfileDialog",0,0},{"NpCommerceDialog",0,0},{"ImeDialog",0,0}};
static int common_initialized __attribute__((unused));
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(msg,1) DIALOG(save,2) DIALOG(profile,3) DIALOG(commerce,4)
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("BB_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the player name (launcher) or "Player": a controller alone
     * can accept it (the port has no on-screen keyboard). */
    const char *name=getenv("BB_USER_NAME");
    const char *start=initial[0] ? initial : name && *name ? name : "Player";
    if (!bbgpu_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=bbgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

#include "runtime_trophy.inc"

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
/* Many dumps lack sce_sys/playgo-chunk.dat, and the chunk count with it. The game then still asks about the chunks of
 * its tracks and cars (its own map, scripts/playgochunks_id_map.csv): an error for those reads as "downloading" in
 * every menu after the tutorial, which is chunk 0. Without the file every chunk number is installed. */
static int playgo_count_known;
static int playgo_chunk_ok(uint16_t id) { return !playgo_count_known || id<playgo_chunks; }
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1; playgo_count_known=0;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) {
            playgo_chunks=header[10]|header[11]<<8;
            playgo_count_known=playgo_chunks>0;
            if (!playgo_count_known) playgo_chunks=1;
        }
        runtime_file_close(fd);
    }
    if (playgo_count_known) printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    else printf("Runtime: PlayGo initialized; no sce_sys/playgo-chunk.dat in the dump, every chunk reads installed locally\n");
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    static unsigned logged;
    if (logged<20) { ++logged; printf("Runtime: scePlayGoGetLocus %u chunk(s) from %u\n",count,ids[0]); }
    for (uint32_t i=0;i<count;++i) {
        if (!playgo_chunk_ok(ids[i])) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
/* The package is fully installed: nothing to download, nothing queued, no time remaining. */
#define PLAYGO_NOT_INITIALIZED ((int32_t)0x80B20005)
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    return 0;
}
static ABI int32_t playgo_terminate(void) {
    if (!playgo_handle) return PLAYGO_NOT_INITIALIZED;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_language_mask(int32_t handle,uint64_t mask) {
    (void)mask; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE;
}
static ABI int32_t playgo_set_todo(int32_t handle,const void *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    return 0;
}
static ABI int32_t playgo_get_todo(int32_t handle,void *list,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *out=0; return 0;
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0; return 0;
}
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (!playgo_chunk_ok(ids[i])) return PLAYGO_BAD_CHUNK;
    out[0]=1; out[1]=1; /* progressSize == totalSize: complete (non-zero, so no caller divides by zero) */
    return 0;
}
/* libSceDiscMap: every file is read from disk already. */
static ABI int32_t discmap_all_local(const char *path,int64_t offset,int64_t nbytes,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)nbytes;
    if (flags) *flags=0;
    if (r1) *r1=0;
    if (r2) *r2=0;
    return 0;
}
static ABI int32_t discmap_no_bitmap(void) { return (int32_t)0x81100004; }
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) { if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE; if (speed) *speed=2; return 0; }
static ABI int32_t playgo_prefetch(int32_t handle,const void *chunks,uint32_t count,int32_t mode,void *locus) { (void)chunks;(void)count;(void)mode;(void)locus; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyDestroyContext",trophy_destroy_context}, {"sceNpTrophyDestroyHandle",trophy_destroy_handle},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed}, {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoPrefetch",playgo_prefetch},
    {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"scePlayGoSetLanguageMask",playgo_language_mask}, {"scePlayGoSetToDoList",playgo_set_todo},
    {"scePlayGoGetToDoList",playgo_get_todo}, {"scePlayGoGetEta",playgo_eta},
    {"scePlayGoGetProgress",playgo_progress},
    {"Func_7C980FFB0AA27E7A",discmap_all_local}, {"sceDiscMapIsRequestOnHDD",discmap_no_bitmap},
    {"sceDiscMapGetPackageSize",discmap_no_bitmap},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
/* Co-op (BB_ONLINE=1): network and PSN calls go to the shadNet libraries in bbgpu when they
 * implement them. Trophies, voice and the store/profile dialogs stay here. */
static int online_symbol(const char *symbol) {
    static const char *const online[]={"sceNet","sceNp","sceHttp","sceSsl","sceRudp"};
    static const char *const local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    int match=0;
    for (size_t i=0;i<sizeof(online)/sizeof(*online);++i)
        if (!strncmp(symbol,online[i],strlen(online[i]))) match=1;
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i)
        if (!strncmp(symbol,local[i],strlen(local[i]))) match=0;
    return match;
}
uintptr_t runtime_services_resolve(const char *name) {
    static int online=-1;
    if (online<0) { const char *v=getenv("BB_ONLINE"); online=v && v[0]=='1'; }
    if (online) {
        const char *symbol=runtime_symbol(name);
        if (symbol && online_symbol(symbol)) {
            uintptr_t address=bbgpu_resolve(name);
            if (address) return address;
            printf("Co-op: %s has no online version; using the offline one\n",symbol);
        }
    }
    return RUNTIME_LOOKUP(exports,name);
}
