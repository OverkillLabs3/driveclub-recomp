#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production repeat sampler in a small Sirit adapter and verify its CFG.

Requires Python3.10+, a C++23 compiler compatible with the Sirit static library,
Sirit built from the source tree, and SPIRV-Tools (spirv-val/spirv-dis). No assets,
GPU, Vulkan driver, Pillow or nonstandard Python package is used. See --help.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def check_variant(assembly,variant):
    lines=[line.split() for line in assembly.splitlines() if line.split()]
    definitions={t[0]:t[2:] for t in lines if len(t)>2 and t[1]=='='}
    defs=definitions;blocks={};locations={};current=None
    for t in lines:
        if len(t)>2 and t[2]=='OpLabel':current=t[0];blocks[current]=[]
        elif current:
            blocks[current].append(t)
            if len(t)>2 and t[1]=='=':locations[t[0]]=current
    assert not any('OpLoopMerge' in t for t in lines),'No new sampling loops'
    assert not any('OpImageQueryLevels' in t or 'InverseSqrt' in t for t in lines),'Global mean/gain returned'
    samples=[t for t in lines if 'OpImageSampleExplicitLod' in t or 'OpImageSampleImplicitLod' in t]
    assert len(samples)==9,'Original +4 reads per expansion required'
    def number(i,n):
        d=defs[i];return d[0]=='OpConstant' and float(d[2])==n
    joins=[t for t in lines if len(t)>3 and t[2]=='OpPhi' and t[3]=='%v4float']
    assert len(joins)==2
    for phi in joins:
        value,body,original,before=phi[4:8];insert=defs[value]
        assert insert[0]=='OpCompositeInsert' and insert[-1]=='3'
        assert defs[insert[2]]==['OpCompositeExtract','%float',original,'3']
        mix=defs[insert[3]];assert mix[0]=='OpExtInst' and mix[3]=='FMix' and mix[4]==original
        splat=defs[mix[6]];assert splat[0]=='OpCompositeConstruct' and len(set(splat[2:]))==1
        branch=blocks[before][-1];assert branch[0]=='OpBranchConditional' and branch[2]==body and branch[3]==locations[phi[0]]
        condition=defs[branch[1]];assert condition[0]=='OpFOrdGreaterThan' and number(condition[3],0) and condition[2]==splat[2]
        assert all(not any(op.startswith('OpDP') for op in t) for t in blocks[body]),'Derivatives entered added branch'
        reads=[];weights=[];node=mix[5]
        for _ in range(4):
            add=defs[node];assert add[0]=='OpFAdd';term=defs[add[3]];assert term[0]=='OpVectorTimesScalar'
            sample_id,weight_id=term[2:4];sample=defs[sample_id]
            assert sample[0]=='OpImageSampleExplicitLod' and sample[-3]=='Grad' and locations[sample_id]==body
            assert defs[sample[2]][0]=='OpSampledImage' and locations[sample[2]]==body
            weight=defs[weight_id];assert weight[0]=='OpFDiv';weights.append(weight)
            assert defs[weight[2]][0]=='OpExtInst' and defs[weight[2]][3]=='Pow' and number(defs[weight[2]][5],8)
            for gradient in sample[-2:]:
                derivative=defs[gradient]
                if derivative[0]=='OpVectorTimesScalar':derivative=defs[derivative[2]]
                assert derivative[0] in ('OpDPdx','OpDPdy')
            if variant&1:
                coord=defs[sample[3]];assert coord[0]=='OpCompositeConstruct' and coord[1]=='%v3float'
                assert defs[coord[4]][0]=='OpCompositeExtract' and defs[coord[4]][-1]=='2'
            reads.append(sample_id);node=add[2]
        assert defs[node]==['OpConstantNull','%v4float']
        denominator=weights[0][3];assert all(w[3]==denominator for w in weights)
        terms=[];node=denominator
        for _ in range(4):
            add=defs[node];assert add[0]=='OpFAdd';terms.append(add[3]);node=add[2]
        assert number(node,0) and set(terms)=={w[2] for w in weights}
        assert len(set(reads))==4
    assert sum(d[0]=='OpExtInst' and d[3]=='Pow' for d in defs.values())==8
    return {'variant':variant,'expansions':2,'added_Grad_reads':8,'no_loops_or_mean_gain':True,'convex_normalization':True,'original_alpha':True,'zero_amount_original':True,'derivatives_outside_branches':True,'period_contract':'Inherited Wrap1/Mirror2'}


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


def check_rejected_mutations(assembly):
    import re
    mutations={
      'restore_gain':assembly.replace(' FMix ', ' InverseSqrt '),
      'restore_mean_Lod_read':assembly.replace(' Grad ', ' Lod '),
      'break_alpha':re.sub(r'(OpCompositeInsert %v4float %\S+ %\S+) 3',r'\1 2',assembly,count=1),
      'break_normalization':assembly.replace('OpFDiv','OpFMul'),
      'change_power':assembly.replace('OpConstant %float 8','OpConstant %float 4'),
    }
    for name,changed in mutations.items():
        assert changed!=assembly
        try:check_variant(changed,0)
        except (AssertionError,KeyError,IndexError,StopIteration,ValueError):continue
        raise AssertionError('Accepted regression: '+name)
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
    reports = []
    for variant in range(32):
        module = out / f'variant{variant}.spv'
        assembly = out / f'variant{variant}.spvasm'
        target = 'vulkan1.1' if variant % 8 < 4 else 'vulkan1.2'
        run([validator, '--target-env', target, module], out / f'variant{variant}-validation.log', env)
        run([disassembler, module, '-o', assembly], out / f'variant{variant}-disassembly.log', env)
        reports.append(check_variant(assembly.read_text(encoding='utf-8'), variant))
    rejected = check_rejected_mutations((out / 'variant0.spvasm').read_text(encoding='utf-8'))
    receipt = {'pass': True, 'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
               'scope': 'Actual production helper compiled into Sirit adapter; SPIR-V validation and CFG/dataflow regression checks. No GPU execution or visual/performance qualification.',
               'sampler_mode_coverage': 'All four Wrap/Mirror axis combinations', 'rejected_negative_controls': rejected, 'variants': reports}
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
