@echo off
rem Crash Twinsanity: DOIS JOGADORES NO MESMO MUNDO (um jogo so, duas janelas, uma camera para cada um).
rem Jogador 1: teclado (ou o primeiro controle, se houver dois). Jogador 2: o outro controle (o do amigo pelo Parsec).
rem A janela "Crash Twinsanity - Jogador 2" e a do segundo jogador: deixe ela no segundo monitor (real ou virtual) que o Parsec mostra.
cd /d "%~dp0"
set TWIN_PLAYERS=2
start "" CrashTwinsanity.exe
