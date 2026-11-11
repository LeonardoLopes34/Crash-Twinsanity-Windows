@echo off
rem Compila o jogo e copia o exe para a pasta Jogar. A saida fica em compilar_log.txt
cd /d "%~dp0.."
echo Compilando... (a primeira vez demora alguns minutos)
call .venv\Scripts\activate.bat
set PS2DEV=%CD%\build\ps2dev
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_*") do for /d %%E in ("%%D\llvm-mingw-*") do set "LLVM_MINGW=%%E"
python native\tools\build_windows.py %* > compilar_log.txt 2>&1
if errorlevel 1 (
  echo ERRO na compilacao. Veja compilar_log.txt
  echo FALHOU>> compilar_log.txt
  pause
  exit /b 1
)
copy /Y build\native\cmake\CrashTwinsanity.exe coop\Jogar\ >> compilar_log.txt
copy /Y build\native\cmake\*.dll coop\Jogar\ >> compilar_log.txt
xcopy /Y /I /Q build\native\cmake\fonts coop\Jogar\fonts >> compilar_log.txt
if exist build\coop-tests\coopprobe.exe copy /Y build\coop-tests\coopprobe.exe coop\Jogar\ >> compilar_log.txt
echo OK: CrashTwinsanity.exe copiado para Jogar>> compilar_log.txt
echo Pronto! O exe novo esta na pasta Jogar.
pause
