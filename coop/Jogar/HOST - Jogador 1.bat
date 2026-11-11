@echo off
rem Crash Twinsanity co-op: abre o jogo como HOST (quem hospeda). O amigo conecta no seu IP do Radmin VPN, porta UDP 27420.
rem Na primeira vez o Windows pergunta sobre o Firewall: marque redes PRIVADAS e PUBLICAS e clique em Permitir.
cd /d "%~dp0"
set TWIN_COOP=host
set TWIN_COOP_NAME=Jogador 1
start "" CrashTwinsanity.exe
