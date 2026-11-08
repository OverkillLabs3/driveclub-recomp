#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production repeat sampler in a small Sirit adapter and verify its CFG.

Requires Python3.10+, a C++23 compiler compatible with the Sirit static library,
Sirit built from the source tree, and SPIRV-Tools (spirv-val/spirv-dis). No assets,
GPU, Vulkan driver, Pillow or nonstandard Python package is used. See --help.
"""
from pathlib import Path
from decimal import Decimal, localcontext, ROUND_CEILING
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def poisson_thresholds():
    # Independent distribution specification, not copied emitter constants.
    with localcontext() as dec:
        dec.prec = 70
        probability = Decimal('-.5').exp()
        cumulative = Decimal(0)
        result = []
        for count in range(10):
            if count:
                probability *= Decimal('.5') / count
            cumulative += probability
            result.append(int((cumulative*(1 << 32)-Decimal('.5')).to_integral_value(rounding=ROUND_CEILING)))
        return result


def check_variant(assembly, variant, expected):
    lines=assembly.splitlines();defs={};blocks={};loc={};cur=None;entry=[]
    for line in lines:
        t=line.split()
        if not t:continue
        if t[0]=='OpEntryPoint':entry=t[4:]
        if len(t)>2 and t[1]=='=':defs[t[0]]=t[2:]
        if 'OpLabel' in t:cur=t[0];blocks[cur]=[]
        elif cur:
            blocks[cur].append(t)
            if len(t)>2 and t[1]=='=':loc[t[0]]=cur
    def const(i):
        d=defs[i];return int(d[2]) if d[0]=='OpConstant' and d[1]=='%uint' else None
    def zero(i):return defs[i][0]=='OpConstant' and defs[i][-1]=='0'
    def number(i, value):
        d=defs[i];return d[0]=='OpConstant' and float(d[2])==value
    def load(i):
        d=defs[i];assert d[0]=='OpLoad';return d[2]
    def successors(b):
        t=blocks[b][-1]
        if t[0]=='OpBranch':return t[1:]
        if t[0]=='OpBranchConditional':return t[2:4]
        return []
    loops=[]
    for b,ts in blocks.items():
        m=next((x for x in ts if x[0]=='OpLoopMerge'),None)
        if not m:continue
        merge,cont=m[1:3];branch=ts[-1];assert branch[0]=='OpBranchConditional';condition=defs[branch[1]];assert condition[0]=='OpULessThan'
        region=set();pending=[b]
        while pending:
            n=pending.pop()
            if n==merge or n in region:continue
            region.add(n);pending+=successors(n)
        loops.append({'header':b,'merge':merge,'continue':cont,'region':region,'bound':const(condition[3])})
    assert len(loops)==4 and sum(x['bound']==16 for x in loops)==2
    # A complete shifted 4x4 catalogue, not a nearest-site list or fixed stratum.
    for loop in loops:
        condition=defs[blocks[loop['header']][-1][1]]
        index_id=condition[2];index_state=load(index_id)
        assert any(t==['OpStore',index_state,next_id] and defs[next_id]==['OpIAdd','%uint',index_id,'%uint_1'] for t in blocks[loop['continue']] for next_id in t[2:3])
        assert any(t==['OpStore',index_state,'%uint_0'] for block,ts in blocks.items() if block not in loop['region'] for t in ts),'Loop state must reset before use'
        if loop['bound']==16:
            index=condition[2]
            for op in ['OpUMod','OpUDiv']:
                axis=[i for i,d in defs.items() if d[0]==op and d[2]==index and number(d[3],4)]
                assert len(axis)==1,'Incomplete cell catalogue'
        else:
            # Execute the emitted scalar count CFG at all interval boundaries.
            outer=next(o for o in loops if o['bound']==16 and loop['header'] in o['region'])
            start=blocks[outer['header']][-1][2]
            hash_ids={d[2] for d in defs.values() if d[0] in ('OpULessThan','OpUGreaterThanEqual') and const(d[3]) in expected}
            probes={0,0xffffffff}
            for threshold in expected:
                probes.update([threshold-1,threshold,min(0xffffffff,threshold+1)])
            probes.update((i*2654435761)&0xffffffff for i in range(1024))
            for h in probes:
                entered={};block=start
                def evaluate(identifier):
                    if identifier in hash_ids:return h
                    d=defs[identifier];op=d[0]
                    if op=='OpConstant':return int(d[2])
                    if op=='OpPhi':
                        pred=entered[loc[identifier]]
                        return evaluate(next(d[i] for i in range(2,len(d),2) if d[i+1]==pred))
                    if op=='OpSelect':return evaluate(d[3] if evaluate(d[2]) else d[4])
                    x,y=evaluate(d[2]),evaluate(d[3])
                    if op=='OpULessThan':return x<y
                    if op=='OpUGreaterThanEqual':return x>=y
                    if op=='OpIAdd':return (x+y)&0xffffffff
                    raise AssertionError('Unexpected count operation: '+str(d))
                for step in range(20):
                    if block==loop['header']:break
                    term=blocks[block][-1]
                    next_block=term[1] if term[0]=='OpBranch' else term[2] if evaluate(term[1]) else term[3]
                    entered[next_block]=block;block=next_block
                assert block==loop['header']
                assert evaluate(condition[3])==sum(h>=t for t in expected),'CDF count changed'
    texture_ops=[(b,t) for b,ts in blocks.items() for t in ts if 'OpImageSampleExplicitLod' in t or 'OpImageSampleImplicitLod' in t]
    extra=[(b,t) for b,t in texture_ops if sum(b in loop['region'] for loop in loops)==2]
    assert len(texture_ops)==3 and len(extra)==2,'Sample expansion was unrolled or original moved into loops'
    assert not any('OpImageQueryLevels' in t for b,ts in blocks.items() for t in ts)
    accumulators=[]
    for b,t in extra:
        assert 'Grad' in t and 'OpImageSampleExplicitLod' in t
        for gradient in t[-2:]:
            derivative=defs[gradient]
            if derivative[0]=='OpVectorTimesScalar':derivative=defs[derivative[2]]
            assert derivative[0] in ('OpDPdx','OpDPdy'),'Added reads must use original unshifted gradients'
        assert defs[t[4]][0]=='OpSampledImage' and loc[t[4]]==b
        coord=defs[t[5]]
        if variant&1:
            assert coord[0]=='OpCompositeConstruct' and coord[1]=='%v3float'
            layer=defs[coord[4]];assert layer[0]=='OpCompositeExtract' and layer[-1]=='2'
            shifted=defs[coord[2]][2]
        else:shifted=t[5]
        add=defs[shifted];assert add[0]=='OpFAdd'
        offsets=defs[add[3]];assert offsets[0]=='OpCompositeConstruct'
        for axis in range(2):
            mul=defs[offsets[2+axis]];assert mul[0]=='OpFMul'
            factor=float(defs[mul[3]][2]);expected_factor=2.0
            assert factor==expected_factor
        pred=[(h,ts[-1]) for h,ts in blocks.items() if ts[-1][0]=='OpBranchConditional' and ts[-1][2]==b];assert len(pred)==1
        header,branch=pred[0];cmp=defs[branch[1]];assert cmp[0]=='OpFOrdGreaterThan' and zero(cmp[3]);weight=defs[cmp[2]];assert weight[0]=='OpFMul' and weight[2]==weight[3]
        raw=defs[weight[2]];assert raw[0]=='OpExtInst' and 'FMax' in raw
        # K=(max(0,1-dot(delta,delta)/2))^2: compact support and C1 edge.
        assert zero(raw[4]);radius=defs[raw[5]];assert radius[0]=='OpFSub' and number(radius[2],1)
        half_distance=defs[radius[3]];assert half_distance[0]=='OpFMul' and number(half_distance[3],.5)
        distance=defs[half_distance[2]];assert distance[0]=='OpDot' and distance[2]==distance[3]
        delta=defs[distance[2]];assert delta[0]=='OpFSub'
        point=defs[delta[2]];assert point[0]=='OpFAdd'
        offset=defs[point[2]];assert offset[0]=='OpCompositeConstruct'
        for axis in range(2):
            add=defs[offset[axis+2]];assert add[0]=='OpFAdd'
            convert=defs[add[2]];assert convert[0]=='OpConvertUToF'
            index=defs[convert[2]];assert index[0]==('OpUMod' if axis==0 else 'OpUDiv') and number(index[3],4)
            lower_component=defs[add[3]];assert lower_component[0]=='OpCompositeExtract' and lower_component[3]==str(axis)
            lower=defs[lower_component[2]];assert lower[0]=='OpExtInst' and lower[3]=='Floor'
            shifted=defs[lower[4]];assert shifted[0]=='OpFSub' and shifted[2]==delta[3]
            radius_pair=defs[shifted[3]];assert radius_pair[0]=='OpCompositeConstruct'
            assert all(abs(float(defs[i][2])-float.fromhex('0x1.6a09e8p+0'))<1e-8 for i in radius_pair[2:])

        merge=branch[3];phis=[x for x in blocks[merge] if len(x)>2 and x[2]=='OpPhi'];assert len(phis)==3
        for phi in phis:assert phi[5]==b and phi[7]==header
        assert sum(x[0]=='OpStore' for x in blocks[merge])==3
        value_phi=next(p for p in phis if p[3]=='%v4float')
        value_add=defs[value_phi[4]];assert value_add[0]=='OpFAdd' and value_add[2]==value_phi[6]
        weighted_sample=defs[value_add[3]]
        assert weighted_sample==['OpVectorTimesScalar','%v4float',t[0],cmp[2]]
        scalar_phis=[p for p in phis if p[3]=='%float']
        weight_phi=next(p for p in scalar_phis if defs[p[4]][0]=='OpFAdd')
        weight_add=defs[weight_phi[4]];assert weight_add[2]==weight_phi[6] and weight_add[3]==cmp[2]
        trans_phi=next(p for p in scalar_phis if defs[p[4]][0]=='OpFMul')
        trans_mul=defs[trans_phi[4]];assert trans_mul[2]==trans_phi[6]
        one_minus=defs[trans_mul[3]];assert one_minus[0]=='OpFSub' and number(one_minus[2],1) and one_minus[3]==cmp[2]
        state=(load(value_phi[6]),load(weight_phi[6]),load(trans_phi[6]))
        initial=[defs[state_id][3] for state_id in state]
        assert defs[initial[0]]==['OpConstantNull','%v4float'] and zero(initial[1]) and number(initial[2],1)
        for state_id,initial_id in zip(state,initial):
            assert any(ts.count(['OpStore',state_id,initial_id]) for block,ts in blocks.items() if not any(block in loop['region'] for loop in loops)),'Accumulators must reset for every helper expansion'
        for phi,state_id in zip([value_phi,weight_phi,trans_phi],state):
            assert ['OpStore',state_id,phi[0]] in blocks[merge]
        accumulators.append(state)
    selection_regions=[]
    for header,ts in blocks.items():
        sm=next((x for x in ts if x[0]=='OpSelectionMerge'),None)
        if not sm:continue
        merge=sm[1];branch=ts[-1];assert branch[0]=='OpBranchConditional'
        region=set();pending=[branch[2]]
        while pending:
            b=pending.pop()
            if b==merge or b in region:continue
            region.add(b);pending+=successors(b)
        selection_regions.append(region)
    for b,ts in blocks.items():
        if any(any(x.startswith('OpDP') for x in t) for t in ts):
            assert not any(b in loop['region'] for loop in loops)
            assert not any(b in region for region in selection_regions)
        if any('OpImageSampleImplicitLod' in t for t in ts):
            assert not any(b in region for region in selection_regions)
    # Empty-support/whole-effect joins select exact original SSA on each false edge.
    vectorphis=[(b,t) for b,ts in blocks.items() for t in ts if len(t)>3 and t[2]=='OpPhi' and t[3]=='%v4float']
    alpha_joins=0;outer_joins=0
    for b,t in vectorphis:
        truevalue=defs[t[4]]
        if truevalue[0]=='OpCompositeInsert':
            assert truevalue[-1]=='3';alpha=defs[truevalue[2]];assert alpha==['OpCompositeExtract','%float',t[6],'3'];alpha_joins+=1
            mix=defs[truevalue[3]];assert mix[:2]==['OpExtInst','%v4float'] and mix[3]=='FMix' and mix[4]==t[6]
            average=defs[mix[5]];assert average[0]=='OpVectorTimesScalar'
            reciprocal=defs[average[3]];assert reciprocal[0]=='OpFDiv' and number(reciprocal[2],1)
            splat=defs[mix[6]];assert splat[0]=='OpCompositeConstruct' and len(set(splat[2:]))==1
            blend=defs[splat[2]];assert blend[0]=='OpFMul'
            coverage=defs[blend[3]];assert coverage[0]=='OpFSub' and number(coverage[2],1)
            assert (load(average[2]),load(reciprocal[3]),load(coverage[3])) in accumulators,'Mean/gain or wrong accumulator entered output'
            branch=blocks[t[7]][-1];cmp=defs[branch[1]];assert cmp[0]=='OpFOrdGreaterThan' and zero(cmp[3]);assert defs[cmp[2]][0]=='OpLoad'
            assert cmp[2]==reciprocal[3],'Division must be guarded by the same positive W'
        if truevalue[0]=='OpPhi' and truevalue[1]=='%v4float':
            assert truevalue[4]==t[6] # empty-support original and outer-zero original match
            branch=blocks[t[7]][-1];cmp=defs[branch[1]];assert cmp[0]=='OpFOrdGreaterThan' and zero(cmp[3]);outer_joins+=1
    assert alpha_joins==2 and outer_joins==2
    private=[i for i,d in defs.items() if d[0]=='OpVariable' and d[2]=='Private'];assert len(private)==10
    assert all((i in entry)==((variant%8)>=4) for i in private)
    thresholds=[const(d[3]) for d in defs.values() if d[0] in ('OpULessThan','OpUGreaterThanEqual') and const(d[3]) in expected];assert sorted(thresholds)==sorted(expected*2)
    return {'variant':variant,'spirv':'1.3' if (variant%8)<4 else '1.4','loops':4,'static_texture_instructions':3,'conditional_site_Grad_instructions':2,'sequential_helper_expansions':2,'loop_support_K_positive_only':True,'zero_and_empty_return_original':True,'original_alpha_preserved':True,'derivatives_outside_all_new_branches_and_loops':True,'private_state_variables':10,'private_interfaces_correct':True,'array_layer_preserved':True,'wrap_x':'Mirror' if (variant//8)&1 else 'Wrap','wrap_y':'Mirror' if (variant//8)&2 else 'Wrap'}


def extract_helper(source):
    """Keep the production function intact, including all emitted control flow."""
    start = source.index('static Id ApplyGlobalRepeat(')
    end = source.index('\nId EmitImageSampleImplicitLod(', start)
    return source[start:end].strip()


def find_library(build_dir):
    if build_dir is None:
        raise ValueError('Supply --sirit-lib or --build-dir containing a built Sirit library')
    for name in ('libsirit.a', 'sirit.lib', 'libsirit.lib'):
        found = sorted(build_dir.rglob(name))
        if len(found) == 1:
            return found[0]
        if len(found) > 1:
            raise ValueError('Several Sirit libraries found; select one with --sirit-lib')
    raise ValueError('No built Sirit library found under --build-dir')


def tool_path(name):
    resolved = shutil.which(str(name))
    if resolved:
        return Path(resolved).resolve()
    path = Path(name).expanduser()
    if path.is_file():
        return path.resolve()
    raise ValueError(f'Tool not found: {name}')


def run(command, log, env):
    process = subprocess.run([str(x) for x in command], capture_output=True, text=True, env=env)
    log.write_text(process.stdout + process.stderr, encoding='utf-8')
    if process.returncode:
        raise RuntimeError(f'Command failed ({process.returncode}); see {log}\n{process.stdout}{process.stderr}')


def check_rejected_mutations(assembly, expected):
    """Negative controls: these emitted-kernel regressions must fail the CFG checks."""
    import re
    definitions = {}
    for line in assembly.splitlines():
        words = line.split()
        if len(words) > 2 and words[1] == '=':
            definitions[words[0]] = words[2:]
    mix = next(d for d in definitions.values() if d[0] == 'OpExtInst' and d[3] == 'FMix')
    average = definitions[mix[5]]
    reciprocal_id = average[3]
    blend = definitions[definitions[mix[6]][2]]
    coverage_id = blend[3]
    def change_definition(identifier, old, new):
        return re.sub(r'(' + re.escape(identifier) + r' = )' + re.escape(old), r'\g<1>' + new, assembly, count=1)
    mutations = {
        'support_radius': assembly.replace('OpConstant %float 0.5', 'OpConstant %float 0.25'),
        'incomplete_catalogue': re.sub(r'(OpULessThan %bool %\S+) %\S+(\n\s+OpLoopMerge)', r'\1 %uint_1\2', assembly, count=1),
        'coverage_gain': change_definition(coverage_id, 'OpFSub', 'OpFAdd'),
        'biased_normalization': change_definition(reciprocal_id, ' '.join(definitions[reciprocal_id]), 'OpFDiv %float %float_1 %float_1'),
        'break_alpha': re.sub(r'(OpCompositeInsert %v4float %\S+ %\S+) 3', r'\1 2', assembly, count=1),
        'different_sampler_period': assembly.replace('OpConstant %float 2', 'OpConstant %float 1'),
    }
    for name, changed in mutations.items():
        assert changed != assembly, f'Negative control did not mutate: {name}'
        try:
            check_variant(changed, 0, expected)
        except (AssertionError, KeyError, IndexError, StopIteration, ValueError):
            continue
        raise AssertionError(f'CFG checker accepted negative control: {name}')
    return list(mutations)


def test(args, out):
    root = args.source_root.resolve()
    source_path = root / 'gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_image.cpp'
    source = source_path.read_text(encoding='utf-8')
    fixture = Path(__file__).with_name('repeat_sampling_fixture.cpp').read_text(encoding='utf-8')
    assert fixture.count('// PRODUCTION_FUNCTION') == 1
    harness = out / 'repeat_sampling.cpp'
    harness.write_text(fixture.replace('// PRODUCTION_FUNCTION', extract_helper(source)), encoding='utf-8')
    sirit = root / 'gpu/third_party/sirit'
    library = args.sirit_lib.resolve() if args.sirit_lib else find_library(args.build_dir)
    cxx, validator = tool_path(args.cxx), tool_path(args.spirv_val)
    disassembler = tool_path(args.spirv_dis or str(validator.with_name('spirv-dis' + validator.suffix)))
    executable = out / ('repeat_sampling.exe' if os.name == 'nt' else 'repeat_sampling')
    env = os.environ.copy()
    env['PATH'] = str(cxx.parent) + os.pathsep + env.get('PATH', '')
    command = [cxx, '-std=c++23', '-O0', harness,
               '-I' + str(sirit / 'include'), '-I' + str(sirit / 'externals/SPIRV-Headers/include'),
               *args.cxx_flag, library, '-o', executable]
    (out / 'compile-command.json').write_text(json.dumps([str(x) for x in command], indent=2), encoding='utf-8')
    run(command, out / 'compile.log', env)
    run([executable, out], out / 'fixture.log', env)
    expected = poisson_thresholds()
    reports = []
    for variant in range(32):
        module = out / f'variant{variant}.spv'
        assembly = out / f'variant{variant}.spvasm'
        target = 'vulkan1.1' if variant % 8 < 4 else 'vulkan1.2'
        run([validator, '--target-env', target, module], out / f'variant{variant}-validation.log', env)
        run([disassembler, module, '-o', assembly], out / f'variant{variant}-disassembly.log', env)
        reports.append(check_variant(assembly.read_text(encoding='utf-8'), variant, expected))
    for core in range(8):
        modules = {(out / f'variant{core+8*mode}.spv').read_bytes() for mode in range(4)}
        assert len(modules) == 1, 'Mixed Wrap/Mirror paired layers have different numeric shifts'
    rejected = check_rejected_mutations((out / 'variant0.spvasm').read_text(encoding='utf-8'), expected)
    receipt = {'pass': True, 'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
               'scope': 'Actual production helper compiled into Sirit adapter; SPIR-V validation and CFG/dataflow regression checks. No GPU execution or visual/performance qualification.',
               'mixed_wrap_mirror_modules_identical': True, 'rejected_negative_controls': rejected, 'variants': reports}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(f'PASS: 32 modules, two sequential expansions each; SPIR-V1.3/1.4, arrays, gradients, mixed Wrap/Mirror; {len(rejected)} negative controls rejected.')


def main():
    if not __debug__:
        raise RuntimeError("Run this regression test without Python -O; assertions are required")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--sirit-lib', type=Path, help='Existing Sirit static library, built with a compatible compiler')
    parser.add_argument('--build-dir', type=Path, help='Find the existing Sirit static library below this directory')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'), help='GCC/Clang-style C++23 compiler executable')
    parser.add_argument('--cxx-flag', action='append', default=[], help='Additional compiler/linker flag, repeatable; use --cxx-flag=-flag')
    parser.add_argument('--spirv-val', default='spirv-val', help='SPIRV-Tools validator executable')
    parser.add_argument('--spirv-dis', help='SPIRV-Tools disassembler; defaults to sibling of spirv-val')
    parser.add_argument('--output-dir', type=Path, help='Retain generated modules, disassembly and logs here; otherwise use a temporary directory')
    args = parser.parse_args()
    try:
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            test(args, args.output_dir.resolve())
        else:
            with tempfile.TemporaryDirectory(prefix='repeat-sampling-') as temporary:
                test(args, Path(temporary))
    except (OSError, ValueError, RuntimeError, AssertionError, KeyError, IndexError, StopIteration) as error:
        parser.exit(1, f'FAIL: {error}\n')


if __name__ == '__main__':
    main()
