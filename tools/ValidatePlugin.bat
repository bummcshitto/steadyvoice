@echo off
setlocal
pushd "%~dp0.."
tools\ValidatePlugin.exe "%CD%\SteadyVoice.dll" "%CD%\tools\editor-test.bmp" > "%CD%\tools\validation-windows.txt" 2>&1
set "sv_result=%ERRORLEVEL%"
type "%CD%\tools\validation-windows.txt"
if "%sv_result%"=="0" (
  echo Windows test host passed. Try the plugin in your audio host next.
) else (
  echo The test host reported a failure. The report is in tools\validation-windows.txt.
)
popd
pause
exit /b %sv_result%
