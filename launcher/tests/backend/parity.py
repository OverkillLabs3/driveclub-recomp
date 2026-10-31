"""Offline native/WPF protocol regression checks; no game/network/user-profile changes."""
from pathlib import Path
import copy,json,subprocess,shutil,sys,uuid
exe=Path(sys.argv[1]).resolve();root=Path(sys.argv[2]).resolve()/('parity-'+uuid.uuid4().hex[:8]);root.mkdir(parents=True)
shutil.copy2(exe,root/'DriveClub.Backend.exe')
(root/'launcher.ini').write_text('skip_gpu_check=1\ncheck_updates=1\nname=Parity\n',encoding='utf-8')
checks=[];counter=0
def check(ok,name):
 assert ok,name
 checks.append(name);print('PASS',name,flush=True)
def call(action,state=None,fail=False):
 global counter
 counter+=1;out=root/f'{counter}.response.json';args=[str(root/'DriveClub.Backend.exe'),'--ui-'+action]
 if state is not None:
  req=root/f'{counter}.request.json';req.write_text(json.dumps(state),encoding='utf-8');args.append(str(req))
 args.append(str(out));p=subprocess.run(args,cwd=root,capture_output=True,creationflags=0x08000000,timeout=40)
 result=json.loads(out.read_text(encoding='utf-8'))
 assert bool(result.get('Error'))==fail,(p.returncode,result)
 assert (p.returncode!=0)==fail,(p.returncode,result)
 return result
s=call('state');before=(root/'launcher.ini').read_bytes()
check(sum(bool(c['Details']) for c in s['Controls'])>=19,'expanded native help texts reach WPF')
check(any(c['Label']=='Force feedback' and 'recommended' in c['Details'] for c in s['Controls']),'new main FFB guidance retained')
w=call('wheelsetup',s,fail=True);check('not found' in w['Error'],'missing WheelSetup reports its actual error')
check((root/'launcher.ini').read_bytes()==before,'failed Wheel Setup does not save pending settings')
for version in ['', 'main-a89720d','0.3.2-wip','v0.3','1.2.3.4.5','garbage']:
 (root/'version.txt').write_text(version,encoding='utf-8')
 # Disable network for syntactically valid release identifiers in this offline test.
 state=copy.deepcopy(s)
 for c in state['Controls']:
  if c['Tag']=='@updatecheck':c['Selected']=0
 r=call('check-update',state)
 check(r['InstalledVersion']==version and not r['UpdateVersion'],f'installed version read from file: {version!r}')
 if version.startswith('main-'):check('Development build' in r['UpdateNote'],'development build is not offered a stable downgrade')
(root/'version.txt').write_text('main-test',encoding='utf-8');call('install-update',s,fail=True)
check(not (root/'update_download').exists(),'development update refused before download/staging')
state=copy.deepcopy(s)
for c in state['Controls']:
 if c['Tag']=='@skipgpu':c['Selected']=0
state['HardwareProblem']='CPU fixture reason'
v=call('validate',state);check(v['HardwareProblem']=='CPU fixture reason','hardware reason survives unrelated IPC validation')
for c in state['Controls']:
 if c['Tag']=='@skipgpu':c['Selected']=1
check(call('validate',state)['HardwareProblem']=='','explicit hardware-check bypass clears displayed block')
bench=root/'data/user/benchmark';bench.mkdir(parents=True,exist_ok=True)
(bench/'result.txt').write_text('avg_fps=75 low1_fps=45 low01_fps=38 track=fixture car=fixture tod=18 weather=Clear seconds=60\nran render=2560x1440 output=3840x2160 fps_limit=none aa=dlss aa_scale=quality frame_gen=off mirror_rate=1 aniso=16 dynamic_res=1 car_detail=1 opponent_detail=0 scenery_detail=1 hq_reflections=1 dlc=1 vblank_hz=120 present=fifo\n',encoding='utf-8')
r=call('state');details='\n'.join(r['BenchmarkRanDetails'])
check(r['BenchmarkFps']=='75.0' and r['BenchmarkLows'],'benchmark headline/lows retained')
check('2560x1440' in details and '3840x2160' in details,'actual render/output resolutions exported')
check('no frame cap' in details and '120 Hz' in details and 'fifo' in details,'actual cap/vblank/present exported')
check('DLAA' in details and 'quality' in details and 'frame generation off' in details,'actual AA/upscaling/frame generation exported')
check('16x' in details and 'live' in details and 'Dynamic resolution: on' in details,'actual filtering/reflections/dynamic resolution exported')
# Clearing a saved assignment needs no attached wheel or SDL. Only its mapping is saved.
config=root/'data/user/input_config/default.ini';config.parent.mkdir(parents=True,exist_ok=True)
config.write_text('#DriveClub layout v2\nwheel = FixtureWheel\nwheel_button = 4, ffb_gain_up\nwheel_button = 5, ffb_gain_down\nplayer_unknown = keep\n',encoding='utf-8')
# Use the exact runtime marker accepted by WheelIniReady.
source=Path(__file__).resolve().parents[2]/'driveclub_launcher.cpp'
import re
marker=re.search(r'kLayoutMarker\[\]\s*=\s*"([^"]+)"',source.read_text(encoding='utf-8')).group(1)
config.write_text(config.read_text().replace('#DriveClub layout v2',marker),encoding='utf-8')
r=call('state');r['CaptureClear']=True
cleared=call('ffbgainup',r)
check('ffb_gain_up' not in config.read_text() and 'ffb_gain_down' in config.read_text(),'FFB clear removes only the requested mapping without a wheel')
check('player_unknown = keep' in config.read_text(),'FFB clear preserves unrelated input configuration')
check('no button now' in cleared['Message'],'FFB clear returns the native success message')
if len(sys.argv)>3:
 # The fake SDL library is isolated in this profile; no physical input is used.
 runtime=root/'runtime';runtime.mkdir(exist_ok=True);shutil.copy2(Path(sys.argv[3]),runtime/'SDL3.dll')
 for command in ['cancel','clear']:
  config.write_text(marker+'\nwheel_button = 4, ffb_gain_up\nwheel_button = 5, ffb_gain_down\n',encoding='utf-8')
  state=call('state');counter+=1;out=root/f'{counter}.response.json';req=root/f'{counter}.request.json';req.write_text(json.dumps(state),encoding='utf-8')
  subprocess_args=[str(root/'DriveClub.Backend.exe'),'--ui-ffbgainup',str(req),str(out)]
  p=subprocess.Popen(subprocess_args,cwd=root,creationflags=0x08000000,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  import time
  time.sleep(.15);Path(str(out)+'.control').write_text(command+'\n',encoding='utf-8');p.communicate(timeout=5)
  response=json.loads(out.read_text(encoding='utf-8'))
  check(p.returncode==0 and not response['Error'],f'live FFB capture accepts {command} without waiting for timeout')
  check(response['CaptureCancelled']==(command=='cancel'),f'{command}: explicit capture outcome returned')
  check(('ffb_gain_up' in config.read_text())==(command=='cancel'),f'{command}: requested mapping retained or cleared correctly')
  check('ffb_gain_down' in config.read_text(),f'{command}: other mapping untouched')
check((root/'launcher.ini').read_bytes()==before,'parity actions preserve saved launcher settings')
(root/'result.json').write_text(json.dumps({'passed':len(checks),'checks':checks,'game_launched':False},indent=2))
print(json.dumps({'passed':len(checks),'fixture':str(root)}))
