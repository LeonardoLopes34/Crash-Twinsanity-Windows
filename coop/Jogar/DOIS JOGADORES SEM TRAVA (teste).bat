@echo off
rem Teste: dois jogadores com as duas janelas sem trava de quadros (1000 por janela = o mais rapido que o PC aguentar).
rem Serve para medir o maximo que este PC entrega. O log grava "frame split" e "performance" a cada 60 segundos.
cd /d "%~dp0"
set TWIN_PLAYERS=2
set TWIN_P1_FPS=1000
set TWIN_P2_FPS=1000
start "" CrashTwinsanity.exe
