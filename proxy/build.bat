@echo off
rem Builds the 32-bit dinput8.dll proxy into build\. coopbridge headers come from the coop-anything checkout.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /LD /I ..\..\coop-anything\tools\bridge\include coop.cpp /Fobuild\ /Febuild\dinput8.dll /link /EXPORT:DirectInput8Create=_DirectInput8Create@20 ws2_32.lib user32.lib shell32.lib
