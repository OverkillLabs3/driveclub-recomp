"""Backend Timelapse settings persistence; runs only inside a private fixture.
Usage: python timelapse.py BACKEND_EXE OUTPUT_DIRECTORY
"""
import json,subprocess,shutil,sys,uuid
from pathlib import Path
root=Path(sys.argv[2]).resolve();root.mkdir(parents=True,exist_ok=True)
def test(exe,enabled):
 out=root/('test-'+uuid.uuid4().hex);out.mkdir();shutil.copy2(exe,out/'DriveClub.Backend.exe')
 (out/'launcher.ini').write_text('skip_gpu_check=1\ncheck_updates=0\nname=KeepMe\nfuture_option=preserve\n')
 def call(action,state=None):
  result=out/'response.json';args=[str(out/'DriveClub.Backend.exe'),'--ui-'+action]
  if state is not None:
   req=out/'request.json';req.write_text(json.dumps(state));args.append(str(req))
  args.append(str(result));p=subprocess.run(args,cwd=out,capture_output=True,timeout=45,creationflags=0x08000000)
  assert p.returncode==0,(p.returncode,p.stderr);return json.loads(result.read_text(encoding='utf-8-sig'))
 def control(s,tag):return next(x for x in s['Controls'] if x['Tag']==tag)
 s=call('state'); assert control(s,'@timelapse')['Selected']==enabled
 assert control(s,'@timelapse_rate')['Selected']==500
 assert 'Shift' in (control(s,'@timelapse').get('Hint','')+control(s,'@timelapse').get('Details',''))
 control(s,'@timelapse')['Selected']=1;control(s,'@timelapse_rate')['Selected']=750
 call('apply',s);s=call('state');assert control(s,'@timelapse_rate')['Selected']==750 and control(s,'@timelapse')['Selected']==1
 text=(out/'launcher.ini').read_text();assert 'future_option=preserve' in text and 'name=KeepMe' in text
 control(s,'@timelapse')['Selected']=0;call('apply',s);s=call('state');assert control(s,'@timelapse')['Selected']==0
 assert control(s,'@timelapse_rate')['Selected']==750
 print('PASS',exe.name,'defaults, custom rate, toggle, save/reload, unknown settings')
test(Path(sys.argv[1]).resolve(),0)
