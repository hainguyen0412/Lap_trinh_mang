$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $root
try {
    New-Item -ItemType Directory -Force -Path build/bin | Out-Null
    & g++ '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-Werror' '-pthread' '-static' '-Iinclude' '-Isrc/client' '-Isrc/gateway' '-isystem' 'third_party/mavlink' '-isystem' 'third_party' 'src/processing/processing.cpp' 'tests/unit/processing/parser_tests.cpp' '-o' 'build/bin/parser_tests.exe' 
    if($LASTEXITCODE -ne 0){throw 'Build failed'}
    & '.\build\bin\parser_tests.exe'
    if($LASTEXITCODE -ne 0){throw 'Tests failed'}
} finally {Pop-Location}
