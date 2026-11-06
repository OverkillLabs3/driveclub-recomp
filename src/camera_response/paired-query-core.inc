/* Private paired source-time normalization arithmetic. No memory/getter,
 * provider clock write, cursor clamp, source mutation or interpolation.
 * SUCCESS HERE IS NOT RUNTIME ELIGIBILITY: scope/mode/lease/pair checks remain
 * separate. dt0 arithmetic is representable but pause policy must be admitted. */
#ifndef PRIVATE_PAIRED_QUERY_CORE_INCLUDED
#define PRIVATE_PAIRED_QUERY_CORE_INCLUDED
#define PQN_Q INT64_C(33333)
typedef struct {
 int64_t T,dt,original_argument,reader_offset,shift,normalized_argument;
 int64_t original_query_unclamped,normalized_query_unclamped;
 int64_t original_K,effective_K;
 unsigned native_Q;
} PqnTime;
static int pqn_iadd(int64_t a,int64_t b,int64_t*out){if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b))return 0;*out=a+b;return 1;}
static int pqn_isub(int64_t a,int64_t b,int64_t*out){if((b>0&&a<INT64_MIN+b)||(b<0&&a>INT64_MAX+b))return 0;*out=a-b;return 1;}
static int pqn_time_argument(int64_t argument,int64_t reader_offset,int64_t T,int64_t dt,PqnTime*out){
 if(!out)return 0;memset(out,0,sizeof*out);if(T<0||dt<0||dt>PQN_Q)return 0;
 PqnTime r;memset(&r,0,sizeof r);r.T=T;r.dt=dt;r.original_argument=argument;r.reader_offset=reader_offset;r.effective_K=T;r.native_Q=dt==PQN_Q;
 int64_t base=0;
 if(!pqn_isub(dt,PQN_Q,&r.shift)||!pqn_iadd(argument,r.shift,&r.normalized_argument)||
    !pqn_iadd(argument,reader_offset,&r.original_query_unclamped)||!pqn_iadd(r.normalized_argument,reader_offset,&r.normalized_query_unclamped)||
    !pqn_isub(T,dt,&base)||!pqn_iadd(base,PQN_Q,&r.original_K))return 0;
 *out=r;return 1;
}
#endif
