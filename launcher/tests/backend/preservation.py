"""Preservation checks for an existing installation and forward-compatible settings.
Run with backend.exe, an existing installation (read-only), and a private output dir.
No game is prepared or launched. Every mutation is inside a new fixture directory.
"""
from pathlib import Path
import copy,hashlib,json,shutil,subprocess,sys,uuid,os,stat
exe=Path(sys.argv[1]).resolve();installation=Path(sys.argv[2]).resolve();out=Path(sys.argv[3]).resolve();out.mkdir(parents=True,exist_ok=True)
original_paths=[installation/'launcher.ini',*[installation/'data/user'/x for x in ['patches_enabled.txt','camera.ini','driveclub.ini','hdr.ini','toggles.txt','input_config/default.ini']]]
originals={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in original_paths if p.is_file()};checks=[]
def check(ok,name):
 assert ok,name
 checks.append(name);print('PASS',name,flush=True)
def fixture(name):
 root=out/(name+'-'+uuid.uuid4().hex[:8]);root.mkdir();shutil.copy2(exe,root/'DriveClub.Backend.exe');return root
counter=0
def call(root,action,state=None,fail=False):
 global counter
 counter+=1;response=root/f'ipc-{counter}.json';args=[str(root/'DriveClub.Backend.exe'),'--ui-'+action]
 if state is not None:
  request=root/f'request-{counter}.json';request.write_text(json.dumps(state),encoding='utf-8');args.append(str(request))
 args.append(str(response));p=subprocess.run(args,cwd=root,timeout=45,creationflags=0x08000000,capture_output=True);s=json.loads(response.read_text(encoding='utf-8'))
 if fail:assert p.returncode!=0 and s.get('Error'),s
 else:assert p.returncode==0 and not s.get('Error'),s
 return s
def ini(p):return dict(x.split('=',1) for x in p.read_text(encoding='utf-8-sig').splitlines() if '=' in x)
def tag(s,t):return next(c for c in s['Controls'] if c['Tag']==t)
def values(s):return {c['Label']:(c['Selected'],c.get('Text','')) for c in s['Controls'] if not c['Hidden']}
r=fixture('existing-user');
for p in original_paths:
 if p.is_file():
  dest=r/p.relative_to(installation);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
before=ini(r/'launcher.ini');tracked={p:hashlib.sha256((r/Path(p).relative_to(installation)).read_bytes()).hexdigest() for p in originals}
s=call(r,'state');check(all(hashlib.sha256((r/Path(p).relative_to(installation)).read_bytes()).hexdigest()==h for p,h in tracked.items()),'opening a copy of the existing 0.3.1 configuration leaves its files unchanged')
expected=values(s);saved=call(r,'apply',s);reloaded=call(r,'state');after=ini(r/'launcher.ini')
diff={k:(v,after.get(k)) for k,v in before.items() if after.get(k)!=v};check(not diff,'all existing launcher.ini keys retain their values on an unchanged Save: '+str(diff))
changed={k:(v,values(reloaded).get(k)) for k,v in expected.items() if values(reloaded).get(k)!=v};check(not changed,'all visible effective settings survive Save/restart: '+str(changed))
check(all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==h for p,h in originals.items()),'actual installation files remain untouched')
# Forward-compatible keys and a deliberately nonstandard optimization mask.
r=fixture('forward-compatible');u=r/'data/user';u.mkdir(parents=True);custom=2199023808512 | (1<<55)
(r/'launcher.ini').write_text('skip_gpu_check=1\nname=KeepMe\nfast=1\nfgen=4\nfuture_launcher_option=retain\nffb.future_option=retain\n',encoding='utf-8');(u/'toggles.txt').write_text(str(custom));(u/'camera.ini').write_text('fov=125\nfuture_camera=retain\n');(u/'driveclub.ini').write_text('show_fps=0\nfuture_overlay=retain\n');(u/'hdr.ini').write_text('paper=350\nfuture_hdr=retain\n');(u/'patches_enabled.txt').write_text('Custom compatibility workaround\n')
s=call(r,'state');s=call(r,'apply',s)
check(ini(r/'launcher.ini')['future_launcher_option']=='retain' and ini(r/'launcher.ini')['ffb.future_option']=='retain','unrecognized launcher/FFB keys survive Save')
check(ini(r/'launcher.ini')['fast']=='1' and ini(r/'launcher.ini')['fgen']=='4','unexposed legacy preferences are retained')
check(int((u/'toggles.txt').read_text())==custom,'custom optimization bits are retained on unchanged Save')
tag(s,'@twothread')['Selected']=0;s=call(r,'apply',s);check(int((u/'toggles.txt').read_text())==(custom | (1<<37)),'two-thread control changes only bit 37')
tag(s,'@twothread')['Selected']=1;s=call(r,'apply',s);check(int((u/'toggles.txt').read_text())==custom,'re-enabling two-thread preserves every unrelated bit')
# Explicit profile changes may update owned values, but must retain unrelated keys.
s['Controls'][s['PresetId']]['Selected']=s['Controls'][s['PresetId']]['Options'].index('PS4');s=call(r,'preset',s)
paper=tag(s,'@hdr_paper');paper['Selected']=400
s=call(r,'apply',s)
check(ini(u/'camera.ini').get('future_camera')=='retain' and ini(u/'driveclub.ini').get('future_overlay')=='retain' and ini(u/'hdr.ini').get('future_hdr')=='retain','camera/HDR/overlay sidecars preserve unknown keys')
check('Custom compatibility workaround' in (u/'patches_enabled.txt').read_text(),'unlisted custom patch selections are preserved')
tag(s,'@drive_fov')['Selected']=125;s=call(r,'apply',s);loaded=call(r,'state');check(loaded['Controls'][loaded['PresetId']]['Options'][loaded['Controls'][loaded['PresetId']]['Selected']]=='Custom','saved custom FOV is loaded before classifying the PS4 profile')
# A malformed optimization file must not be replaced by a fresh default mask.
(u/'toggles.txt').write_text('invalid mask');before_files={p:p.read_bytes() for p in [r/'launcher.ini',u/'toggles.txt',u/'camera.ini',u/'driveclub.ini',u/'hdr.ini',u/'patches_enabled.txt']};call(r,'apply',loaded,fail=True)
check(all(p.read_bytes()==v for p,v in before_files.items()),'invalid existing mask rejects Save without changing configuration files')
(u/'toggles.txt').write_text(str(custom))
protected=r/'launcher.ini';original_bytes=protected.read_bytes();os.chmod(protected,stat.S_IREAD)
try:
 failure=call(r,'apply',loaded,fail=True)
 check('retained' in failure['Error'] and protected.read_bytes()==original_bytes,'failed replacement leaves the previous INI intact')
finally:os.chmod(protected,stat.S_IREAD | stat.S_IWRITE)
check(all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==h for p,h in originals.items()),'original installation still unchanged after all checks')
(out/'preservation-result.json').write_text(json.dumps({'passed':len(checks),'checks':checks,'original_files_checked':len(originals),'game_launched':False},indent=2),encoding='utf-8');print(len(checks),'preservation checks passed')
