@echo off
setlocal
pushd "%~dp0.."
if not exist build-msvc mkdir build-msvc
pushd build-msvc
rc /nologo /fo SteadyVoice.res ..\src\SteadyVoice.rc
if errorlevel 1 goto failed
cl /nologo /O2 /MT /std:c++17 /W4 /fp:precise /LD ..\src\plugin.cpp ..\src\editor.cpp SteadyVoice.res /Fe:SteadyVoice.dll /link user32.lib gdi32.lib
if errorlevel 1 goto failed
cl /nologo /O2 /MT /std:c++17 /W4 /fp:precise ..\tests\host_tests.cpp /Fe:ValidatePlugin.exe /link user32.lib gdi32.lib
if errorlevel 1 goto failed
popd
popd
echo Built build-msvc\SteadyVoice.dll
exit /b 0
:failed
popd
popd
exit /b 1
