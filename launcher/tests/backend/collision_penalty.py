"""Collision option IPC/persistence regression. Uses only private fixture profiles.

Usage: python collision_penalty.py BACKEND_EXE OUTPUT_DIRECTORY
Never prepares or starts the game, SDL, or a physical wheel.
"""
from pathlib import Path
import copy
import json
import shutil
import subprocess
import sys
import uuid

OPTION = 'One-second collision penalties'
LEGACY = 'Disable collision slowdown penalties'
root = Path(sys.argv[2]).resolve() / ('collision-ui-' + uuid.uuid4().hex[:8])
root.mkdir(parents=True)
exe = root / 'DriveClub.Backend.exe'
shutil.copy2(Path(sys.argv[1]).resolve(), exe)
(root / 'launcher.ini').write_text(
    'skip_gpu_check=1\ncheck_updates=0\nname=KeepMe\nfuture_setting=retain\n', encoding='utf-8')
user = root / 'data/user'
user.mkdir(parents=True)
selection = user / 'patches_enabled.txt'
selection.write_text('Custom compatibility workaround\n', encoding='utf-8')
checks = []
counter = 0


def check(ok, name):
    assert ok, name
    checks.append(name)
    print('PASS', name, flush=True)


def call(action, state=None):
    global counter
    counter += 1
    response = root / f'{counter}.response.json'
    args = [str(exe), '--ui-' + action]
    if state is not None:
        request = root / f'{counter}.request.json'
        request.write_text(json.dumps(state), encoding='utf-8')
        args.append(str(request))
    args.append(str(response))
    result = subprocess.run(args, cwd=root, capture_output=True,
                            creationflags=0x08000000, timeout=45)
    data = json.loads(response.read_text(encoding='utf-8'))
    assert result.returncode == 0 and not data.get('Error'), (result.returncode, data)
    return data


def option(state):
    return next(c for c in state['Controls'] if c['Tag'] == OPTION)


state = call('state')
c = option(state)
check(c['Kind'] == 'Toggle' and c['Selected'] == 0 and c['Enabled'] and not c['Hidden'],
      'new profiles expose a usable collision toggle, off by default')
card = next(card for card in state['Cards'] if c['Id'] in card['Controls'])
check(card['Title'] == 'GAMEPLAY PATCHES' and card['Page'] == 5,
      'shared native model exposes option under Patches / Gameplay patches')
before = selection.read_bytes()
c['Selected'] = 1
state = call('validate', state)
check(option(state)['Selected'] == 1 and selection.read_bytes() == before,
      'WPF validation retains pending enable without saving it')
call('apply', state)
state = call('state')
check(option(state)['Selected'] == 1 and OPTION in selection.read_text().splitlines(),
      'enabled selection survives Save and backend restart')
next(c for c in state['Controls'] if c['Tag'] == '@drive_fov')['Selected'] = 125
call('apply', state)
check(option(call('state'))['Selected'] == 1,
      'saving an unrelated driving setting preserves collision preference')
check('Custom compatibility workaround' in selection.read_text().splitlines()
      and 'future_setting=retain' in (root / 'launcher.ini').read_text(),
      'unknown patches and launcher preferences are preserved')
state = call('state')
state['ResetPage'] = 0
state['ResetSubpage'] = -1
state = call('reset-page', state)
check(option(state)['Selected'] == 1, 'Display reset leaves collision option enabled')
state['ResetPage'] = 5
state = call('reset-page', state)
check(option(state)['Selected'] == 0 and option(call('state'))['Selected'] == 1,
      'explicit Patches reset restores off default without implicit save')
state = call('state')
state['Controls'][state['PresetId']]['Selected'] = 6
state = call('preset', state)
check(option(state)['Selected'] == 0, 'explicit PS4 preset restores original collision rules')
state = call('state')
option(state)['Selected'] = 0
call('apply', state)
check(option(call('state'))['Selected'] == 0 and OPTION not in selection.read_text().splitlines(),
      'unchecking removes only the collision selection and persists after restart')
selection.write_text(OPTION + '\nCustom compatibility workaround\n', encoding='utf-8')
before = selection.read_bytes()
check(option(call('state'))['Selected'] == 1 and selection.read_bytes() == before,
      'existing enabled profiles load without replacing their preferences')
selection.write_text(LEGACY + '\nCustom compatibility workaround\n', encoding='utf-8')
check(option(call('state'))['Selected'] == 0,
      'obsolete disable-slowdown selection does not silently enable the replacement')
(root / 'result.json').write_text(json.dumps(
    {'passed': len(checks), 'checks': checks, 'game_launched': False}, indent=2), encoding='utf-8')
print(f'{len(checks)} collision UI checks passed; fixtures: {root}')
