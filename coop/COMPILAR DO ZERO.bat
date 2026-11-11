@echo off
rem Prepara o ambiente Python neste PC (uma vez so) e compila do zero. Saida em compilar_log.txt
cd /d "%~dp0.."
echo [1/3] Criando o ambiente Python...
echo === venv > compilar_log.txt
if not exist .venv\Scripts\python.exe (
  py -3 -m venv .venv >> compilar_log.txt 2>&1 || python -m venv .venv >> compilar_log.txt 2>&1
)
if not exist .venv\Scripts\python.exe (
  echo ERRO: nao achei o Python. Instale com: winget install Python.Python.3.13
  echo FALHOU: sem python>> compilar_log.txt
  pause
  exit /b 1
)
echo [2/3] Instalando as ferramentas (splat, ninja, cmake)...
echo === pip >> compilar_log.txt
.venv\Scripts\python.exe -m pip install --disable-pip-version-check -r requirements.txt cmake >> compilar_log.txt 2>&1
if errorlevel 1 (
  echo ERRO no pip. Veja compilar_log.txt
  echo FALHOU: pip>> compilar_log.txt
  pause
  exit /b 1
)
call .venv\Scripts\activate.bat
rem O toolchain do PS2 (so os cabecalhos e o g++ do PS2: o build le o layout dos dados do jogo com ele), baixado uma vez
set PS2DEV=%CD%\build\ps2dev
if not exist "build\ps2dev\ee\bin\mips64r5900el-ps2-elf-g++.exe" (
  echo [2b] Baixando o toolchain do PS2 ^(uma vez so, algumas centenas de MB^)...
  echo === ps2dev >> compilar_log.txt
  curl -L --fail -o build\ps2dev-windows-latest.tar.gz https://github.com/ps2dev/ps2dev/releases/download/latest/ps2dev-windows-latest.tar.gz >> compilar_log.txt 2>&1
  tar -xzf build\ps2dev-windows-latest.tar.gz -C build >> compilar_log.txt 2>&1
)
if not exist "build\ps2dev\ee\bin\mips64r5900el-ps2-elf-g++.exe" (
  echo ERRO: o toolchain do PS2 nao foi baixado. Veja compilar_log.txt
  echo FALHOU: ps2dev>> compilar_log.txt
  dir /b build\ps2dev >> compilar_log.txt 2>&1
  pause
  exit /b 1
)
echo [2c] Colocando as DLLs do toolchain do PS2...
echo === toolchain_dlls >> compilar_log.txt
python tools\toolchain_dlls.py >> compilar_log.txt 2>&1
if errorlevel 1 (
  echo ERRO nas DLLs do toolchain. Veja compilar_log.txt
  echo FALHOU: toolchain_dlls>> compilar_log.txt
  pause
  exit /b 1
)
echo [2d] Instalando o compilador LLVM-MinGW ^(clang 23^) pelo winget, se ainda nao estiver...
echo === winget llvm-mingw >> compilar_log.txt
winget install --id MartinStorsjo.LLVM-MinGW.UCRT -e --accept-package-agreements --accept-source-agreements >> compilar_log.txt 2>&1
set LLVM_MINGW=
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_*") do for /d %%E in ("%%D\llvm-mingw-*") do set "LLVM_MINGW=%%E"
if "%LLVM_MINGW%"=="" (
  echo ERRO: o LLVM-MinGW do winget nao foi encontrado. Veja compilar_log.txt
  echo FALHOU: llvm-mingw>> compilar_log.txt
  pause
  exit /b 1
)
echo compilador: %LLVM_MINGW%>> compilar_log.txt
"%LLVM_MINGW%\bin\clang++.exe" --version >> compilar_log.txt 2>&1
echo [3/3] Compilando... (alguns minutos)
echo === build >> compilar_log.txt
python native\tools\build_windows.py --clean >> compilar_log.txt 2>&1
if errorlevel 1 (
  echo ERRO na compilacao. Veja compilar_log.txt
  echo FALHOU: build>> compilar_log.txt
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
