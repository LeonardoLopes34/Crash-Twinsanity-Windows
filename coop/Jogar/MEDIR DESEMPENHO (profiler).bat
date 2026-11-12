@echo off
rem Medicao de desempenho: dois jogadores sem trava de quadros, com o profiler do jogo ligado.
rem O profiler espera 90 segundos (tempo de passar pelos menus e comecar a jogar) e depois mede por 30 segundos.
rem Nesses 30 segundos, jogue normalmente com os dois (de preferencia num lugar onde o FPS cai).
rem O resultado vai para perfil.txt nesta pasta.
cd /d "%~dp0"
set TWIN_PLAYERS=2
set TWIN_P1_FPS=1000
set TWIN_P2_FPS=1000
set TWIN_PROFILE=%~dp0perfil.txt
set TWIN_PROFILE_DELAY=90
set TWIN_PROFILE_SECONDS=30
start "" CrashTwinsanity.exe
