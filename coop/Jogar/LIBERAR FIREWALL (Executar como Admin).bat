@echo off
chcp 65001 >nul
echo ============================================================================
echo   CRASH TWINSANITY CO-OP - LIBERAR PORTA UDP 27420 NO FIREWALL DO WINDOWS
echo ============================================================================
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [AVISO] Solicitando permissao de Administrador para configurar o Firewall...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo Removendo regras antigas (se existirem)...
netsh advfirewall firewall delete rule name="Crash Twinsanity Co-op (UDP 27420)" >nul 2>&1
netsh advfirewall firewall delete rule name="Crash Twinsanity Co-op (Executavel)" >nul 2>&1
netsh advfirewall firewall delete rule name="Crash Twinsanity Co-op (Teste)" >nul 2>&1

echo Adicionando regra para a porta UDP 27420 (Redes Privadas e Publicas / Radmin VPN)...
netsh advfirewall firewall add rule name="Crash Twinsanity Co-op (UDP 27420)" dir=in action=allow protocol=UDP localport=27420 profile=any

echo Adicionando regra para o CrashTwinsanity.exe e coopprobe.exe...
netsh advfirewall firewall add rule name="Crash Twinsanity Co-op (Executavel)" dir=in action=allow program="%~dp0CrashTwinsanity.exe" profile=any
netsh advfirewall firewall add rule name="Crash Twinsanity Co-op (Teste)" dir=in action=allow program="%~dp0coopprobe.exe" profile=any

echo.
echo [OK] Porta UDP 27420 e executaveis liberados com sucesso no Firewall do Windows!
echo.
pause
