'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''
import sys, os, re

def finish(code):
    global executePath
    os.chdir(executePath)
    sys.exit(code)

if sys.platform == 'win32' and not 'COMSPEC' in os.environ:
    print('[ERROR] COMSPEC environment variable is not set.')
    finish(1)

executePath = os.getcwd()
scriptPath = os.path.dirname(os.path.realpath(__file__))

inputVersion = ''
versionOriginal = ''
versionMajor = ''
versionMinor = ''
versionPatch = ''
versionAlpha = '0'
versionBeta = False
for arg in sys.argv:
  match = re.match(r'^\s*(\d+)\.(\d+)(\.(\d+)(\.(\d+|beta))?)?\s*$', arg)
  if match:
    inputVersion = arg
    versionOriginal = inputVersion
    versionMajor = match.group(1)
    versionMinor = match.group(2)
    versionPatch = match.group(4) if match.group(4) else '0'
    versionAlphaBeta = match.group(5) if match.group(5) else ''
    if len(versionAlphaBeta) > 0:
      if match.group(6) == 'beta':
        versionBeta = True
      else:
        versionAlpha = match.group(6)

if not len(versionMajor):
  print("Wrong version parameter")
  finish(1)

def checkVersionPart(part):
  cleared = int(part) % 1000 if len(part) > 0 else 0
  if str(cleared) != part:
    print("Bad version part: " + part)
    finish(1)

checkVersionPart(versionMajor)
checkVersionPart(versionMinor)
checkVersionPart(versionPatch)
checkVersionPart(versionAlpha)

versionFull = str(int(versionMajor) * 1000000 + int(versionMinor) * 1000 + int(versionPatch))
versionFullAlpha = '0'
if versionAlpha != '0':
  versionFullAlpha = str(int(versionFull) * 1000 + int(versionAlpha))

versionStr = versionMajor + '.' + versionMinor + '.' + versionPatch
versionStrSmall = versionStr if versionPatch != '0' else versionMajor + '.' + versionMinor

if versionBeta:
  print('Setting version: ' + versionStr + ' beta')
elif versionAlpha != '0':
  print('Setting version: ' + versionStr + '.' + versionAlpha + ' closed alpha')
else:
  print('Setting version: ' + versionStr + ' stable')

#def replaceInFile(path, replaces):

def replaceInFile(path, replacements):
  content = ''
  foundReplacements = {}
  updated = False
  with open(path, 'r') as f:
    for line in f:
      for replacement in replacements:
        if re.search(replacement[0], line):
          changed = re.sub(replacement[0], replacement[1], line)
          if changed != line:
            line = changed
            updated = True
          foundReplacements[replacement[0]] = True
      content = content + line
  for replacement in replacements:
    if not replacement[0] in foundReplacements:
      print('Could not find "' + replacement[0] + '" in "' + path + '".')
      finish(1)
  if updated:
    with open(path, 'w') as f:
      f.write(content)

print('Patching build/ptg_version...')
replaceInFile(scriptPath + '/ptg_version', [
  [ r'(PtgAppVersion\s+)\d+', r'\g<1>' + versionFull ],
  [ r'(PtgAppVersionStr\s+)\d[\d\.]*', r'\g<1>' + versionStr ],
])

print('Patching core/version.h...')
replaceInFile(scriptPath + '/../SourceFiles/core/version.h', [
  [ r'(PTelegramAppVersion\s+=\s+)\d+', r'\g<1>' + versionFull ],
  [ r'(PTelegramAppVersionStr\s+=\s+)[^;]+', r'\g<1>"' + versionStrSmall + '"' ],
])

if "commit" in sys.argv:
  print('Making commit...')
  os.system("git add ptg_version")
  os.system("git add ../SourceFiles/core/version.h")
  os.system("git commit -m 'v%s'" % (versionStr))
