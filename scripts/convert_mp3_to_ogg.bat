@echo off
setlocal enabledelayedexpansion

if "%~1"=="" goto usage
if "%~2"=="" goto usage

set SRC_DIR=%~1
set DST_DIR=%~2

if not exist "%SRC_DIR%" (
    echo Error: Source directory does not exist: %SRC_DIR%
    pause
    exit /b 1
)

if not exist "%DST_DIR%" mkdir "%DST_DIR%"

set /a count=0
for %%f in ("%SRC_DIR%\*.mp3") do set /a count+=1

if %count%==0 (
    echo No mp3 files found in %SRC_DIR%
    pause
    exit /b 0
)

echo Found %count% mp3 files, starting conversion...
echo.

set /a current=0
for %%f in ("%SRC_DIR%\*.mp3") do (
    set /a current+=1
    set filename=%%~nf
    set ogg_file=%DST_DIR%\!filename!.ogg

    echo [!current!/%count%] Converting: %%~nxf

    ffmpeg -i "%%f" -vn -ac 1 -ar 48000 -c:a libopus -b:a 32k "!ogg_file!" -y -loglevel error

    if !errorlevel! equ 0 (
        echo   Success: !filename!.ogg
    ) else (
        echo   Failed: %%~nxf
    )
    echo.
)

echo Conversion completed! Output directory: %DST_DIR%
pause
exit /b 0

:usage
echo Usage: %~nx0 source_dir target_dir
echo.
echo Example:
echo   %~nx0 E:\music E:\output
pause
exit /b 1
