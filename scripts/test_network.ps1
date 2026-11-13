$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $projectRoot
try {
    New-Item -ItemType Directory -Force -Path build | Out-Null
    & g++ '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-Werror' '-pthread' '-static' '-Iinclude' '-Isrc/gateway' 'src/gateway/network.cpp' 'tests/unit/gateway/network_primitives_tests.cpp' '-lws2_32' '-o' 'build/network_primitives_tests.exe'
    if($LASTEXITCODE -ne 0){throw 'Build network tests failed'}
    & '.\build\network_primitives_tests.exe'
    if($LASTEXITCODE -ne 0){throw 'Network tests failed'}
} finally {Pop-Location}
