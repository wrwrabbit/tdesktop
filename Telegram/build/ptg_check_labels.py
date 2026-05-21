import re, sys
from pathlib import Path

repo_root = Path(__file__).parent.parent.parent

with open(repo_root / 'Telegram/build/release.py') as f:
    py_src = f.read()

with open(repo_root / '.github/workflows/deploy_finish.yml') as f:
    yml_src = f.read()

py_labels = set(re.findall(r"'label':\s*'([^']+)'", py_src))

py_remotes = set()
for raw in re.findall(r"'remote':\s*(.+)", py_src):
    resolved = re.sub(r"'\s*\+\s*\w+\s*\+\s*'", 'latest', raw.strip())
    m = re.match(r"'([^']+)'", resolved)
    if m:
        py_remotes.add(m.group(1))

yml_labels = set(re.findall(r"label:\s*'([^']+)'", yml_src))
yml_names = set(re.findall(r"name:\s*'(t[^']+)'", yml_src))

errors = []
for label in sorted(yml_labels):
    if label not in py_labels:
        errors.append(f"Label in deploy_finish.yml but not in release.py: '{label}'")
for name in sorted(yml_names):
    if name not in py_remotes:
        errors.append(f"Asset name in deploy_finish.yml but not in release.py: '{name}'")

if errors:
    print("CONSISTENCY ERRORS:")
    for e in errors:
        print(f"  {e}")
    sys.exit(1)

print(f"OK: all {len(yml_labels)} labels and {len(yml_names)} filenames "
      f"from deploy_finish.yml are present in release.py")
