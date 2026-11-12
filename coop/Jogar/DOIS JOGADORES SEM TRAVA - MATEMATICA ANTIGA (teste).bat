@echo off
rem Teste de comparacao: igual ao "DOIS JOGADORES SEM TRAVA (teste).bat", mas com a matematica do VU0 do jeito antigo
rem (a imitacao exata, operacao por operacao, de antes da otimizacao de 11/11). Os resultados do jogo sao os mesmos;
rem so muda a velocidade. Serve para comparar a fluidez dos dois jeitos.
cd /d "%~dp0"
set TWIN_PLAYERS=2
set TWIN_P1_FPS=1000
set TWIN_P2_FPS=1000
set TWIN_VU_EXACT_ROUNDING=1
start "" CrashTwinsanity.exe
