/* Native1.28 numeric span metadata only. Bodies are copied from the loaded game at startup. */
#define HGR_MAIN 0
#define HGR_MAIN_SIZE 3015
#define HGR_LOOK 3072
#define HGR_LOOK_SIZE 50
#define HGR_ACCUM 3136
#define HGR_ACCUM_SIZE 148
#define HGR_ARGS 3328
#define HGR_ARGS_SIZE 156
#define HGR_CE 3520
#define HGR_CE_SIZE 478
#define HGR_REMAP 4032
#define HGR_REMAP_SIZE 31
static const uintptr_t hgr_data_targets[]={0x1351614,0x13514c0,0x191b100,0x13514b0,0x191b0f0,0x1351618,0x1351604,0x13514d0,0x13514e0,0x13514f0,0x1351500,0x1351510,0x135160c,0x1351520,0x135161c,0x1351620,0x1351624,0x1351628,0x1351654,0x1351658,0x135165c,0x1351660,0x1351664,0x166e250,0x1351550,0x135166c,0x135bcb8,0x135bcbc,0x135bc90,0x135bca0,0x12e33fc};
static const struct{unsigned offset,length,disp,index;}hgr_relocs[]={
{8,8,4,0},
{344,8,4,1},
{394,7,3,2},
{405,8,4,1},
{413,8,4,3},
{433,7,3,4},
{510,8,4,5},
{532,8,4,0},
{540,8,4,1},
{627,8,4,3},
{737,8,4,1},
{888,8,4,5},
{915,8,4,1},
{996,8,4,6},
{1004,8,4,7},
{1022,8,4,8},
{1042,8,4,9},
{1172,8,4,8},
{1193,8,4,9},
{1282,8,4,6},
{1353,8,4,7},
{1366,8,4,10},
{1568,8,4,11},
{1612,8,4,1},
{1633,8,4,12},
{1836,8,4,13},
{1858,8,4,11},
{1892,8,4,8},
{1912,8,4,9},
{1962,8,4,14},
{1970,8,4,15},
{2081,8,4,16},
{2184,8,4,11},
{2346,8,4,17},
{2596,8,4,8},
{2620,8,4,9},
{3092,8,4,18},
{3100,8,4,19},
{3141,8,4,20},
{3158,8,4,21},
{3176,8,4,22},
{3198,8,4,20},
{3221,8,4,22},
{3407,7,3,23},
{3429,8,4,24},
{3450,8,4,25},
{3520,8,4,26},
{3540,8,4,27},
{3548,8,4,27},
{3558,8,4,28},
{3566,9,4,26},
{3575,8,4,27},
{3646,8,4,28},
{3668,8,4,28},
{3728,8,4,29},
{3775,8,4,29},
{3803,8,4,29},
{3849,8,4,27},
{3892,8,4,27},
{4054,8,4,30},
};
static const struct{unsigned offset;uintptr_t target,native;}hgr_calls[]={
{1788,0x123e820,0xb9209d},
{1978,0x675e00,0xb9215b},
{2038,0xb92590,0xb92197},
{2092,0xb92a40,0xb921cd},
{2111,0x672a70,0xb921e0},
{2236,0xe70b10,0xb9225d},
{2466,0xb93010,0xb92343},
{2682,0xb93510,0xb9241b},
{3399,0xce03b0,0xb92f8f},
};
#define HGR_ISLANDS 4064
#define HGR_DATA 8192
#define HGR_TOTAL 12288
static const struct{uintptr_t native;unsigned offset,size;uint64_t hash;}hgr_parts[]={
{0xb919a1,0,3015,UINT64_C(0x35cb3a4b8d373e4b)},
{0xb929e1,3072,50,UINT64_C(0x88977302f5e67325)},
{0xb92cb4,3136,148,UINT64_C(0x8bebfd97731ba151)},
{0xb92f48,3328,156,UINT64_C(0xd30d1e87fe8b9928)},
{0xce03b0,3520,478,UINT64_C(0xc3778680af268612)},
{0x675e00,4032,31,UINT64_C(0xccbcbfe056b1cb06)},
};
