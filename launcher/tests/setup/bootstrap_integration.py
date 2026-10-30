"""Real native bootstrap/child routing. Uses fixture programs, never launches a game."""
from pathlib import Path
import os, shutil, struct, subprocess, sys, uuid

bootstrap, child, output = map(Path, sys.argv[1:])
root = output / ('bootstrap route проба ' + uuid.uuid4().hex[:8])
root.mkdir(parents=True)
for src, name in [(bootstrap, 'DriveClub.exe'), (child, 'DriveClub.Launcher.exe'), (child, 'DriveClub.Native.exe')]:
    shutil.copy2(src, root/name)
# Installed-runtime routing must never run PowerShell/the installer.
(root/'Install-Launcher.ps1').write_text("throw 'Unexpected installer invocation'\n")
record = root/'child.bin'
env = dict(os.environ, DC_BOOTSTRAP_TEST_RECORD=str(record))
args = ['--preview', '--capture', str(root/'папка с пробелом'/'кадр.png'), '', 'embedded "quote"', 'ending slash\\']

def run(extra, expected):
    if record.exists(): record.unlink()
    p = subprocess.run([str(root/'DriveClub.exe'), *extra], cwd=output, env=env, timeout=30,
                       creationflags=0x08000000)
    assert p.returncode == expected, p.returncode

def read():
    data = record.read_bytes(); values=[]
    while data:
        n = struct.unpack_from('<I',data)[0]; values.append(data[4:4+n*2].decode('utf-16-le')); data=data[4+n*2:]
    return values

run(['--check-only'],0); assert not record.exists()
run(args,17); values=read(); assert values[:-2] == args and Path(values[-2]) == root and values[-1]
# Explicit native fallback works even when the .NET prerequisite script is absent.
(root/'Install-Launcher.ps1').unlink()
run(['--native','--check-only'],0); assert not record.exists()
run(['--native',*args],17); values=read(); assert values[:-2] == args and Path(values[-2]) == root
print('PASS: bootstrap forwards arguments, Unicode paths, working directory and child exit; check-only launches nothing; native fallback bypasses .NET setup')
