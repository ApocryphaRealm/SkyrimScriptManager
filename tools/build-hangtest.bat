@echo off
setlocal
REM Builds tools\hangtest.cpp: the hang recorder's core alone, with no game. Run build\hangtest\hangtest.exe <log file> afterwards.

call "%~dp0..\find-msvc.bat"
if errorlevel 1 exit /b 1

cd /d "%~dp0.."
if not exist build\hangtest mkdir build\hangtest
cl /nologo /std:c++20 /O2 /EHsc /W4 /wd4324 /Iinclude /Fobuild\hangtest\ /Febuild\hangtest\hangtest.exe tools\hangtest.cpp source\HangCore.cpp user32.lib
