#include <sirit/sirit.h>
#include <array>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>
using u32 = uint32_t;
using Id = Sirit::Id;
namespace AmdGpu { enum class ImageType { Color2D, Color2DArray }; enum class ClampMode { Wrap, Mirror, Other }; }
namespace IR { struct Inst {}; struct Value { bool IsEmpty() const { return true; } }; }
namespace WorldRepeat { std::unordered_set<IR::Inst*> selected; }
enum class SwStage { Fragment };
static bool GlobalRepeatEnabled() { return true; }
struct Sharp { u32 NumLevels() const { return 8; } };
struct Samp { bool force_unnormalized=false; AmdGpu::ClampMode clamp_x=AmdGpu::ClampMode::Wrap,clamp_y=AmdGpu::ClampMode::Mirror; };
struct InfoImage { Sharp GetSharp(auto&) const { return {}; } };
struct InfoSampler { bool is_depth=false; Samp value; Samp GetSharp(auto&) const { return value; } };
struct Info { std::array<InfoImage,1> images; std::array<InfoSampler,1> samplers; };
struct Texture { bool is_integer=false,is_storage=false,native_cube=false; AmdGpu::ImageType view_type; Id image_type,id,sampled_type; };
struct EmitContext: Sirit::Module {
    using Module::Module;
    std::array<Id,5> F32{},U32{},S32{},U1{};
    std::array<Texture,1> images;
    std::array<Id,1> samplers;
    Id sampler_type{},last_label{};
    SwStage sw_stage=SwStage::Fragment;
    Info info;
    struct Profile { u32 supported_spirv=0x00010300; } profile;
    std::vector<Id> interfaces;
    Id ConstF32(float v) { return Constant(F32[1],v); }
    Id ConstU32(u32 v) { return Constant(U32[1],v); }
    Id AddLabel(Id label) { last_label=Module::AddLabel(label); return last_label; }
    Id AddLabel() { last_label=Module::AddLabel(); return last_label; }
};
// PRODUCTION_FUNCTION
int main(int argc,char** argv) {
 for (int variant=0;variant<32;++variant) {
    const u32 version=(variant%8)<4?0x00010300:0x00010400;
    EmitContext c(version);
    c.profile.supported_spirv=version;
    c.AddCapability(spv::Capability::Shader);
    c.SetMemoryModel(spv::AddressingModel::Logical,spv::MemoryModel::GLSL450);
    c.F32[1]=c.TypeFloat(32);c.U32[1]=c.TypeUInt(32);c.S32[1]=c.TypeSInt(32);c.U1[1]=c.TypeBool();
    for(int i=2;i<=4;++i)c.F32[i]=c.TypeVector(c.F32[1],i);
    const bool array=variant&1, gradients=variant&2;
    c.info.samplers[0].value.clamp_x=(variant/8)&1?AmdGpu::ClampMode::Mirror:AmdGpu::ClampMode::Wrap;
    c.info.samplers[0].value.clamp_y=(variant/8)&2?AmdGpu::ClampMode::Mirror:AmdGpu::ClampMode::Wrap;
    auto& t=c.images[0]; t.view_type=array?AmdGpu::ImageType::Color2DArray:AmdGpu::ImageType::Color2D;
    t.image_type=c.TypeImage(c.F32[1],spv::Dim::Dim2D,0,array,false,1,spv::ImageFormat::Unknown);
    t.sampled_type=c.TypeSampledImage(t.image_type);
    t.id=c.AddGlobalVariable(c.TypePointer(spv::StorageClass::UniformConstant,t.image_type),spv::StorageClass::UniformConstant);
    c.sampler_type=c.TypeSampler();c.samplers[0]=c.AddGlobalVariable(c.TypePointer(spv::StorageClass::UniformConstant,c.sampler_type),spv::StorageClass::UniformConstant);
    c.Decorate(t.id,spv::Decoration::DescriptorSet,0u);c.Decorate(t.id,spv::Decoration::Binding,0u);
    c.Decorate(c.samplers[0],spv::Decoration::DescriptorSet,0u);c.Decorate(c.samplers[0],spv::Decoration::Binding,1u);
    const Id input=c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Input,c.F32[4]),spv::StorageClass::Input);
    const Id output=c.AddGlobalVariable(c.TypePointer(spv::StorageClass::Output,c.F32[4]),spv::StorageClass::Output);
    c.Decorate(input,spv::Decoration::Location,0u);c.Decorate(output,spv::Decoration::Location,0u);
    const Id fn=c.OpFunction(c.TypeVoid(),spv::FunctionControlMask::MaskNone,c.TypeFunction(c.TypeVoid()));
    c.AddExecutionMode(fn,spv::ExecutionMode::OriginUpperLeft);
    c.AddLabel();
    const Id coords4=c.OpLoad(c.F32[4],input);
    const Id uv=c.OpVectorShuffle(c.F32[2],coords4,coords4,0,1);
    const Id coords=array?c.OpVectorShuffle(c.F32[3],coords4,coords4,0,1,2):uv;
    const Id image=c.OpLoad(t.image_type,t.id),sampler=c.OpLoad(c.sampler_type,c.samplers[0]);
    const Id combined=c.OpSampledImage(t.sampled_type,image,sampler);
    const Id dx=gradients?c.OpDPdx(c.F32[2],uv):Id{},dy=gradients?c.OpDPdy(c.F32[2],uv):Id{};
    const Id original=gradients?c.OpImageSampleExplicitLod(c.F32[4],combined,coords,spv::ImageOperandsMask::Grad,dx,dy):c.OpImageSampleImplicitLod(c.F32[4],combined,coords);
    IR::Inst inst;WorldRepeat::selected.insert(&inst);
    const Id bias=gradients?c.ConstF32(.5f):Id{};
    const Id first=ApplyGlobalRepeat(c,&inst,0,coords,bias,{},original,dx,dy);
    // Repeat within the same IR block: second Phi must use the first merge as predecessor.
    const Id result=ApplyGlobalRepeat(c,&inst,0,coords,bias,{},first,dx,dy);
    c.OpStore(output,result);c.OpReturn();c.OpFunctionEnd();
    std::vector<Id> entry_interfaces{input,output};
    if(version>=0x00010400) { entry_interfaces.push_back(t.id);entry_interfaces.push_back(c.samplers[0]);entry_interfaces.insert(entry_interfaces.end(),c.interfaces.begin(),c.interfaces.end()); }
    c.AddEntryPoint(spv::ExecutionModel::Fragment,fn,"main",std::span<const Id>(entry_interfaces));
    auto words=c.Assemble();
    std::ofstream f(std::string(argv[1])+"/variant"+std::to_string(variant)+".spv",std::ios::binary);
    f.write(reinterpret_cast<char*>(words.data()),words.size()*sizeof(u32));
 }
}
