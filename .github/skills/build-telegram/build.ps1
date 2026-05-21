# Telegram Desktop Debug Build Helper
# Usage: .\build.ps1
# Always builds Debug configuration.
# Writes output to build_latest.txt and appends BUILD_EXIT_CODE=N as the final line.

$repoRoot = git -C $PSScriptRoot rev-parse --show-toplevel
$buildDir = Join-Path $repoRoot "out"
$outputFile = Join-Path $repoRoot "build_latest.txt"

cmake --build $buildDir --config Debug --target Telegram 2>&1 | Out-File -FilePath $outputFile -Encoding utf8
$exit = $LASTEXITCODE
"BUILD_EXIT_CODE=$exit" | Add-Content $outputFile
