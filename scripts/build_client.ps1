$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $root
try {
New-Item -ItemType Directory -Force build/bin | Out-Null
& g++ '-std=c++17' '-O2' '-Wall' '-Wextra' '-Wpedantic' '-Werror' '-pthread' '-static' '-Iinclude' '-Isrc/client' '-isystem' 'third_party' '-isystem' 'third_party/mavlink' 'src/client/client_config.cpp' 'src/client/net_socket.cpp' 'src/client/send_queue.cpp' 'src/client/mavlink_builder.cpp' 'src/config/config.cpp' 'src/processing/processing.cpp' 'src/logging/csv_logger.cpp' 'apps/client_main.cpp' '-lws2_32' '-lpsapi' '-o' 'build/bin/c_client.exe'
if($LASTEXITCODE -ne 0){throw 'Client build failed'}
} finally {Pop-Location}
