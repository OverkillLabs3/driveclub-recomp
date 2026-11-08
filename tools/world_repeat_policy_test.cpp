#include "shader_recompiler/backend/spirv/world_repeat_policy.h"
#include <cstdio>
#include <memory>
using namespace Shader;
using namespace Shader::IR;
using namespace Shader::Backend::SPIRV;
static std::array<u32, 16> ud{};
struct Fixture {
    Info info{HwStage::Fragment, SwStage::Fragment, ShaderParams{ud, {}, 0xf44ca5e6}};
    Common::ObjectPool<Inst> pool;
    Block block{pool};
    Program program{info};
    Inst *colour{}, *normal{}, *mask{}, *scale{}, *uv{}, *colExtract{};
    Fixture(bool scaled = true, bool alpha = false, bool mismatched = false) {
        program.blocks.push_back(&block);
        info.stores.Set(Attribute::RenderTarget2);
        for (unsigned i = 0; i < 3; ++i) {
            AmdGpu::Image image{};
            image.type = unsigned(AmdGpu::ImageType::Color2D);
            image.width = 1023;
            image.height = 1023;
            image.last_level = 10;
            image.data_format =
                unsigned(i == 1 ? AmdGpu::DataFormat::FormatBc5 : AmdGpu::DataFormat::FormatBc1);
            image.num_format =
                unsigned(i == 0 ? AmdGpu::NumberFormat::Srgb : AmdGpu::NumberFormat::Unorm);
            ImageResource r{};
            std::memcpy(r.sharp_fetch.immediates.data(), &image, sizeof(image));
            info.images.push_back(r);
            SamplerResource s{};
            info.samplers.push_back(s); // Wrap=0, normalized.
        }
        auto* attr = add(Opcode::GetAttribute, {Value(Attribute::Param0), Value(0u), Value(0u)});
        scale = add(Opcode::ReadConstBuffer, {Value(1u), Value(4u)});
        auto* x = scaled ? add(Opcode::FPMul32, {Value(attr), Value(scale)}) : attr;
        uv = add(Opcode::CompositeConstructF32x2, {Value(x), Value(x)});
        auto* uv2 = add(Opcode::CompositeConstructF32x2, {Value(x), Value(x)});
        if (mismatched)
            uv2 = add(Opcode::CompositeConstructF32x2, {Value(attr), Value(attr)});
        colour = add(Opcode::ImageSampleImplicitLod, {Value(0u), Value(uv), Value(), Value()});
        normal = add(Opcode::ImageSampleImplicitLod, {Value(65537u), Value(uv2), Value(), Value()});
        mask = add(Opcode::ImageSampleImplicitLod, {Value(131074u), Value(uv), Value(), Value()});
        colExtract = add(Opcode::CompositeExtractF32x4, {Value(colour), Value(alpha ? 3u : 0u)});
        auto* n = add(Opcode::CompositeExtractF32x4, {Value(normal), Value(0u)});
        auto* m = add(Opcode::CompositeExtractF32x4, {Value(mask), Value(3u)});
        auto* blend = add(Opcode::FPMul32, {Value(colExtract), Value(m)});
        add(Opcode::SetAttribute, {Value(Attribute::RenderTarget0), Value(blend), Value(0u)});
        add(Opcode::SetAttribute, {Value(Attribute::RenderTarget1), Value(n), Value(0u)});
    }
    Inst* add(Opcode op, std::initializer_list<Value> args) {
        block.AppendNewInst(op, args);
        return &block.back();
    }
    void format(unsigned image, AmdGpu::DataFormat f) {
        AmdGpu::Image sharp{};
        std::memcpy(&sharp, info.images[image].sharp_fetch.immediates.data(), sizeof(sharp));
        sharp.data_format = unsigned(f);
        std::memcpy(info.images[image].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    void number_format(unsigned image, AmdGpu::NumberFormat format) {
        AmdGpu::Image sharp{};
        std::memcpy(&sharp, info.images[image].sharp_fetch.immediates.data(), sizeof(sharp));
        sharp.num_format = unsigned(format);
        std::memcpy(info.images[image].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
    }
    void expect(const char* name, std::initializer_list<const Inst*> expected) {
        WorldRepeat::Prepare(program);
        const std::unordered_set<const Inst*> wanted(expected);
        const bool okay = WorldRepeat::selected == wanted;
        std::printf("%s %s (%zu samples)\n", okay ? "PASS" : "FAIL", name,
                    WorldRepeat::selected.size());
        if (!okay)
            std::exit(1);
    }
    void check(const char* name, bool accepted) {
        WorldRepeat::Prepare(program);
        bool okay = accepted ? WorldRepeat::selected.contains(colour) &&
                                   WorldRepeat::selected.contains(normal) &&
                                   !WorldRepeat::selected.contains(mask)
                             : WorldRepeat::selected.empty();
        std::printf("%s %s (%zu samples)\n", okay ? "PASS" : "FAIL", name,
                    WorldRepeat::selected.size());
        if (!okay)
            std::exit(1);
    }
};
// The interior fixture uses the real all-owner leather cohort hash. Its
// independent albedo/mask controls must stay original as the grain is selected.
struct InteriorFixture : Fixture {
    InteriorFixture() {
        info.pgm_hash = 0xc6c425e2;
        auto* y = add(Opcode::CompositeExtractF32x4, {Value(normal), Value(1u)});
        add(Opcode::SetAttribute, {Value(Attribute::RenderTarget1), Value(y), Value(1u)});
    }
    void scalar_ao() {
        format(0, AmdGpu::DataFormat::FormatBc4);
        number_format(0, AmdGpu::NumberFormat::Unorm);
    }
    void fabric(AmdGpu::DataFormat format_value) {
        info.pgm_hash = 0x97a6899c;
        format(1, format_value);
        number_format(1, AmdGpu::NumberFormat::Srgb);
        for (auto& inst : block.Instructions()) {
            if (inst.GetOpcode() != Opcode::SetAttribute ||
                inst.Arg(0).Attribute() != Attribute::RenderTarget1)
                continue;
            const Value raw = inst.Arg(1);
            auto* twice = add(Opcode::FPMul32, {Value(2.f), raw});
            auto* decoded = add(Opcode::FPAdd32, {Value(twice), Value(-1.f)});
            inst.SetArg(1, Value(decoded));
        }
    }
};
void interior_checks() {
    {
        InteriorFixture f;
        f.expect("interior grain only; albedo and alpha mask unchanged", {f.normal});
    }
    {
        InteriorFixture f;
        f.scalar_ao();
        f.expect("interior grain and same-UV BC4 scalar AO", {f.colour, f.normal});
    }
    {
        InteriorFixture f;
        f.scalar_ao();
        auto* a = f.add(Opcode::GetAttribute, {Value(Attribute::Param1), Value(0u), Value(0u)});
        auto* other = f.add(Opcode::FPMul32, {Value(a), Value(f.scale)});
        f.colour->SetArg(1, Value(f.add(Opcode::CompositeConstructF32x2,
                                     {Value(other), Value(other)})));
        f.expect("AO on different UV remains original", {f.normal});
    }
    {
        InteriorFixture f;
        auto* alpha = f.add(Opcode::CompositeExtractF32x4, {Value(f.normal), Value(3u)});
        f.add(Opcode::SetAttribute, {Value(Attribute::RenderTarget1), Value(alpha), Value(2u)});
        f.expect("alpha-dependent normal rejected", {});
    }
    {
        InteriorFixture f;
        auto* attr = f.add(Opcode::GetAttribute, {Value(Attribute::Param0), Value(0u), Value(0u)});
        f.normal->SetArg(1, Value(f.add(Opcode::CompositeConstructF32x2,
                                      {Value(attr), Value(attr)})));
        f.expect("unscaled stitch/unique UV normal rejected", {});
    }
    {
        InteriorFixture f;
        auto* square = f.add(Opcode::FPMul32, {f.uv->Arg(0), f.uv->Arg(0)});
        f.normal->SetArg(1, Value(f.add(Opcode::CompositeConstructF32x2,
                                      {Value(square), Value(square)})));
        f.expect("nonlinear varying product rejected in interior UV", {});
    }
    for (const auto format : {AmdGpu::DataFormat::FormatBc1, AmdGpu::DataFormat::FormatBc7}) {
        InteriorFixture f;
        f.fabric(format);
        f.expect("fabric signed-RG decode; albedo/alpha retained", {f.normal});
    }
    {
        InteriorFixture f;
        f.info.pgm_hash = 0x97a6899c;
        f.format(1, AmdGpu::DataFormat::FormatBc7);
        f.number_format(1, AmdGpu::NumberFormat::Srgb);
        f.expect("BC7 without signed-RG normal decode rejected", {});
    }
    {
        InteriorFixture f;
        f.fabric(AmdGpu::DataFormat::FormatBc7);
        auto* raw = f.add(Opcode::CompositeExtractF32x4, {Value(f.normal), Value(0u)});
        f.add(Opcode::SetAttribute, {Value(Attribute::RenderTarget1), Value(raw), Value(2u)});
        f.expect("fabric decoded plus parallel raw normal path rejected", {});
    }
    {
        InteriorFixture f;
        f.fabric(AmdGpu::DataFormat::FormatBc7);
        auto* alpha = f.add(Opcode::CompositeExtractF32x4, {Value(f.normal), Value(3u)});
        f.add(Opcode::SetAttribute, {Value(Attribute::RenderTarget1), Value(alpha), Value(2u)});
        f.expect("fabric alpha normal decode rejected", {});
    }
    {
        InteriorFixture f;
        f.fabric(AmdGpu::DataFormat::FormatBc7);
        auto* raw_colour = f.add(Opcode::CompositeExtractF32x4, {Value(f.normal), Value(0u)});
        f.add(Opcode::SetAttribute, {Value(Attribute::RenderTarget0), Value(raw_colour), Value(2u)});
        f.expect("fabric mixed direct albedo path rejected", {});
    }
    {
        InteriorFixture f;
        f.fabric(AmdGpu::DataFormat::FormatBc7);
        auto* control = f.add(Opcode::CompositeExtractF32x4, {Value(f.normal), Value(3u)});
        f.add(Opcode::SetAttribute, {Value(Attribute::RenderTarget2), Value(control), Value(3u)});
        f.expect("fabric separate original-alpha control accepted", {f.normal});
    }
    {
        InteriorFixture f;
        f.scalar_ao();
        f.add(Opcode::DiscardCond, {Value(true)});
        f.expect("interior cutout rejected", {});
    }
    {
        InteriorFixture f;
        f.info.pgm_hash = 0x945a7de5; // Interior paint is outside the cohort.
        f.expect("interior paint family rejected", {});
    }
    {
        InteriorFixture f;
        f.info.pgm_hash = 0x8d9dd0a2; // Shared with glass/paint/world/other families.
        f.expect("mixed material owners rejected", {});
    }
    {
        InteriorFixture f;
        f.info.samplers[1].is_depth = true;
        f.expect("interior comparison sampler rejected", {});
    }
    {
        InteriorFixture f;
        f.info.samplers[1].sharp_fetch.immediates[0] =
            unsigned(AmdGpu::ClampMode::ClampLastTexel);
        f.expect("interior clamped atlas rejected", {});
    }
}
int main() {
    interior_checks();
    {
        Fixture f;
        f.check("paired scaled world colour/normal; alpha mask retained", true);
    }
    {
        Fixture f(false);
        f.check("unique unscaled mesh UV", false);
    }
    {
        Fixture f(true, true);
        f.check("alpha-only texture is not albedo", false);
    }
    {
        Fixture f(true, false, true);
        f.check("normal UV differs", false);
    }
    {
        Fixture f;
        f.info.pgm_hash = 0;
        f.check("non-world program", false);
    }
    {
        Fixture f;
        f.add(Opcode::Discard, {});
        f.check("cutout discard", false);
    }
    {
        Fixture f;
        f.add(Opcode::DiscardCond, {Value(true)});
        f.check("conditional discard", false);
    }
    {
        Fixture f;
        f.info.samplers[0].sharp_fetch.immediates[0] = unsigned(AmdGpu::ClampMode::ClampLastTexel);
        f.check("clamped atlas axis", false);
    }
    {
        Fixture f;
        f.format(1, AmdGpu::DataFormat::FormatBc7);
        f.check("unconfirmed normal format", false);
    }
    {
        Fixture f;
        auto* fract = f.add(Opcode::FPFract32, {f.uv->Arg(0)});
        f.uv->SetArg(0, Value(fract));
        f.check("non-affine UV or atlas", false);
    }
    {
        Fixture f;
        f.colour->SetArg(3, Value(1u));
        f.check("instruction sample offset", false);
    }
    {
        Fixture f;
        f.info.samplers[0].is_depth = true;
        f.check("comparison sampler", false);
    }
    {
        Fixture f;
        f.info.sw_stage = SwStage::Vertex;
        f.check("non-fragment stage", false);
    }
    {
        Fixture f;
        Value x = f.uv->Arg(0);
        for (int i = 0; i < 40; ++i)
            x = Value(f.add(Opcode::FPMul32, {x, x}));
        f.uv->SetArg(0, x);
        f.check("bounded expansion of shared SSA DAG", false);
    }
    {
        Fixture f;
        AmdGpu::Image sh{};
        std::memcpy(&sh, f.info.images[0].sharp_fetch.immediates.data(), sizeof(sh));
        sh.last_level = 0;
        std::memcpy(f.info.images[0].sharp_fetch.immediates.data(), &sh, sizeof(sh));
        f.check("no mip chain", false);
    }
    std::puts("World and interior production-policy regression checks passed.");
    return 0;
}
