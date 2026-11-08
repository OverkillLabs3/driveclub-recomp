#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../../src/camera_response/head-staged-publication-core.inc"
static unsigned checks;static void ck(int b,const char*s){++checks;if(!b){printf("FAIL %s\n",s);__builtin_trap();}}
int main(void){
 unsigned char actor[0x380],before[0x380],private_actor[0x380],framebuf[0x300],fbefore[0x300],ymm[16][32],ybefore[16][32];
 unsigned char*frame=framebuf+0x200;HtpSelection s={0};HtpTransaction t={0};
 for(unsigned i=0;i<sizeof actor;++i)actor[i]=before[i]=(unsigned char)(i*37+5),private_actor[i]=(unsigned char)(i*19+91);
 memset(framebuf,0x5a,sizeof framebuf);memcpy(fbefore,framebuf,sizeof framebuf);memset(ymm,0xa6,sizeof ymm);memcpy(ybefore,ymm,sizeof ymm);
 s.key=(HtpKey){0x123400,0x200000,0x300000,9,11,33333,42,77,0,0};s.ready=1;
 htp_pack_bank(&s.bank,private_actor);memset(s.pre_frame90,0x11,64);memset(s.pre_base100,0x22,16);memset(s.target130,0x33,16);
 HtpKey k=s.key;s.native30_bypass=1;ck(htp_publish1(&t,&s,&k,actor,frame)==0,"pure30 nooverride");ck(!memcmp(actor,before,sizeof actor)&&!memcmp(framebuf,fbefore,sizeof framebuf),"pure30 all memory unchanged");s.native30_bypass=0;
 HtpKey bad=k;bad.epoch++;ck(htp_publish1(&t,&s,&bad,actor,frame)<0,"stale epoch rejected before writes");ck(!memcmp(actor,before,sizeof actor),"reject no writes");
 ck(htp_publish1(&t,&s,&k,actor,frame)==1,"stage1 selected");
 for(unsigned i=0;i<sizeof actor;++i){int allowed=(i>=0x1a0&&i<0x1e8)||(i>=0x1f0&&i<0x228)||(i>=0x2b0&&i<0x2b4)||(i>=0x350&&i<0x35c);ck(actor[i]==(allowed?private_actor[i]:before[i]),"actor strict whitelist");}
 for(unsigned i=0;i<sizeof framebuf;++i){unsigned at=0x200-i;int a=(at>0x50&&at<=0x90),b=(at>0xf0&&at<=0x100),c=(at>0x120&&at<=0x130);ck(framebuf[i]==(a?0x11:b?0x22:c?0x33:fbefore[i]),"frame strict whitelist");}
 ck(!memcmp(ymm,ybefore,sizeof ymm),"stage1 no register writes");
 ck(htp_publish3(&t,&k,actor)<0,"wrong stage no publication");
 unsigned char snap[0x380];memcpy(snap,actor,sizeof snap);ck(htp_publish2(&t,&bad,actor,ymm)<0,"stage2 bad lease blocked");ck(!memcmp(snap,actor,sizeof actor)&&!memcmp(ymm,ybefore,sizeof ymm),"bad-stage2 no writes");
 ck(htp_publish2(&t,&k,actor,ymm)==1,"stage2 selected");ck(!memcmp(ymm[0],s.bank.controller200,16),"native stage2 XMM0 controller");ck(!memcmp(ymm[1],&s.bank.filtered224,4)&&!memcmp(ymm[2],&s.bank.filtered220,4)&&!memcmp(ymm[3],&s.bank.derived350,4),"native stage2 consumed scalar lanes");
 for(unsigned i=0;i<4;++i)for(unsigned j=16;j<32;++j)ck(ymm[i][j]==0,"native VEX128 upper zero");ck(!memcmp(ymm+4,ybefore+4,12*32),"unrelated YMM unchanged");
 memset(actor+0x1f0,0xee,16);ck(htp_publish3(&t,&k,actor)==1&&!memcmp(actor+0x1f0,s.bank.direction1f0,16),"stage3 selected direction");
 memset(&t,0,sizeof t);k.main_bl=s.key.main_bl=1;ck(htp_publish1(&t,&s,&k,actor,frame)==1&&htp_publish2(&t,&k,actor,ymm)==1,"directBL publication");
 uint32_t want[4]={s.bank.controller200[1],s.bank.controller200[0],s.bank.controller200[0],s.bank.controller200[0]};ck(!memcmp(ymm[1],want,16)&&!memcmp(ymm[2],s.bank.controller200,16),"exact directBL shuffle pattern");
 puts("PASS publication whitelist/stages/pure30/binding/scalar-lanes; runtime reset serialization required separately");printf("checks=%u\n",checks);return 0;
}
