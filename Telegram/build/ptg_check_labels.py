import json
import sys
from pathlib import Path

repo_root = Path(__file__).resolve().parents[2]
build = repo_root / 'Telegram/build'
assets = json.loads((build / 'release_assets.json').read_text(encoding='utf-8'))
sources = {
    'win-x64': (build / 'build.bat').read_text(encoding='utf-8'),
    'win-x86': (build / 'build.bat').read_text(encoding='utf-8'),
    'linux-x64': (build / 'build.sh').read_text(encoding='utf-8'),
    'mac': (build / 'build_mac_release.sh').read_text(encoding='utf-8'),
}
version_tokens = {
    'win-x64': '%PtgAppVersionStr%%V2Suffix%',
    'win-x86': '%PtgAppVersionStr%%V2Suffix%',
    'linux-x64': '$PtgAppVersionStr$V2Suffix',
    'mac': '$VERSION',
}
errors = []
names = set()
for asset in assets:
    platform = asset['platform']
    name = f"{asset['prefix']}-{version_tokens[platform]}.{asset['extension']}"
    alias = f"{asset['prefix']}.latest.{asset['extension']}"
    if name not in sources[platform]:
        errors.append(f'Release asset does not match build output: {name}')
    if alias in names:
        errors.append(f'Duplicate release alias: {alias}')
    names.add(alias)
    if not asset['label'] or not asset['directory']:
        errors.append(f'Missing release label or directory: {name}')
if errors:
    print('\n'.join(errors))
    sys.exit(1)
print(f'OK: {len(assets)} versioned release assets and latest aliases match build outputs')
