/* clang -std=c11 -Wall -Wextra -Werror tools/trophy_persistence_test.c -pthread -o trophy-test
 * Run with the same empty test directory in two separate processes: trophy-test <dir> write, then read. */
#define _GNU_SOURCE
#include "../src/runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TROPHY_INVALID ((int32_t)0x80551604)
static const char *test_root, *test_title="CUSA00003";
const char *runtime_file_user_dir(void) { return test_root; }
const char *runtime_savedata_title(void) { return test_title; }
static int new_id(void) { static int id; return ++id; }
#include "../src/runtime_trophy.inc"

static unsigned unlocked(int32_t ctx,int trophy) {
    uint32_t flags[4],count;
    assert(!trophy_unlock_state(ctx,1,flags,&count));
    assert(count==128);
    TrophyData data={.size=sizeof(data)};
    assert(!trophy_info(ctx,1,trophy,NULL,&data));
    assert(data.id==trophy && data.unlocked==!!(flags[trophy/32]&(1u<<(trophy%32))));
    assert(!!data.timestamp==data.unlocked);
    unsigned total=0;
    for (unsigned i=0;i<count;++i) total+=!!(flags[i/32]&(1u<<(i%32)));
    return total;
}
int main(int argc,char **argv) {
    assert(argc==3);
    test_root=argv[1];
    int32_t ctx,handle,platinum=999;
    assert(!trophy_context(&ctx,1,0,0));
    assert(!trophy_handle(&handle) && !trophy_register(ctx,handle,0));
    if (!strcmp(argv[2],"write")) {
        assert(unlocked(ctx,126)==0);
        assert(!trophy_unlock(ctx,handle,126,&platinum) && platinum==-1);
        assert(unlocked(ctx,126)==1 && unlocked(ctx,125)==1);
        assert(!trophy_destroy_context(ctx));
        assert(!trophy_context(&ctx,1,0,0) && unlocked(ctx,126)==1);
        TrophyData before={.size=sizeof(before)},after={.size=sizeof(after)};
        assert(!trophy_info(ctx,handle,126,NULL,&before));
        assert(trophy_unlock(ctx,handle,126,&platinum)==(int32_t)0x8055160C);
        assert(!trophy_info(ctx,handle,126,NULL,&after) && before.timestamp==after.timestamp);
        const int edges[]={0,31,32,63,64,95,96,127};
        for (unsigned i=0;i<sizeof(edges)/sizeof(*edges);++i) assert(!trophy_unlock(ctx,handle,edges[i],NULL));
        assert(unlocked(ctx,126)==9);
    } else {
        assert(!strcmp(argv[2],"read"));
        assert(unlocked(ctx,126)==9);
        uint32_t flags[4],count;
        assert(!trophy_unlock_state(ctx,handle,flags,&count));
        assert(flags[0]==0x80000001u && flags[1]==0x80000001u && flags[2]==0x80000001u && flags[3]==0xc0000001u);
        TrophyData data={.size=sizeof(data)};
        assert(!trophy_info(ctx,handle,125,NULL,&data) && !data.unlocked);
    }
    assert(trophy_unlock(ctx,handle,-1,NULL)==TROPHY_BAD_ID);
    assert(trophy_unlock(ctx,handle,128,NULL)==TROPHY_BAD_ID);
    assert(trophy_unlock_state(ctx,handle,NULL,NULL)==TROPHY_INVALID);
    assert(trophy_info(ctx,handle,128,NULL,NULL)==TROPHY_BAD_ID);
    uint64_t short_details[2]={8,0x12345678};
    assert(trophy_info(ctx,handle,126,short_details,NULL)==TROPHY_INVALID && short_details[1]==0x12345678);
    int32_t other;
    assert(trophy_context(&other,1,0,0)==(int32_t)0x80551613);
    assert(!trophy_context(&other,2,0,0) && unlocked(other,126)==0);
    assert(!trophy_destroy_context(other));
    assert(!trophy_context(&other,1,1,0) && unlocked(other,126)==0);
    assert(!trophy_destroy_context(other));
    test_title="CUSA00093";
    assert(!trophy_context(&other,1,0,0) && unlocked(other,126)==0);
    assert(!trophy_destroy_context(other));
    test_title="CUSA00003";
    char tmp[1032]; snprintf(tmp,sizeof(tmp),"%s.new",trophy_find(ctx)->path);
    assert(!mkdir(tmp,0755));
    assert(trophy_unlock(ctx,handle,125,NULL)==TROPHY_IO_ERROR);
    TrophyData data={.size=sizeof(data)};
    assert(!trophy_info(ctx,handle,125,NULL,&data) && !data.unlocked);
    assert(!rmdir(tmp));
    assert(!trophy_destroy_context(ctx));
    assert(trophy_destroy_context(ctx)==TROPHY_BAD_CONTEXT);
    assert(!trophy_context(&ctx,1,0,0) && unlocked(ctx,126)==9);
    uint64_t game[8]={sizeof(game)};
    assert(!trophy_game_info(ctx,handle,game,NULL));
    assert(!trophy_destroy_handle(handle));
    puts("PASS: trophy state, process restart, flag boundaries, timestamps, duplicate unlock, isolation and failed-write rollback");
    return 0;
}
