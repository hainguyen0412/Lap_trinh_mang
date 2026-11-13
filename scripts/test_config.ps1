$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $projectRoot
try {
    New-Item -ItemType Directory -Force -Path build | Out-Null
    & g++ '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-Werror' '-static' '-Iinclude' '-isystem' 'third_party' 'src/config/config.cpp' 'tests/unit/config/config_tests.cpp' '-o' 'build/config_tests.exe'
    if($LASTEXITCODE -ne 0){throw 'Build config tests failed'}
    & '.\build\config_tests.exe'
    if($LASTEXITCODE -ne 0){throw 'Config tests failed'}
} finally {Pop-Location}
