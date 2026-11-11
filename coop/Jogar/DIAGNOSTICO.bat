@echo off
rem Abre o jogo como host e guarda o que aconteceu em diagnostico.txt (nesta pasta)
cd /d "%~dp0"
set TWIN_COOP=host
set TWIN_COOP_NAME=Jogador 1
echo Abrindo o jogo... (feche o jogo para terminar o diagnostico)
echo === inicio %date% %time% > diagnostico.txt
CrashTwinsanity.exe >> diagnostico.txt 2>&1
echo === codigo de saida: %errorlevel% >> diagnostico.txt
echo === log.txt do jogo: >> diagnostico.txt
type "%APPDATA%\Crash Twinsanity\log.txt" >> diagnostico.txt 2>&1
echo Pronto. Agora avise o Claude.
pause
