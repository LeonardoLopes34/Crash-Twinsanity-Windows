@echo off
rem Dois jogadores: a janela do Jogador 2 (a do Parsec) travada em 60 quadros por segundo, que e o que o Parsec transmite,
rem e a janela do Jogador 1 sem trava, com todo o resto que o PC der. O contador de FPS de cada janela mostra o real dela.
rem Jogador 1: teclado (ou o primeiro controle, se houver dois). Jogador 2: o outro controle (ou as setas, sem controle).
cd /d "%~dp0"
set TWIN_PLAYERS=2
set TWIN_P1_FPS=1000
set TWIN_P2_FPS=60
start "" CrashTwinsanity.exe
