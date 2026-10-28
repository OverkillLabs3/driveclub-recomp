"""Windows native backend integration checks. Never prepares or launches the game.
Usage: python verify.py BACKEND_EXE OUTPUT_DIRECTORY
"""
from pathlib import Path
import copy,json,os,shutil,subprocess,sys,uuid
backend=Path(sys.argv[1]).resolve();root=Path(sys.argv[2]).resolve()/('backend-'+uuid.uuid4().hex[:10]);root.mkdir(parents=True)
exe=root/'DriveClub.Backend.exe';shutil.copy2(backend,exe)
(root/'launcher.ini').write_text('skip_gpu_check=1\nname=LauncherTest\ncheck_updates=0\n',encoding='utf-8')
passed=[];counter=0

def check(condition,name):
 if not condition:raise AssertionError(name)
 passed.append(name);print('PASS',name,flush=True)

def call(action,state=None,error=False):
 global counter
 counter+=1;output=root/f'{counter}.response.json';args=[str(exe),'--ui-'+action]
 if state is not None:
  request=root/f'{counter}.request.json';request.write_text(json.dumps(state),encoding='utf-8');args.append(str(request))
 args.append(str(output));p=subprocess.run(args,cwd=root,timeout=45,creationflags=0x08000000,capture_output=True)
 result=json.loads(output.read_text(encoding='utf-8'))
 if error:assert p.returncode!=0 and result.get('Error'),result
 else:assert p.returncode==0 and not result.get('Error'),result
 return result

def tag(state,value):return next(c for c in state['Controls'] if c['Tag']==value)
def label(state,value):return next(c for c in state['Controls'] if c['Label']==value)
def select(state,value,selection):tag(state,value)['Selected']=selection

def validate(state,changed=-1):state['ChangedId']=changed;return call('validate',state)
def preset(state):state['Controls'][state['PresetId']]['Selected']=6;return call('preset',state)
s=call('state')
graphics=copy.deepcopy(s)
graphics['Graphics']={'checked':True,'name':'GeForce RTX 5080','dlss':True,'neural':True,'reflex':True,'fsr411':True,'dlss_runtime':True,'neural_runtime':True,'frame_multiplier_max':6}
graphics=validate(graphics)
check(tag(graphics,'@aa')['OptionReasons'][2]=='' and tag(graphics,'@fgen_method')['OptionReasons'][2]=='','available DLSS choices retain GPU capabilities across IPC')
select(graphics,'@aa',2); select(graphics,'@fgen_method',2); select(graphics,'@fgen_x',4); select(graphics,'@nr',1); select(graphics,'@reflex',2)
for name,value in [('@nr_intensity',3),('@nr_tone',1),('@nr_structure',4)]:select(graphics,name,value)
graphics=validate(graphics)
check(all(tag(graphics,t)['Selected']==v for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]),'DLSS, FG multiplier, NR and Reflex selections survive validation')
check(tag(graphics,'@sr_preset')['Enabled'] and tag(graphics,'@fg_preset')['Enabled'] and tag(graphics,'@nr_preset')['Enabled'],'SR, FG and NR preset controls become available independently')
roundtrip=root/'graphics-roundtrip';roundtrip.mkdir();roundtrip_exe=roundtrip/'DriveClub.Backend.exe';shutil.copy2(exe,roundtrip_exe)
(roundtrip/'launcher.ini').write_text('skip_gpu_check=1\ncheck_updates=0\nname=LauncherTest\n',encoding='utf-8')
runtime=roundtrip/'runtime';runtime.mkdir();
for name in ('bbport_dlss.dll','nvngx_dlss.dll','nvngx_dlssg.dll','bbport_nr_nvngx.dll','nvngx_dlssnr.dll'):(runtime/name).touch()
request=roundtrip/'apply.json';request.write_text(json.dumps(graphics),encoding='utf-8');response=roundtrip/'apply-response.json'
result=subprocess.run([str(roundtrip_exe),'--ui-apply',str(request),str(response)],cwd=roundtrip,timeout=45,creationflags=0x08000000,capture_output=True)
check(result.returncode==0 and not json.loads(response.read_text())['Error'],'graphics selections save through the WPF backend')
response=roundtrip/'state-response.json';roundtrip_env=dict(os.environ,BB_GPU_ID='0');result=subprocess.run([str(roundtrip_exe),'--ui-state',str(response)],cwd=roundtrip,env=roundtrip_env,timeout=45,creationflags=0x08000000,capture_output=True)
reloaded=json.loads(response.read_text());check(result.returncode==0 and all(tag(reloaded,t)['Selected']==v for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]),'graphics selections survive a fresh backend process')
check(all(tag(reloaded,t)['Selected']==v for t,v in [('@nr_intensity',3),('@nr_tone',1),('@nr_structure',4)]),'neural percentage slider choices persist with their existing index encoding')
check(not reloaded['Graphics']['checked'],'explicit Vulkan device index is not guessed from Windows display ordering')
skip_env=os.environ.copy();skip_env.pop('BB_GPU_ID',None);skip_env['DRIVECLUB_FAKE_GPU_PROBLEM']='1'
for mode in ('setting','file','environment'):
 (roundtrip/'skip_gpu_check.txt').unlink(missing_ok=True)
 if mode=='file':(roundtrip/'skip_gpu_check.txt').touch()
 if mode=='environment':skip_env['DRIVECLUB_SKIP_GPU_CHECK']='1'
 else:skip_env.pop('DRIVECLUB_SKIP_GPU_CHECK',None)
 ini=(roundtrip/'launcher.ini').read_text();ini=ini.replace('skip_gpu_check=1','skip_gpu_check='+('1' if mode=='setting' else '0'));(roundtrip/'launcher.ini').write_text(ini)
 result=subprocess.run([str(roundtrip_exe),'--ui-state',str(response)],cwd=roundtrip,env=skip_env,timeout=45,creationflags=0x08000000,capture_output=True)
 skipped=json.loads(response.read_text());check(result.returncode==0 and not skipped['HardwareProblem'],f'{mode} skip bypasses the simulated Vulkan compatibility error')
 if skipped['Graphics']['checked'] and 'RTX 5080' in skipped['Graphics']['name']:
  check(skipped['Graphics']['dlss'] and skipped['Graphics']['frame_multiplier_max']==6 and not tag(skipped,'@aa')['OptionReasons'][2],f'{mode} skip retains actual RTX 5080 graphics choices without Vulkan probing')
  if mode=='setting':
   requested=copy.deepcopy(skipped)
   for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]:select(requested,t,v)
   request.write_text(json.dumps(requested));result=subprocess.run([str(roundtrip_exe),'--ui-run',str(request),str(response)],cwd=roundtrip,env=skip_env,timeout=45,creationflags=0x08000000,capture_output=True)
   stopped=json.loads(response.read_text());check(result.returncode!=0 and 'folders' in stopped['Error'].lower() and all(tag(stopped,t)['Selected']==v for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]),'skip-mode launch retains supported settings before the missing-folders stop')
fail_env=dict(os.environ,BB_GPU_ID='0',DRIVECLUB_FAKE_GPU_PROBLEM='1');fail_env.pop('DRIVECLUB_SKIP_GPU_CHECK',None)
(roundtrip/'skip_gpu_check.txt').unlink(missing_ok=True)
ini=(roundtrip/'launcher.ini').read_text().replace('skip_gpu_check=1','skip_gpu_check=0');(roundtrip/'launcher.ini').write_text(ini)
result=subprocess.run([str(roundtrip_exe),'--ui-state',str(response)],cwd=roundtrip,env=fail_env,timeout=45,creationflags=0x08000000,capture_output=True)
failed=json.loads(response.read_text());check(result.returncode==0 and bool(failed['HardwareProblem']) and not failed['Graphics']['checked'] and not failed['Graphics']['dlss_runtime'],'failed compatibility check produces unknown GPU and stale runtime flags')
select(failed,'@skipgpu',1);failed['ChangedId']=tag(failed,'@skipgpu')['Id'];request.write_text(json.dumps(failed))
result=subprocess.run([str(roundtrip_exe),'--ui-validate',str(request),str(response)],cwd=roundtrip,env=fail_env,timeout=45,creationflags=0x08000000,capture_output=True)
enabled=json.loads(response.read_text());check(result.returncode==0 and enabled['Graphics']['dlss_runtime'] and enabled['Graphics']['neural_runtime'] and all(tag(enabled,t)['Selected']==v for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1)]),'enabling hardware-check bypass refreshes runtime availability before normalizing saved choices')
unknown_env=dict(skip_env,BB_GPU_ID='0',DRIVECLUB_SKIP_GPU_CHECK='1')
result=subprocess.run([str(roundtrip_exe),'--ui-state',str(response)],cwd=roundtrip,env=unknown_env,timeout=45,creationflags=0x08000000,capture_output=True)
unknown=json.loads(response.read_text());check(result.returncode==0 and not unknown['Graphics']['checked'] and not tag(unknown,'@aa')['OptionReasons'][2] and not tag(unknown,'@fgen_method')['OptionReasons'][2] and 'unverified' in tag(unknown,'@aa')['Hint'],'explicit device with skip enabled offers installed runtimes without guessing GPU support')
for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]:select(unknown,t,v)
request.write_text(json.dumps(unknown));result=subprocess.run([str(roundtrip_exe),'--ui-validate',str(request),str(response)],cwd=roundtrip,env=unknown_env,timeout=45,creationflags=0x08000000,capture_output=True)
unknown=json.loads(response.read_text());check(result.returncode==0 and all(tag(unknown,t)['Selected']==v for t,v in [('@aa',2),('@fgen_method',2),('@fgen_x',4),('@nr',1),('@reflex',2)]) and not tag(unknown,'@fgen_x')['OptionReasons'][4],'unverified explicit-device choices stay selectable and are not silently reset')
for name in ('nvngx_dlss.dll','nvngx_dlssg.dll','nvngx_dlssnr.dll'):(runtime/name).unlink()
result=subprocess.run([str(roundtrip_exe),'--ui-state',str(response)],cwd=roundtrip,env=unknown_env,timeout=45,creationflags=0x08000000,capture_output=True)
missing=json.loads(response.read_text());check(result.returncode==0 and not missing['Graphics']['checked'] and all(tag(missing,t)['Selected']==0 for t in ['@aa','@fgen_method','@nr']) and tag(missing,'@reflex')['Selected']==2,'unknown GPU still normalizes missing runtimes while preserving unverified Reflex')
fsr_assets=root/'fsr4_411'/'t1080_m0';fsr_assets.mkdir(parents=True);(fsr_assets/'initializer.bin').write_bytes(bytes(131072))
for name in ['spd','prepass','pass0_post','postpass','rcas']+[part for n in range(1,13) for part in (f'pass{n}',f'pass{n}_post')]:(fsr_assets/(name+'.spv')).touch()
fsr=copy.deepcopy(graphics);select(fsr,'@aa',3);select(fsr,'@up',1);fsr=validate(fsr);check(tag(fsr,'@aa')['Selected']==3,'FSR 4 stays enabled with a complete 1080p asset fixture')
fsr['Controls'][fsr['PresetId']]['Selected']=4;fsr=call('preset',fsr)
check(tag(fsr,'@aa')['Selected']==0,'4K preset immediately normalizes FSR 4 when the 2160p asset set is missing')
graphics['Graphics'].update({'dlss':False,'dlss_runtime':False,'neural':False,'neural_runtime':False,'reflex':False,'frame_multiplier_max':0})
graphics=validate(graphics)
check(all(tag(graphics,t)['Selected']==0 for t in ['@aa','@fgen_method','@nr','@reflex']),'unsupported selections are normalized on validation')
check(bool(tag(graphics,'@aa')['OptionReasons'][2]) and bool(tag(graphics,'@fgen_method')['OptionReasons'][2]),'unsupported choices carry explanatory reasons')
check(all(tag(s,t)['Kind']=='Button' and any(tag(s,t)['Id'] in c['Controls'] for c in s['Cards']) for t in ['@ffbgainup','@ffbgaindown']),'new native FFB gain controls are exposed as wheel buttons')
before=(root/'launcher.ini').read_bytes()
call('ffbgainup',s,error=True)
check((root/'launcher.ini').read_bytes()==before,'FFB capture without wheel configuration reports a prerequisite error without saving pending settings')
select(s,'@pedals',1);s=call('apply',s)
check(tag(call('state'),'@pedals')['Selected']==1 and 'show_pedals=1' in (root/'data/user/driveclub.ini').read_text(),'pedal overlay persists through launcher and game settings')
check(int((root/'data/user/toggles.txt').read_text())==2199023792128,'fresh settings use the current main optimization defaults')
(root/'launcher.ini').write_text('skip_gpu_check=1\nname=LauncherTest\ncheck_updates=0\n',encoding='utf-8')
s=call('state')
ids=[c['Id'] for c in s['Controls']];assigned=[i for card in s['Cards'] for i in card['Controls']]
check(len(ids)==len(set(ids)) and sorted(ids)==list(range(len(ids))),'stable unique control identifiers')
check(all(0<=i<len(ids) for i in assigned) and len(assigned)==len(set(assigned)),'every card has valid controls without duplicates')
check(all(c['Id'] in assigned for c in s['Controls'] if not c['Hidden']),'every visible setting belongs to exactly one card')
check([s['Controls'][i]['Label'] for i in s['QuickControls']]==['Performance preset','Frame rate','Output resolution','Anti-aliasing'],'Quick Settings order is preset, frame rate, output, AA')
check({card['Page'] for card in s['Cards']}==set(range(6)),'Display Graphics General Driving Benchmark Patches all populated')
cam=tag(s,'@camera_patch');check(cam['Selected']==1 and not cam['Enabled'] and 'runtime' in cam['Hint'],'missing camera runtime is explicitly unavailable, configured on by default')
rt=root/'runtime';rt.mkdir();marker=b'CameraPatch: experimental high-FPS camera/replay workaround enabled';(rt/'DriveClubGame.exe').write_bytes(b'x'*65520+marker+b'x')
s=call('state');check(tag(s,'@camera_patch')['Enabled'],'camera runtime capability recognized across a read-block boundary')
# Dirty all graphics choices and optional patches before applying the PS4 profile.
s['UpdatePath']=str(root/'update-not-used');s['BasePath']=str(root/'base-not-used');tag(s,'@name')['Text']='PendingName';tag(s,'@lang')['Selected']=7
select(s,'@drive_fov',125);select(s,'@drive_gearbox',2)
for c in s['Controls']:
 if c['Tag']=='@gfx':c['Selected']=0 if c['Selected'] else 1
for name in ['No reset-to-track timer','Disable track barriers','Photo mode: free camera through cars','Photo mode: skip processing','Disable motion blur and depth of field']:label(s,name)['Selected']=1
select(s,'@camera_patch',0);select(s,'@aa',2);s=preset(s)
check(s['Controls'][s['PresetId']]['Selected']==6,'PS4 profile recognized after applying it')
check(label(s,'Frame rate')['Selected']==0 and label(s,'Output resolution')['Selected']==0 and label(s,'Render resolution')['Selected']==0,'PS4 selects 30 FPS and native 1080p render/output')
check(tag(s,'@aa')['Selected']==0 and tag(s,'@aniso')['Selected']==1 and tag(s,'@mirror')['Selected']==0 and tag(s,'@hdr')['Selected']==0,'PS4 uses game AA/filtering, live reflections and SDR')
expected={'Your car: full detail':0,'Other cars: full detail':0,'Scenery detail (less pop-in)':0,'High-quality reflections':0,'Shadow edges':0,'More particles':0,'Dynamic resolution':1,'Motion blur':1,'Lens dust':1,'Heat haze':1,'FXAA pass':1}
check(all(label(s,k)['Selected']==v for k,v in expected.items()),'PS4 restores original quality, dynamic resolution and camera effects')
check(label(s,'Motion blur exposure')['Selected']==0 and tag(s,'@drive_fov')['Selected']==100,'PS4 selects Slow exposure and original field of view')
check(all(label(s,k)['Selected']==0 for k in ['No reset-to-track timer','Disable track barriers','Photo mode: free camera through cars','Photo mode: skip processing','Disable motion blur and depth of field']),'PS4 removes optional gameplay and effect overrides')
check(tag(s,'@camera_patch')['Selected']==0,'PS4 disables the unnecessary 30-FPS camera workaround')
check(tag(s,'@name')['Text']=='PendingName' and tag(s,'@lang')['Selected']==7 and tag(s,'@drive_gearbox')['Selected']==2 and s['UpdatePath'].endswith('update-not-used'),'PS4 preserves pending player, language, controls and folders')
# These IPC mutations are transient until Save; the original INI must remain unchanged.
check('name=LauncherTest' in (root/'launcher.ini').read_text() and 'camera_patch=' not in (root/'launcher.ini').read_text(),'preset and validate do not persist unsaved settings')
s=call('apply',s);loaded=call('state');check(loaded['Controls'][loaded['PresetId']]['Selected']==6 and tag(loaded,'@name')['Text']=='PendingName','PS4 and user settings survive save/restart')
select(s,'@camera_patch',1);s=validate(s);check(s['Controls'][s['PresetId']]['Selected']!=6,'manual camera override leaves the PS4 profile')
select(s,'@camera_patch',0)
s=call('apply',s);check(tag(call('state'),'@camera_patch')['Selected']==0 and 'camera_patch=0' in (root/'launcher.ini').read_text(),'individual camera opt-out persists')
s=preset(s);s=call('apply',s);check(tag(call('state'),'@camera_patch')['Selected']==0,'reapplying PS4 disables the camera workaround')
for mode in [1,3,0,2]:
 label(s,'Frame rate')['Selected']=mode;s=validate(s,label(s,'Frame rate')['Id']);check(tag(s,'@camera_patch')['Selected']==int(mode in [1,3]),f'frame mode {mode} applies the appropriate camera default')
label(s,'Frame rate')['Selected']=3;s=validate(s,label(s,'Frame rate')['Id']);select(s,'@camera_patch',0);s=call('apply',s);check(tag(call('state'),'@camera_patch')['Selected']==0,'manual opt-out in VRR survives Save/restart')
# Scoped resets: mutate unrelated pages and verify their pending state is not discarded.
select(s,'@drive_fov',120);select(s,'@camera_patch',0);label(s,'Your car: full detail')['Selected']=1;label(s,'Frame rate')['Selected']=3
s['ResetPage']=1;s['ResetSubpage']=-1;s=call('reset-page',s)
check(label(s,'Your car: full detail')['Selected']==0 and label(s,'Frame rate')['Selected']==3 and tag(s,'@drive_fov')['Selected']==120 and tag(s,'@camera_patch')['Selected']==0,'Graphics reset excludes Display Driving and Patches')
s['ResetPage']=5;s=call('reset-page',s);check(tag(s,'@camera_patch')['Selected']==1 and label(s,'Frame rate')['Selected']==3 and tag(s,'@drive_fov')['Selected']==120,'Patches reset affects only its controls')
wheel=next(c for c in s['Controls'] if c['Tag'].startswith('@ffb:') and c['Kind']=='Slider');wheel['Selected']=wheel['Minimum'];wid=wheel['Id'];wv=wheel['Selected']
s['ResetPage']=3;s['ResetSubpage']=0;s=call('reset-page',s);check(tag(s,'@drive_fov')['Selected']==100 and s['Controls'][wid]['Selected']==wv,'Driving reset respects the selected subpage')
s['ResetPage']=2;s=call('reset-page',s);check(tag(s,'@name')['Text']=='PendingName' and tag(s,'@lang')['Selected']==7,'General reset preserves identity and language')
before=(root/'launcher.ini').read_bytes();bad=copy.deepcopy(s);bad['ResetPage']=99;call('reset-page',bad,error=True)
bad=copy.deepcopy(s);tag(bad,'@aa')['Selected']=999;call('validate',bad,error=True)
check((root/'launcher.ini').read_bytes()==before,'invalid reset scope and invalid setting rejected without saving')
# Existing resolution presets still work, without changing the new compatibility choice.
s['Controls'][s['PresetId']]['Selected']=1;s=call('preset',s)
check(label(s,'Output resolution')['Selected']==1 and label(s,'Render resolution')['Selected']==1 and tag(s,'@camera_patch')['Selected']==1,'legacy Handheld preset preserved')
# Upgrades without a camera preference derive a default from the saved FPS mode.
for mode,patch,unlocked,explicit,expected in [(0,'',0,None,0),(3,'60 FPS with deltatime',1,None,1),(2,'60 FPS with fixed tickrate',0,None,0),(0,'',0,1,1)]:
 text=f'skip_gpu_check=1\nname=Legacy\nunlocked={unlocked}\n'
 if explicit is not None:text+=f'camera_patch={explicit}\n'
 (root/'launcher.ini').write_text(text)
 (root/'data/user/patches_enabled.txt').write_text(patch+'\n' if patch else '')
 legacy=call('state')
 check(label(legacy,'Frame rate')['Selected']==mode and tag(legacy,'@camera_patch')['Selected']==expected,f'legacy frame mode {mode}, camera preference {explicit}: correct default without replacing explicit choice')
result={'passed':len(passed),'checks':passed,'fixtures':str(root),'game_launched':False};(root/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
