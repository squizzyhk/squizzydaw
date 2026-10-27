@echo off
rem Squizzy Studio build wrapper (cmd). Forwards all arguments to build.ps1.
rem   build.bat                                   -> windows-debug into build\windows-debug, then ctest
rem   build.bat -Preset windows-release
rem   build.bat -BuildDir build\agent-engine      -> private build dir for a parallel agent
rem   build.bat -NoTest / -Clean / -Target svc_dummy / -TestFilter settings
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
