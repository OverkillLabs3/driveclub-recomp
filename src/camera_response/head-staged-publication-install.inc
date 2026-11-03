static void htp_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static uint64_t htp_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static int htp_install(void){
 if(!htp_requested()||htp_installed||!camera_reference_seek_original)return 0;
 static const unsigned char reset[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb};
 if(!hcl_reset_original&&!htp_reset_original&&(runtime_image_size<0xb91210+sizeof reset||memcmp(image+0xb91210,reset,sizeof reset)))return 0;
 static const unsigned char sig1[]={0x4d,0x8d,0x77,0x30,0x48,0x8d,0xbd,0x30,0xff,0xff,0xff,0x4c,0x89,0xf6};
 static const unsigned char sig2[]={0x49,0x8d,0x9f,0x0,0x2,0x0,0x0,0xc5,0xf8,0x12,0xc0,0xc5,0xfa,0x58,0xc3};
 static const unsigned char sig3[]={0x49,0x8d,0xb7,0x70,0x2,0x0,0x0,0x4c,0x89,0xff,0x48,0x89,0xda};
 if(runtime_image_size<0xe70b13||memcmp(image+0xb921d2,sig1,sizeof sig1)||memcmp(image+0xb9230b,sig2,sizeof sig2)||memcmp(image+0xb9240e,sig3,sizeof sig3)||memcmp(image+0xe70b10,"\x31\xc0\xc3",3)||htp_hash(image+0xb921d2,0x24e)!=UINT64_C(0x36f41fd9f887d8fb)||htp_hash(image+0xb93010,0x500)!=UINT64_C(0x850fd674d76964a5))return 0;
 unsigned char*g=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!g)return 0;
 unsigned char*relay=NULL;uintptr_t origin=((uintptr_t)image+runtime_image_size+65535)&~(uintptr_t)65535;
 for(uintptr_t off=0;off<0x70000000&&!relay;off+=65536){uintptr_t target=origin+off;int64_t rel=(int64_t)target-(int64_t)((uintptr_t)image+0xb9240e +5);if(rel<INT32_MIN||rel>INT32_MAX)break;relay=VirtualAlloc((void*)target,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);}
 if(!relay){VirtualFree(g,0,MEM_RELEASE);return 0;}
 memcpy(g,sig1,14);htp_jump(g+14,(uintptr_t)image+0xb921e0);memcpy(g+128,sig2,15);htp_jump(g+143,(uintptr_t)image+0xb9231a);
 htp_jump(relay,(uintptr_t)htp_bridge3);memcpy(relay+64,sig3,13);htp_jump(relay+77,(uintptr_t)image+0xb9241b);
 DWORD a,b;if(!VirtualProtect(g,4096,PAGE_EXECUTE_READ,&a)||!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&b)){VirtualFree(g,0,MEM_RELEASE);VirtualFree(relay,0,MEM_RELEASE);return 0;}
 DWORD old;if(!VirtualProtect(image+0xb92000,4096,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(g,0,MEM_RELEASE);VirtualFree(relay,0,MEM_RELEASE);return 0;}
 if(!hcl_reset_original&&!htp_reset_original){htp_reset_original=(HtpReset)camera_rate_hook(0xb91210,reset,sizeof reset,(uintptr_t)htp_reset);if(!htp_reset_original){DWORD unused;VirtualProtect(image+0xb92000,4096,old,&unused);VirtualFree(g,0,MEM_RELEASE);VirtualFree(relay,0,MEM_RELEASE);return 0;}}
 htp_original1=(uintptr_t)g;htp_original2=(uintptr_t)g+128;htp_original3=(uintptr_t)relay+64;
 htp_jump(image+0xb921d2,(uintptr_t)htp_bridge1);htp_jump(image+0xb9230b,(uintptr_t)htp_bridge2);image[0xb92319]=0x90;
 image[0xb9240e]=0xe9;int32_t rel=(int32_t)((intptr_t)relay-((intptr_t)image+0xb9240e +5));memcpy(image+0xb9240f,&rel,4);memset(image+0xb92413,0x90,8);
 DWORD unused;VirtualProtect(image+0xb92000,4096,old,&unused);FlushInstructionCache(GetCurrentProcess(),g,4096);FlushInstructionCache(GetCurrentProcess(),relay,4096);FlushInstructionCache(GetCurrentProcess(),image+0xb92000,4096);htp_installed=1;
 puts("HeadGroupPublication: one leased bank, exact current composition, native SDK CALL B9241B unchanged");return 1;
}
