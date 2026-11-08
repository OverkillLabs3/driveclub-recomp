/* Actual TimeCycle producer identity, not a guessed meaning of TC00.
 * DeltaTime's exact detour recomputes elapsed from timer-TC08 unconditionally,
 * caps33333<elapsed<=250000 AND advances the stored timer by that same cap.
 * At elapsed>250000 it caps elapsed only; the EXISTING exact delta(TC08)
 * guard rejects that unsupported long-stall clock epoch. With ordinary/clipped
 * matching samples, unit delta(TC08)==pausedTC20 even when TC00=16666.
 * Original native code still requires its actual no-fixed sentinel. */
static int hps_effective_producer(const unsigned char*t){
 static const unsigned char native[]={0x75,0x07,0x48,0x89,0xc6,0x48,0x2b,0x73,0x08};
 static const unsigned char delta[]={0xe9,0x0d,0x50,0xbf,0x00,0x90,0x90,0x90,0x90};
 static const unsigned char body[]={0x48,0x89,0xc6,0x48,0x2b,0x73,0x08,0x48,0x81,0xfe,0x90,0xd0,0x03,0x00,0x77,0x13,0x48,0x81,0xfe,0x35,0x82,0x00,0x00,0x76,0x0f,0x48,0x8b,0x43,0x08,0x48,0x05,0x35,0x82,0x00,0x00,0xbe,0x35,0x82,0x00,0x00,0xe9,0xca,0xaf,0x40,0xff};
 if(hps_u64(t+0x30)!=UINT64_C(0x3ff0000000000000))return 0;
 unsigned char a[9],b[9],pa[45],pb[45];uint64_t sentinel=0;
 if(!camera_pose_read((uintptr_t)image+0x66306e,a,9))return 0;
 if(!memcmp(a,native,9))return camera_pose_read((uintptr_t)image+0x66306e,b,9)&&!memcmp(a,b,9)&&camera_pose_read((uintptr_t)image+0x12493e0,&sentinel,8)&&hps_u64(t)==sentinel?1:0;
 if(memcmp(a,delta,9)||!camera_pose_read((uintptr_t)image+0x1258080,pa,45)||!camera_pose_read((uintptr_t)image+0x1258080,pb,45)||!camera_pose_read((uintptr_t)image+0x66306e,b,9)||memcmp(a,b,9)||memcmp(pa,pb,45)||memcmp(pa,body,45))return 0;
 return 2;
}
