@echo off
rem Crash Twinsanity co-op: abre o jogo e CONECTA no host.
cd /d "%~dp0"
set /p TWIN_COOP_JOIN=Digite o IP do host no Radmin VPN (ex: 26.12.34.56) e aperte Enter: 
set TWIN_COOP=join
set TWIN_COOP_NAME=Jogador 2
set TWIN_COOP_SKIN=1
start "" CrashTwinsanity.exe
