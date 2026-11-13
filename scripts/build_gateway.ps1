$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $root
try {
New-Item -ItemType Directory -Force build/bin | Out-Null
& g++ '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-Werror' '-pthread' '-static' '-Iinclude' '-Isrc/gateway' '-isystem' 'third_party' '-isystem' 'third_party/mavlink' 'src/gateway/network.cpp' 'src/gateway/audit.cpp' 'src/gateway/gateway.cpp' 'src/config/config.cpp' 'src/processing/processing.cpp' 'src/logging/csv_logger.cpp' 'apps/gateway_main.cpp' '-lws2_32' '-lpsapi' '-o' 'build/bin/gateway.exe'
if($LASTEXITCODE -ne 0){throw 'Gateway build failed'}
} finally {Pop-Location}
