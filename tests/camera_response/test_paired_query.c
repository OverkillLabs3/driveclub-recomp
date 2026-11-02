#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/camera_response/paired-query-core.inc"
static unsigned checks;
static int bounded(__int128 x){return x>=INT64_MIN&&x<=INT64_MAX;}
static uint64_t state=UINT64_C(0x6128a41eb3097c55);
static uint64_t draw(void){state^=state<<13;state^=state>>7;return state^=state<<17;}
static int test(int64_t argument,int64_t offset,int64_t T,int64_t dt){
 PqnTime out;memset(&out,0xA5,sizeof out);
 int got=pqn_time_argument(argument,offset,T,dt,&out);
 __int128 shift=(__int128)dt-33333,normalized=(__int128)argument+shift;
 __int128 original_query=(__int128)argument+offset,normalized_query=normalized+offset;
 __int128 K=(__int128)T-dt+33333;
 int expect=T>=0&&dt>=0&&dt<=33333&&bounded(normalized)&&bounded(original_query)&&bounded(normalized_query)&&bounded(K);
 ++checks;if(got!=expect)return 0;
 if(!got){PqnTime zero={0};return !memcmp(&out,&zero,sizeof out);}
 return out.T==T&&out.dt==dt&&out.original_argument==argument&&out.reader_offset==offset&&out.shift==(int64_t)shift&&out.normalized_argument==(int64_t)normalized&&out.original_query_unclamped==(int64_t)original_query&&out.normalized_query_unclamped==(int64_t)normalized_query&&out.original_K==(int64_t)K&&out.effective_K==T&&out.native_Q==(dt==33333);
}
int main(void){
 int64_t edge[]={INT64_MIN,INT64_MIN+1,-33334,-33333,-1,0,1,33332,33333,33334,INT64_MAX-33333,INT64_MAX-1,INT64_MAX};
 for(unsigned a=0;a<13;++a)for(unsigned b=0;b<13;++b)for(unsigned c=0;c<13;++c)for(unsigned d=0;d<13;++d)if(!test(edge[a],edge[b],edge[c],edge[d]))return 1;
 for(unsigned n=0;n<100000;++n){int64_t a=(int64_t)draw(),b=(int64_t)draw(),T=(int64_t)(draw()>>1),dt=(int64_t)(draw()%33336)-1;if(!test(a,b,T,dt))return 2;}
 ++checks;if(pqn_time_argument(0,0,0,33333,NULL))return 3;
 printf("{\"checks\":%u,\"errors\":0,\"scope\":\"independent 128-bit arithmetic and failure-zero model, not ABI or native calls\"}\n",checks);return 0;
}
