@echo off
setlocal
if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\run.ps1" -Target gateway
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\run.ps1" -Target gateway -Mode "%~1"
)
set "runExit=%errorlevel%"
if not "%runExit%"=="0" echo ERROR: exit %runExit%. Read the message above.
pause
exit /b %runExit%
