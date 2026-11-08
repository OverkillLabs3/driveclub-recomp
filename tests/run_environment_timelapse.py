"""Windows x86-64 host tests; compiles the actual module, never starts the game."""
from pathlib import Path
import argparse,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--cc',default='clang');p.add_argument('--build-dir',type=Path,required=True);args=p.parse_args()
root=Path(__file__).resolve().parent;build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
env=os.environ.copy()
for key in('TEMP','TMP','TMPDIR'):env[key]=str(build)
exe=build/'test_environment_timelapse.exe'
subprocess.run([args.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',str(root/'test_environment_timelapse.c'),'-luser32','-o',str(exe)],check=True,env=env)
for case in('input','groups','clouds','lightning','wind'):subprocess.run([str(exe),case],check=True,env=env)
