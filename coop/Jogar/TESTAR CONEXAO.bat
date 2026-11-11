@echo off
rem Testa a conexao com o amigo SEM abrir o jogo (mostra o ping).
cd /d "%~dp0"
echo 1 = eu sou o HOST     2 = vou ENTRAR no host
set /p MODO=Escolha 1 ou 2 e aperte Enter: 
if "%MODO%"=="1" coopprobe.exe host
if "%MODO%"=="2" (
  set /p IP=Digite o IP do host no Radmin VPN: 
  call coopprobe.exe join %%IP%% Amigo
)
pause
