@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
echo Compiling...
cl /O2 /GL /std:c++17 /EHsc /utf-8 /I"C:\opencv\opencv\build\include" auto_24point.cpp /Fe:auto_24point.exe /link /LTCG /LIBPATH:"C:\opencv\opencv\build\x64\vc16\lib" opencv_world481.lib gdi32.lib user32.lib d3d11.lib dxgi.lib winmm.lib
if %errorlevel%==0 (
    echo.
    echo Build SUCCESS: auto_24point.exe
    copy "C:\opencv\opencv\build\x64\vc16\bin\opencv_world481.dll" . >nul
    echo DLL copied.
) else (
    echo.
    echo Build FAILED
)
pause

