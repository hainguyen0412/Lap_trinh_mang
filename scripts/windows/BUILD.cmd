@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build.ps1"
set "runExit=%errorlevel%"
if not "%runExit%"=="0" echo ERROR: exit %runExit%. Read the message above.
pause
exit /b %runExit%
