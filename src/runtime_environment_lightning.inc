/* Lightning owns a CPU animation/geometry timeline separate from rain particles.
 * Snapshot scalar/RNG state, inline points, draw records and logical queue nodes.
 * Resource pointers, allocation addresses and capacities always remain live. */
enum { ENV_LIGHT_SLOTS=8192,ENV_LIGHT_BUDGET=64*1024*1024 };
typedef struct {
  double time;uintptr_t controller,geometry;
  uint32_t points,draw_bytes,nodes,size;
  unsigned char controller_state[0x30],controller_positions[0x40],geometry_state[0xb8];
  unsigned char data[];
} EnvLightSnapshot;
static EnvLightSnapshot *env_lights[ENV_LIGHT_SLOTS];
static unsigned env_light_first,env_light_count;static size_t env_light_bytes;
static EnvUpdate env_light_original;
static EnvLightSnapshot *env_light_at(unsigned i){return env_lights[(env_light_first+i)%ENV_LIGHT_SLOTS];}
static void env_light_drop_first(void){EnvLightSnapshot *s=env_light_at(0);env_light_bytes-=s->size;free(s);env_light_first=(env_light_first+1)%ENV_LIGHT_SLOTS;--env_light_count;}
static void env_light_drop_last(void){EnvLightSnapshot *s=env_light_at(env_light_count-1);env_light_bytes-=s->size;free(s);--env_light_count;}
static void env_light_clear(void){while(env_light_count)env_light_drop_last();env_light_first=0;env_store_double(&env_light_oldest_bits,0);}
static EnvLightSnapshot *env_light_capture(uintptr_t c,uintptr_t g,double time) {
  if(!c||!g)return NULL;
  unsigned char *p=(unsigned char *)g;
  int active=*(unsigned *)(p+0x40)||*(unsigned *)(p+0x44);
  uint32_t points=active?*(uint32_t *)(p+0x10120):0;
  uintptr_t begin=*(uintptr_t *)(p+0x100),end=*(uintptr_t *)(p+0x108),capacity=*(uintptr_t *)(p+0x110);
  uint64_t nodes=active?*(uint64_t *)(p+0xf0):0;
  uint64_t pool_cap=*(uint64_t *)(p+0xe0),first=*(uint64_t *)(p+0xe8);
  uintptr_t *pool=*(uintptr_t **)(p+0xd8);
  if(points>4096||nodes>4096||pool_cap>65536||(nodes&&(!pool_cap||(pool_cap&(pool_cap-1))))||end<begin||end>capacity||end-begin>81920||(end-begin)%20||nodes>pool_cap||(!pool&&nodes))return NULL;
  uint32_t draws=active?(uint32_t)(end-begin):0;
  size_t size=sizeof(EnvLightSnapshot)+(size_t)points*16+draws+(size_t)nodes*0x50;
  EnvLightSnapshot *s=malloc(size);if(!s)return NULL;
  s->time=time;s->controller=c;s->geometry=g;s->points=points;s->draw_bytes=draws;s->nodes=(uint32_t)nodes;s->size=(uint32_t)size;
  memcpy(s->controller_state,(void *)(c+0x10),0x30);memcpy(s->controller_positions,(void *)(c+0x60),0x40);
  memcpy(s->geometry_state,p,0xb8);
  unsigned char *d=s->data;
  memcpy(d,p+0x120,(size_t)points*16);d+=(size_t)points*16;
  if(draws){memcpy(d,(void *)begin,draws);d+=draws;}
  for(uint64_t i=0;i<nodes;i++) {
    uintptr_t node=pool[(first+i)&(pool_cap-1)];if(!node){free(s);return NULL;}
    memcpy(d,(void *)node,0x50);d+=0x50;
  }
  return s;
}
static int env_light_restore(EnvLightSnapshot *s,uintptr_t c,uintptr_t g) {
  if(s->controller!=c||s->geometry!=g)return 0;
  unsigned char *p=(unsigned char *)g;
  typedef void (ABI *Reserve)(void *,unsigned);
  Reserve reserve=(Reserve)(image+0x1050470);
  uintptr_t begin=*(uintptr_t *)(p+0x100),capacity=*(uintptr_t *)(p+0x110);
  for(int i=0;capacity-begin<s->draw_bytes&&i<8;i++) {
    reserve(p+0xf8,s->draw_bytes/20);
    begin=*(uintptr_t *)(p+0x100);capacity=*(uintptr_t *)(p+0x110);
  }
  if(capacity<begin||capacity-begin<s->draw_bytes)return 0;
  uintptr_t *pool=*(uintptr_t **)(p+0xd8);uint64_t pool_cap=*(uint64_t *)(p+0xe0);
  if(s->nodes>pool_cap||pool_cap>65536||(!pool&&s->nodes))return 0;
  uint32_t available=0;for(uint64_t i=0;i<pool_cap;i++)available+=pool[i]!=0;
  if(available<s->nodes)return 0;
  uint32_t found=0;
  for(uint64_t i=0;i<pool_cap&&found<s->nodes;i++)if(pool[i]) {
    uintptr_t q=pool[found];pool[found]=pool[i];pool[i]=q;++found;
  }
  if(found<s->nodes)return 0;
  memcpy((void *)(c+0x10),s->controller_state,0x30);memcpy((void *)(c+0x60),s->controller_positions,0x40);
  /* Audio is not rewound; never re-issue an old thunder one-shot. */
  *(unsigned char *)(c+0x35)=0;
  memcpy(p,s->geometry_state,0xb8);
  const unsigned char *d=s->data;
  memcpy(p+0x120,d,(size_t)s->points*16);d+=(size_t)s->points*16;
  *(uint32_t *)(p+0x10120)=s->points;
  if(s->draw_bytes){memcpy((void *)begin,d,s->draw_bytes);d+=s->draw_bytes;}
  *(uintptr_t *)(p+0x108)=begin+s->draw_bytes;
  for(uint32_t i=0;i<s->nodes;i++){memcpy((void *)pool[i],d,0x50);d+=0x50;}
  *(uint64_t *)(p+0xe8)=0;*(uint64_t *)(p+0xf0)=s->nodes;
  return 1;
}
static ABI void env_light_update(void *self,float dt) {
  unsigned char *p=self;uintptr_t c=*(uintptr_t *)(p+0x28),g=c?*(uintptr_t *)(c+0xd0):0;
  if(!env_timelapse_enabled||!c||!g||!isfinite(dt)||dt<0||dt>.25f){env_light_original(self,dt);return;}
  static uintptr_t last_c,last_g;static unsigned generation;
  unsigned gen=__atomic_load_n(&env_generation,__ATOMIC_ACQUIRE);
  if(c!=last_c||g!=last_g||gen!=generation){env_light_clear();last_c=c;last_g=g;generation=gen;}
  double cursor=env_load_double(&env_cursor_bits);int action=__atomic_load_n(&env_action,__ATOMIC_ACQUIRE);
  if(action<0) {
    if(env_light_count){unsigned lo=0,hi=env_light_count;while(lo+1<hi){unsigned mid=(lo+hi)/2;if(env_light_at(mid)->time<=cursor)lo=mid;else hi=mid;}(void)env_light_restore(env_light_at(lo),c,g);}
    env_light_original(self,0);
  } else {
    while(env_light_count&&env_light_at(env_light_count-1)->time>cursor)env_light_drop_last();
    double remaining=action>0?env_group_step(dt,0):dt;
    do {float step=(float)fmin(remaining,1.0/30.0);env_light_original(self,step);remaining-=step;}while(remaining>0.000001);
    EnvLightSnapshot *s=env_light_capture(c,g,cursor);
    if(s){while(env_light_count&&(env_light_count==ENV_LIGHT_SLOTS||env_light_bytes+s->size>ENV_LIGHT_BUDGET))env_light_drop_first();env_lights[(env_light_first+env_light_count++)%ENV_LIGHT_SLOTS]=s;env_light_bytes+=s->size;env_store_double(&env_light_oldest_bits,env_light_at(0)->time);}
  }
}
