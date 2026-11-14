@echo off
cd /d "%~dp0"
python tls_windows_app_v6_calibracao.py
if errorlevel 1 (
  echo.
  echo Erro ao iniciar. Instale as dependencias com: python -m pip install -r requirements.txt
  pause
)
