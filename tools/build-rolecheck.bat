@echo off
setlocal
REM Builds rolecheck.exe, the classifier on its own, from the same source the plugin uses. Output goes to build\rolecheck.
call "%~dp0..\find-msvc.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\rolecheck mkdir build\rolecheck
cl /nologo /std:c++latest /EHsc /O2 /Iinclude tools\rolecheck.cpp source\RoleClassifier.cpp /Fobuild\rolecheck\ /Febuild\rolecheck\rolecheck.exe
