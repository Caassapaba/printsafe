@echo off
setlocal
chcp 65001 >nul
title PrintSafe - Emulador de endpoints
cd /d "%~dp0"

rem --- Procura o Python 3 (launcher "py" primeiro, depois "python") ---
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [ERRO] Python 3 nao encontrado.
    echo        Instale em https://www.python.org/downloads/ e marque "Add python.exe to PATH".
    echo.
    pause
    exit /b 1
)

rem --- Exemplos de uso (edite a linha abaixo ou passe argumentos ao .bat) ---
rem   iniciar_emulador.bat --velocidade 60
rem   iniciar_emulador.bat --porta 80 --host 0.0.0.0
rem   iniciar_emulador.bat --ligado --novo
%PY% -u printsafe_emulator.py %*

if errorlevel 1 (
    echo.
    echo O emulador terminou com erro (codigo %errorlevel%).
    pause
)
endlocal
