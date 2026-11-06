/* All three environment queues share the sky clock. Keep plain key data only;
 * objects, vectors, renderer pointers and allocations remain owned by the game. */
enum { ENV_SKY_SLOTS=8192,ENV_SKY_KEYS=4,ENV_SKY_KEY_BYTES=0x88 };
typedef struct {double anchor,cursor;unsigned char key[3][ENV_SKY_KEYS][ENV_SKY_KEY_BYTES];} EnvSkySnapshot;
static EnvSkySnapshot *env_sky_history;
static unsigned env_sky_first,env_sky_count;
static EnvSkySnapshot *env_sky_at(unsigned i){return &env_sky_history[(env_sky_first+i)%ENV_SKY_SLOTS];}
static unsigned env_sky_key_size(unsigned group){return group?0x88:0x90;}
static int env_sky_queue(uintptr_t sequence,uintptr_t *begin,unsigned *count) {
  uintptr_t end=*(uintptr_t *)(sequence+0x28);*begin=*(uintptr_t *)(sequence+0x20);
  if(!*begin||end<*begin||(end-*begin)%8)return 0;
  *count=(unsigned)((end-*begin)/8);return *count>=4&&*count<=5;
}
static void env_sky_capture(uintptr_t *sequence,double clock) {
  uintptr_t begin[3];unsigned count[3];
  for(unsigned i=0;i<3;i++)if(!env_sky_queue(sequence[i],&begin[i],&count[i]))return;
  double anchor=*(float *)(*(uintptr_t *)begin[1]+8);
  if(!isfinite(anchor))return;
  /* Capture every native one-second producer step, including skipped render times. */
  for(unsigned i=0;i<3;i++){
    double a=*(float *)(*(uintptr_t *)begin[i]+8),b=*(float *)(*(uintptr_t *)(begin[i]+8)+8);
    if(!isfinite(a)||!isfinite(b)||a>anchor||b<=anchor)return;
  }
  if(!env_sky_history)env_sky_history=calloc(ENV_SKY_SLOTS,sizeof *env_sky_history);
  if(!env_sky_history)return;
  if(env_sky_count&&anchor<=env_sky_at(env_sky_count-1)->anchor)return;
  if(env_sky_count==ENV_SKY_SLOTS){env_sky_first=(env_sky_first+1)%ENV_SKY_SLOTS;--env_sky_count;}
  EnvSkySnapshot *s=env_sky_at(env_sky_count++);s->anchor=anchor;s->cursor=fmax(0,env_load_double(&env_cursor_bits)+anchor-clock);
  for(unsigned i=0;i<3;i++)for(unsigned j=0;j<ENV_SKY_KEYS;j++){
    uintptr_t key=*(uintptr_t *)(begin[i]+j*8);
    memcpy(s->key[i][j],(void *)(key+8),env_sky_key_size(i)-8);
  }
  env_store_double(&env_cloud_oldest_bits,env_sky_at(0)->cursor);
}
static int env_sky_restore(uintptr_t *sequence,double clock) {
  if(!env_sky_count)return 0;
  unsigned lo=0,hi=env_sky_count;
  while(lo+1<hi){unsigned mid=(lo+hi)/2;if(env_sky_at(mid)->anchor<=clock)lo=mid;else hi=mid;}
  EnvSkySnapshot *s=env_sky_at(lo);
  if(s->anchor>clock)return 0;
  uintptr_t checked[3];unsigned checked_count[3];
  for(unsigned i=0;i<3;i++)if(!env_sky_queue(sequence[i],&checked[i],&checked_count[i]))return 0;
  typedef void (ABI *Remove)(void *,int);
  for(unsigned i=0;i<3;i++){
    uintptr_t begin;unsigned count;if(!env_sky_queue(sequence[i],&begin,&count))return 0;
    if(count==5){Remove remove=(Remove)(image+(i?0x889a60:0x86e630));remove((void *)(sequence[i]-(i?8:0x10)),4);}
    if(!env_sky_queue(sequence[i],&begin,&count)||count!=4)return 0;
    for(unsigned j=0;j<4;j++){
      uintptr_t key=*(uintptr_t *)(begin+j*8);
      memcpy((void *)(key+8),s->key[i][j],env_sky_key_size(i)-8);
    }
    *(int *)(sequence[i]+0x4c)=0;
    if(!i){
      /* Native atmosphere cache records: scalar progress and key index. */
      unsigned char *a=(unsigned char *)(sequence[i]-0x10);
      for(unsigned j=0;j<3;j++){memset(a+0x6f0+j*0x28,0,0x24);*(int *)(a+0x714+j*0x28)=-1000;}
      typedef void (ABI *Dirty)(void *);((Dirty)(image+0x867420))(a);
    }else{
      unsigned char *c=(unsigned char *)(sequence[i]-8);
      /* Discard incremental progress, never restore stale GPU data. */
      for(unsigned j=0;j<3;j++){c[0x59+j]=0;memset(c+0x474+j*0xd0,0,12);*(unsigned *)(c+0x490+j*0xd0)=0;*(int *)(c+0x534+j*0xd0)=-1;}
      *(float *)(c+0x6dc)=0;
    }
  }
  return 1;
}
/* The native producer advances at most one key per render frame. Advance CPU
 * queues before their cloud/shadow consumers, without extra simulation/render. */
static void env_sky_prepare(void *self,float value) {
  if(!env_timelapse_enabled||!isfinite(value))return;
  unsigned char *p=self;uintptr_t wrapper=*(uintptr_t *)(p+0x50);if(!wrapper)return;
  uintptr_t atmosphere=*(uintptr_t *)(wrapper+0x18),cloud=*(uintptr_t *)(wrapper+8),cirrus=*(uintptr_t *)(wrapper+0x10);
  if(!atmosphere||!cloud||!cirrus)return;
  uintptr_t sequence[3]={atmosphere+0x10,cloud+8,cirrus+8};
  static void *last_self;static unsigned generation;static double restored_anchor=-1;
  unsigned gen=__atomic_load_n(&env_generation,__ATOMIC_ACQUIRE);
  if(last_self!=self||generation!=gen){last_self=self;generation=gen;env_sky_first=env_sky_count=0;restored_anchor=-1;env_store_double(&env_cloud_oldest_bits,0);}
  int action=__atomic_load_n(&env_action,__ATOMIC_ACQUIRE);
  if(action<0){
    if(env_sky_restore(sequence,value)){restored_anchor=value;}
    return;
  }
  if(restored_anchor>=0){while(env_sky_count&&env_sky_at(env_sky_count-1)->anchor>value)--env_sky_count;restored_anchor=-1;}
  typedef int (ABI *Index)(void *,float);typedef void (ABI *Produce)(void *);
  Index index=(Index)(image+0x85f470);Produce produce=(Produce)(image+0x602720);
  unsigned steps=0;
  env_sky_capture(sequence,value);
  for(;steps<4096;steps++) {
    int advance=0;
    for(unsigned i=0;i<3;i++){
      uintptr_t begin;unsigned count;if(!env_sky_queue(sequence[i],&begin,&count))return;
      int n=index((void *)sequence[i],value);*(int *)(sequence[i]+0x4c)=n;if(n>0)advance=1;
    }
    if(!advance)break;
    produce(self);env_sky_capture(sequence,value);
  }
}
