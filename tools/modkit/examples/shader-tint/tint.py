# Example: make a shader mod from the dumped shaders. Every fragment shader that writes colour has its green and blue
# outputs replaced by 0, so the picture turns single coloured. It shows the whole route: disassemble, edit the text, assemble.
# usage: python tint.py DUMPDIR OUTDIR     (OUTDIR is the files/shaders folder of a mod; needs spirv-dis and spirv-as on PATH)
import re,subprocess,sys,os,glob
src,dst=sys.argv[1],sys.argv[2]
os.makedirs(dst,exist_ok=True)
n=0
for f in glob.glob(os.path.join(src,'fs_*_0.spv')):
    t=subprocess.run(['spirv-dis',f],capture_output=True,text=True).stdout
    if '%f32_id_0 = OpConstant' not in t: continue
    pat=re.compile(r'(%\d+) = OpAccessChain %output_f32 %frag_color0 %u32_id_([12])\n(\s+)OpStore \1 (%\S+)')
    t2,k=pat.subn(lambda m: f'{m.group(1)} = OpAccessChain %output_f32 %frag_color0 %u32_id_{m.group(2)}\n{m.group(3)}OpStore {m.group(1)} %f32_id_0',t)
    if not k: continue
    out=os.path.join(dst,os.path.basename(f))
    r=subprocess.run(['spirv-as','--target-env','spv1.6','-o',out,'-'],input=t2,text=True,capture_output=True)
    if r.returncode==0: n+=1
    else: print('fail',f,r.stderr[:100])
print('patched',n)
