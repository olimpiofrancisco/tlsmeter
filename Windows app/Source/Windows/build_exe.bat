@echo off
setlocal
cd /d "%~dp0"

set NAME=TLS_Meter_V6
set SCRIPT=tls_windows_app_v6_calibracao.py

where python >nul 2>nul || (echo Python nao encontrado. Instale o Python 3.10-3.13 de 64 bits marcando "Add to PATH". & pause & exit /b 1)

if exist .venv_build rmdir /s /q .venv_build
python -m venv .venv_build || goto :erro
call .venv_build\Scripts\activate.bat
python -m pip install --upgrade pip || goto :erro
pip install -r requirements.txt || goto :erro
pip install pyinstaller || goto :erro

rem --onedir: pasta com o .exe + dependencias (_internal). --windowed: app grafico sem console.
pyinstaller --noconfirm --clean --onedir --windowed --name %NAME% ^
  --collect-all customtkinter ^
  --collect-submodules serial ^
  --hidden-import matplotlib.backends.backend_tkagg ^
  --hidden-import openpyxl.chart ^
  %SCRIPT% || goto :erro

if exist ..\README.md copy /Y ..\README.md dist\%NAME%\LEIAME.md >nul

if exist %NAME%_Windows10.zip del %NAME%_Windows10.zip
powershell -NoProfile -Command "Compress-Archive -Path 'dist\%NAME%' -DestinationPath '%NAME%_Windows10.zip' -Force" || goto :erro

echo.
echo PRONTO: %NAME%_Windows10.zip
echo Extraia a pasta inteira em qualquer Windows 10 e execute %NAME%.exe
pause
exit /b 0

:erro
echo.
echo ERRO no build. Veja as mensagens acima.
pause
exit /b 1
