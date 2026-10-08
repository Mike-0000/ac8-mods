@echo off
rem Builds ..\acepilots_hook.dll. Needs Visual Studio 2022 C++ build tools. Close the game first.
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /O2 /W4 /EHsc /MT /LD acepilots_hook.cpp /Fo:obj\ /Fe:..\acepilots_hook.dll /link /NOLOGO /IMPLIB:obj\acepilots_hook.lib user32.lib gdi32.lib || exit /b 1
if "%1"=="test" (
  cl /nologo /O2 /W4 /EHsc /MT test_host.cpp /Fo:obj\ /Fe:obj\test_host.exe /link /NOLOGO user32.lib || exit /b 1
  obj\test_host.exe "%~dp0..\acepilots_hook.dll" || exit /b 1
)
