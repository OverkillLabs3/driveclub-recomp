#include "../gpu/shadps4/shader_recompiler/backend/spirv/native_cube_coords.h"
#include <array>
#include <fstream>
#include <sirit/sirit.h>
using Sirit::Id;
struct Module : Sirit::Module {
    std::array<Id, 5> F32{};
    std::array<Id, 2> U1{};
    Id u32;
    Id storage_type;
    bool storage_defined = false;
    Module() : Sirit::Module(0x00010300) {
        SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::SampledCubeArray);
        F32[1] = TypeFloat(32);
        F32[3] = TypeVector(F32[1], 3);
        F32[4] = TypeVector(F32[1], 4);
        U1[1] = TypeBool();
        u32 = TypeInt(32, false);
    }
    Id ConstF32(float f) { return Constant(F32[1], f); }
    Id ConstU32(unsigned u) { return Constant(u32, u); }
    Id binding(Id type, unsigned n) {
        Id v = AddGlobalVariable(TypePointer(spv::StorageClass::UniformConstant, type),
                                 spv::StorageClass::UniformConstant);
        Decorate(v, spv::Decoration::DescriptorSet, 0u);
        Decorate(v, spv::Decoration::Binding, n);
        return v;
    }
    Id storage(unsigned n) {
        if (!storage_defined) {
            Id a = TypeRuntimeArray(F32[4]);
            Decorate(a, spv::Decoration::ArrayStride, 16u);
            storage_type = TypeStruct(a);
            Decorate(storage_type, spv::Decoration::Block);
            MemberDecorate(storage_type, 0u, spv::Decoration::Offset, 0u);
            storage_defined = true;
        }
        Id v = AddGlobalVariable(TypePointer(spv::StorageClass::StorageBuffer, storage_type),
                                 spv::StorageClass::StorageBuffer);
        Decorate(v, spv::Decoration::DescriptorSet, 0u);
        Decorate(v, spv::Decoration::Binding, n);
        return v;
    }
};
int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    Module m;
    Id array_type =
        m.TypeImage(m.F32[1], spv::Dim::Dim2D, false, true, false, 1, spv::ImageFormat::Unknown);
    Id cube_type =
        m.TypeImage(m.F32[1], spv::Dim::Cube, false, true, false, 1, spv::ImageFormat::Unknown);
    Id sa = m.TypeSampledImage(array_type), sc = m.TypeSampledImage(cube_type);
    Id array = m.binding(sa, 0), cube = m.binding(sc, 1), input = m.storage(2),
       output = m.storage(3);
    Id uvec3 = m.TypeVector(m.u32, 3);
    Id gid = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, uvec3),
                                 spv::StorageClass::Input);
    m.Decorate(gid, spv::Decoration::BuiltIn, spv::BuiltIn::GlobalInvocationId);
    Id main = m.OpFunction(m.TypeVoid(), spv::FunctionControlMask::MaskNone,
                           m.TypeFunction(m.TypeVoid()));
    m.AddLabel();
    Id index = m.OpCompositeExtract(m.u32, m.OpLoad(uvec3, gid), 0);
    Id vecptr = m.TypePointer(spv::StorageClass::StorageBuffer, m.F32[4]);
    Id data = m.OpLoad(m.F32[4], m.OpAccessChain(vecptr, input, m.ConstU32(0), index));
    Id coords = m.OpVectorShuffle(m.F32[3], data, data, 0u, 1u, 2u);
    Id lod = m.OpCompositeExtract(m.F32[1], data, 3);
    Id old = m.OpImageSampleExplicitLod(m.F32[4], m.OpLoad(sa, array), coords,
                                        spv::ImageOperandsMask::Lod, lod);
    Id direction = Shader::Backend::SPIRV::NativeCubeCoordinates(m, coords);
    Id fixed = m.OpImageSampleExplicitLod(m.F32[4], m.OpLoad(sc, cube), direction,
                                          spv::ImageOperandsMask::Lod, lod);
    Id first = m.OpIMul(m.u32, index, m.ConstU32(2));
    m.OpStore(m.OpAccessChain(vecptr, output, m.ConstU32(0), first), old);
    m.OpStore(m.OpAccessChain(vecptr, output, m.ConstU32(0), m.OpIAdd(m.u32, first, m.ConstU32(1))),
              fixed);
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", gid);
    m.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1u, 1u, 1u);
    auto code = m.Assemble();
    std::ofstream f(argv[1], std::ios::binary);
    f.write((char*)code.data(), code.size() * 4);
}
