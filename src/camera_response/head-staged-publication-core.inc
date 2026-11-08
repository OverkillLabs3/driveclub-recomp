/* Bit-exact publication only; NO recurrence, input sampling, coefficient or
 * trajectory policy. Runtime must hold ONE reset-serialized lease across1..3. */
typedef struct {
 unsigned char coupled[64];uint32_t delay1e0,response1e4,direction1f0[4];
 uint32_t controller200[4],channel210[4],filtered220,filtered224;
 uint32_t look2b0,derived350,accum354,accum358;
} HtpBank;
_Static_assert(sizeof(HtpBank)==144,"named temporal bank144");
typedef struct {
 uintptr_t owner,main_frame,clock;uint64_t epoch,binding_token;int64_t Q;
 uint32_t native_frame,thread,domain,main_bl;
} HtpKey;
typedef struct {
 HtpKey key;HtpBank bank;unsigned char pre_frame90[64],pre_base100[16],target130[16];
 uint32_t native30_bypass,ready;
} HtpSelection;
typedef struct {HtpSelection selected;unsigned stage;} HtpTransaction;
enum{HTP_NO_SELECTION=0,HTP_SELECTED=1,HTP_BAD_BINDING=-1,HTP_BAD_STAGE=-2};
static int htp_key_equal(const HtpKey*a,const HtpKey*b){
 return a->owner==b->owner&&a->main_frame==b->main_frame&&a->clock==b->clock&&
 a->epoch==b->epoch&&a->binding_token==b->binding_token&&a->Q==b->Q&&
 a->native_frame==b->native_frame&&a->thread==b->thread&&a->domain==b->domain&&a->main_bl==b->main_bl;
}
static __attribute__((unused)) void htp_pack_bank(HtpBank*b,const unsigned char*a){
 memcpy(b->coupled,a+0x1a0,64);memcpy(&b->delay1e0,a+0x1e0,8);
 memcpy(b->direction1f0,a+0x1f0,16);memcpy(b->controller200,a+0x200,32);
 memcpy(&b->filtered220,a+0x220,8);memcpy(&b->look2b0,a+0x2b0,4);
 memcpy(&b->derived350,a+0x350,4);memcpy(&b->accum354,a+0x354,8);
}
static void htp_unpack_bank(unsigned char*a,const HtpBank*b){
 memcpy(a+0x1a0,b->coupled,64);memcpy(a+0x1e0,&b->delay1e0,8);
 memcpy(a+0x1f0,b->direction1f0,16);memcpy(a+0x200,b->controller200,32);
 memcpy(a+0x220,&b->filtered220,8);memcpy(a+0x2b0,&b->look2b0,4);
 memcpy(a+0x350,&b->derived350,4);memcpy(a+0x354,&b->accum354,8);
}
static int htp_publish1(HtpTransaction*t,const HtpSelection*s,const HtpKey*actual,
 unsigned char actor[0x380],unsigned char*main_frame){
 if(!s->ready||s->native30_bypass)return HTP_NO_SELECTION;
 if(t->stage||!htp_key_equal(&s->key,actual)||s->key.main_bl>1)return HTP_BAD_BINDING;
 t->selected=*s;t->stage=1;htp_unpack_bank(actor,&s->bank);
 memcpy(main_frame-0x90,s->pre_frame90,64);memcpy(main_frame-0x100,s->pre_base100,16);memcpy(main_frame-0x130,s->target130,16);
 return HTP_SELECTED;
}
static int htp_publish2(HtpTransaction*t,const HtpKey*actual,unsigned char actor[0x380],unsigned char ymm[16][32]){
 if(t->stage!=1)return HTP_BAD_STAGE;
 if(!htp_key_equal(&t->selected.key,actual))return HTP_BAD_BINDING;
 const HtpBank*b=&t->selected.bank;memcpy(actor+0x220,&b->filtered220,8);memcpy(actor+0x350,&b->derived350,4);
 /* Native stage2 equations consume XMM0.z(controller200.z),XMM1.x(224),
  * XMM2.x(220),XMM3.x(350). These full YMM bit patterns match BOTH exact
  * original branches; no unrelated register or FP control/flags are changed. */
 for(unsigned i=0;i<4;++i)memset(ymm[i],0,32);
 memcpy(ymm[0],b->controller200,16);memcpy(ymm[3],&b->derived350,4);
 if(!actual->main_bl){memcpy(ymm[1],&b->filtered224,4);memcpy(ymm[2],&b->filtered220,4);}
 else{uint32_t shuffled[4]={b->controller200[1],b->controller200[0],b->controller200[0],b->controller200[0]};memcpy(ymm[1],shuffled,16);memcpy(ymm[2],b->controller200,16);}
 t->stage=2;return HTP_SELECTED;
}
static int htp_publish3(HtpTransaction*t,const HtpKey*actual,unsigned char actor[0x380]){
 if(t->stage!=2)return HTP_BAD_STAGE;
 if(!htp_key_equal(&t->selected.key,actual))return HTP_BAD_BINDING;
 memcpy(actor+0x1f0,t->selected.bank.direction1f0,16);t->stage=3;return HTP_SELECTED;
}
