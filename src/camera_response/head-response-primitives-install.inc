static uint64_t hpr_hash(const unsigned char*p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--)h=(h^*p++)*UINT64_C(1099511628211);return h;}
static void hpr_jump(unsigned char*p,uintptr_t target){memcpy(p,"\xff\x25\0\0\0\0",6);memcpy(p+6,&target,8);}
static int hpr_install(void){
 hpr_group_enabled=hpr_group_requested();
 if(!hpr_group_enabled)return 0;
 static const unsigned char sig0[]={0xc5,0x7a,0x10,0x1d,0x0,0xb9,0x67,0x0,0xc4,0xc1,0x78,0x2e,0xcb,0xc4,0x41,0x78,0x28,0xe3};
 static const unsigned char sig1[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x68};
 static const unsigned char sig2[]={0x48,0x8b,0x80,0x70,0x9,0x0,0x0,0x48,0x8b,0xb8,0x20,0x6,0x0,0x0,0x48,0x8b,0x7};
 static const unsigned char sig3[]={0xc4,0xc1,0x7a,0x10,0x84,0x24,0xb0,0x2,0x0,0x0,0xc4,0xc1,0x7a,0x10,0x8c,0x24,0xb4,0x2,0x0,0x0};
 static const unsigned char sig4[]={0x48,0x8b,0x0,0x48,0x3b,0x45,0xd0,0x75,0xf,0x48,0x83,0xc4,0x68,0x5b};
 if(!(runtime_image_size>0xce058e&&!memcmp(image+0xce03b0,sig0,sizeof sig0)&&!memcmp(image+0xb92a40,sig1,sizeof sig1)&&!memcmp(image+0xb92c95,sig2,sizeof sig2)&&!memcmp(image+0xb929e1,sig3,sizeof sig3)&&!memcmp(image+0xb92fe4,sig4,sizeof sig4)&&hpr_hash(image+0xce03b0,0x1de)==UINT64_C(0xc3778680af268612)&&hpr_hash(image+0xb92a40,0x5c0)==UINT64_C(0xdf2210d524fd6edd)&&!memcmp(image+0xb92f8f,"\xe8\x1c\xd4\x14\0",5))){puts("HeadResponsePrimitives: native boundaries unsupported; no hooks");return 0;}
 unsigned char*g=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!g)return 0;
 /* Same native scalar VMOVSS through savedRAX, exact compare/move tail. */
 g[0]=0x50;g[1]=0x48;g[2]=0xb8;uintptr_t scalar=(uintptr_t)image+0x135bcb8;memcpy(g+3,&scalar,8);memcpy(g+11,"\xc5\x7a\x10\x18\x58",5);memcpy(g+16,sig0+8,10);hpr_jump(g+26,(uintptr_t)image+0xce03c2);
 memcpy(g+128,sig1,17);hpr_jump(g+145,(uintptr_t)image+0xb92a51);memcpy(g+256,sig2,17);hpr_jump(g+273,(uintptr_t)image+0xb92ca6);memcpy(g+384,sig3,20);hpr_jump(g+404,(uintptr_t)image+0xb929f5);
 /* MOV/CMP unchanged. Invert original JNE over an absolute failure jump,
  * then native ADD/POPRBX and unchanged remaining epilogue. No scratch GPR. */
 memcpy(g+512,sig4,7);g[519]=0x74;g[520]=14;hpr_jump(g+521,(uintptr_t)image+0xb92ffc);memcpy(g+535,sig4+9,5);hpr_jump(g+540,(uintptr_t)image+0xb92ff2);
 DWORD old;if(!VirtualProtect(g,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(g,0,MEM_RELEASE);return 0;}
 if(!hpr_group_enabled){VirtualFree(g,0,MEM_RELEASE);return 0;}
 DWORD a,b;if(!VirtualProtect(image+0xb92000,4096,PAGE_EXECUTE_READWRITE,&a)){VirtualFree(g,0,MEM_RELEASE);return 0;}if(!VirtualProtect(image+0xce0000,4096,PAGE_EXECUTE_READWRITE,&b)){DWORD unused;VirtualProtect(image+0xb92000,4096,a,&unused);VirtualFree(g,0,MEM_RELEASE);return 0;}
 hpr_ce_original=(uintptr_t)g;hpr_entry_original=(uintptr_t)g+128;hpr_raw_original=(uintptr_t)g+256;hpr_look_original=(uintptr_t)g+384;hpr_common_original=(uintptr_t)g+512;
 hpr_jump(image+0xce03b0,(uintptr_t)hpr_ce_bridge);memset(image+0xce03be,0x90,4);hpr_jump(image+0xb92a40,(uintptr_t)hpr_entry_bridge);memset(image+0xb92a4e,0x90,3);hpr_jump(image+0xb92c95,(uintptr_t)hpr_raw_bridge);memset(image+0xb92ca3,0x90,3);hpr_jump(image+0xb929e1,(uintptr_t)hpr_look_bridge);memset(image+0xb929ef,0x90,6);hpr_jump(image+0xb92fe4,(uintptr_t)hpr_common_bridge);
 DWORD unused;VirtualProtect(image+0xb92000,4096,a,&unused);VirtualProtect(image+0xce0000,4096,b,&unused);FlushInstructionCache(GetCurrentProcess(),g,576);FlushInstructionCache(GetCurrentProcess(),image+0xb92000,4096);FlushInstructionCache(GetCurrentProcess(),image+0xce0000,4096);
 if(hpr_group_enabled)puts("HeadGroupPrimitives: functional current original HKT+helper inputs, diagnostic files optional");
 puts("HeadResponsePrimitivesV2: opt-in read-only per-helper entry/CE/common-exit, parentBL and direct-service actual200/210; no getters duplicated");return 1;
}
