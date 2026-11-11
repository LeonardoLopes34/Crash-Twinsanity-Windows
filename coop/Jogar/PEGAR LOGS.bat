@echo off
rem Copia os logs do jogo (%APPDATA%\Crash Twinsanity) para esta pasta, para o Claude ler
cd /d "%~dp0"
copy /Y "%APPDATA%\Crash Twinsanity\log.txt" "log_ultimo.txt" >nul
copy /Y "%APPDATA%\Crash Twinsanity\log.previous.txt" "log_anterior.txt" >nul
copy /Y "%APPDATA%\Crash Twinsanity\options.txt" "options_copia.txt" >nul
echo Logs copiados. Avise o Claude.
pause
