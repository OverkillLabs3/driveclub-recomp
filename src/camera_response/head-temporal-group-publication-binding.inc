/* Typed numeric result -> bitwise staged publication. No solver/source
 * sampling lives here. A missing/unsupported selection leaves native output. */
_Static_assert(offsetof(HgrNativeOutput,pre_frame90)==0,"native precompose90");
_Static_assert(offsetof(HgrNativeOutput,pre_base100)==64,"native precompose100");
_Static_assert(offsetof(HgrNativeOutput,dependent_target130)==80,"native target130");
_Static_assert(offsetof(HgrNativeOutput,final_actor)==1888,"native final actor bank");
static int hgg_htp_produce(void*context,const HtpRequest*request,HtpSelection*out){
 (void)context;if(!request||!out)return HTP_NO_SELECTION;memset(out,0,sizeof*out);
 HggResult result;memset(&result,0,sizeof result);
 int status=hgg_runtime_select(request,&result);
 if(result.native30_bypass){out->native30_bypass=1;return HTP_NO_SELECTION;}
 if(status!=HGG_SELECTED||result.status!=HGG_SELECTED||!result.ready||
    result.domain>1||result.epoch!=request->key.epoch||!result.epoch||
    !result.binding_token||!result.version||result.Q<0||result.Q%HGG_QUANTUM||
    result.Q>result.selected_T||(result.domain==0&&result.selected_T!=request->logical_T)||(result.domain==1&&(request->logical_dt!=0||result.selected_T<request->logical_T)))return HTP_NO_SELECTION;
 out->key=request->key;out->key.domain=result.domain;out->key.Q=result.Q;
 out->key.binding_token=result.binding_token;htp_pack_bank(&out->bank,result.output.final_actor);
 memcpy(out->pre_frame90,result.output.pre_frame90,64);
 memcpy(out->pre_base100,result.output.pre_base100,16);
 memcpy(out->target130,result.output.dependent_target130,16);out->ready=1;
 return HTP_SELECTED;
}
static void hgg_htp_forget(void*context,uintptr_t owner){(void)context;hgg_runtime_forget(owner);}
static int hgg_htp_bind(void){
 if(!hgg_runtime_prepare())return 0;
 return htp_register_producer(hgg_htp_produce,hgg_htp_forget,htp_functional_current,NULL,
     HTP_PROOF_NATIVE_Q|HTP_PROOF_SOURCE_Q|HTP_PROOF_RESET_COVERAGE);
}
