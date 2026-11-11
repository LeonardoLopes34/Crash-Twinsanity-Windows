# Crash Twinsanity Co-op: handoff para o Claude Code

Este repositório é o fork `LeonardoLopes34/Crash-Twinsanity-Windows` do port nativo do GioPixelGH (descompilação do Smartkin
+ camada SDL3/OpenGL do HAB3X). O mod de dois jogadores fica no branch **`coop-online`**. Leia também `coop/README.md`.

O usuário fala português (pt-BR), informal. Responda em português. Comentários e mensagens de commit seguem o estilo do
projeto, em inglês descritivo (veja `git log`).

## Situação atual (11/11/2026)

- **Foco: modo Parsec** (dois jogadores num PC, duas janelas, `TWIN_PLAYERS=2`). O amigo joga pelo Parsec, só com um
  controle.
- **Modo online (Radmin VPN) pausado.** O código fica, sem mudanças novas. Protocolo 5.
- A maior parte das correções de 11/11 compila, mas **ainda não foi testada em jogo**. Veja a seção "Para testar".

## Máquina do usuário

- Repositório: `S:\Leo\projeto-crashTwin-multiplayer\projeto-crash\CrashTwinsanityCoop\Crash-Twinsanity-Windows`.
- A pasta de fora (`S:\Leo\projeto-crashTwin-multiplayer`, com `.git` próprio) é o repositório antigo
  `projeto-crashTwin-multiplayer`, que foi substituído. Não use.
- ISO PAL: `...\CrashTwinsanityCoop\iso\`. O jogo lembra outra cópia em `%USERPROFILE%\Downloads\...`.
- Jogador 1 (host): monitor XG27ACS 2560x1440, 180 Hz. Jogador 2: janela em tela cheia no segundo monitor
  (C24FG7x 1920x1080), transmitida pelo Parsec a 60 FPS, com controle Xbox.
- PC: Ryzen 9 5900X, RX 9070 XT. O gargalo é a emulação no processador (VU1 e thread de desenho), não a placa.
- A conta git do Windows tem a EduardoLopess salva. Os remotos usam `https://LeonardoLopes34@github.com/...` para pedir a
  conta certa.

## Git

- Branch de trabalho: `coop-online`. O `main` espelha o original (upstream).
- Remotos: `origin` = GioPixelGH (upstream, só leitura), `fork` = LeonardoLopes34 (onde se envia).
- Enviar: `git push fork coop-online`.
- Autor local do repositório: Parafalll. Faça um commit por correção, com mensagem detalhada no estilo do `git log`.
- O original está 45 commits à frente (versão 1.2.0). Ainda não foi feito merge: `git merge fork/main` no `coop-online`
  quando o usuário quiser.
- Nunca enviar: ISO, `SLES_525.68`, exe, DLLs, zips, logs (o `.gitignore` já cobre). Os `.bat` ficam com CRLF (via
  `.gitattributes`).

## Compilar

- **`coop\COMPILAR.bat`** (incremental) e **`coop\COMPILAR DO ZERO.bat`** (primeira vez ou depois de mudar de compilador).
  A saída fica em `compilar_log.txt` na raiz. Os scripts copiam exe, DLLs e fontes para `coop\Jogar\`.
- Precisa do **LLVM-MinGW com clang 23 (winget)**. Com clang 20 o jogo compila, mas trava ao abrir (0xC0000005). O script
  descobre o caminho e define `LLVM_MINGW`.
- `PS2DEV=build\ps2dev`: o toolchain do PS2 é baixado para lá, com as DLLs do `tools\toolchain_dlls.py`. É usado só para
  ler o layout dos dados.
- `.venv` usa o Python 3.14 deste PC. A pasta `.venv-antigo-do-D` é lixo do PC antigo.
- O `local.json` ainda aponta para o `D:`, mas a compilação nativa não usa esse arquivo.
- Para verificar que um `.cpp` compila sem o Windows (sintaxe só):
  `g++ -std=gnu++2b -fsyntax-only -fpermissive -DTWIN_NATIVE -Inative/include -Iinclude -Isrc/platform/native -Ibuild/native/deps/SDL3-3.4.18/x86_64-w64-mingw32/include -mno-ms-bitfields <arquivo>`
- Autoteste da rede: `native/coop-tests` (38 verificações).

## Como se testa

Não há teste automático de gameplay. O ciclo é:

1. O usuário compila e joga.
2. O usuário roda **`coop\Jogar\PEGAR LOGS.bat`**, que gera `log_ultimo.txt` e `log_anterior.txt` em `coop\Jogar`.
3. Você lê os logs.

Se o jogo não abrir, use `coop\Jogar\DIAGNOSTICO.bat` (gera `diagnostico.txt`).

Linhas úteis do log:
- `second player:`: mochila, cutscenes, áreas, recriações do Jogador 2.
- `two players:`: ritmo das janelas.
- `camera:`: câmera.
- `performance:` (a cada 60 s, com o custo de cada quadro e a área).
- `game: ... (state N)`: estados (12 jogando, 13 cutscene, 21 fade out, 22 restart).

O usuário também manda prints e vídeos. Para olhar um vídeo, extraia quadros com o ffmpeg.

## Arquitetura do modo Parsec (`src/platform/native/coop/secondplayer.cpp`)

- **O Jogador 2** é uma segunda instância de personagem (`g_Second`, `Make`, `RetireSecond`). É recriada sempre que some.
- **O jogo carrega o mundo em volta do Jogador 1.** As posições são **por área (chunk)**: nunca use `PutAt` entre chunks
  diferentes. Para atravessar, use `FindLinkTo` + `InstanceContext::ChangeChunk` (veja `PutFirstWhereSecond`).
- **"O jogador" trocado:** `BeginObjectUpdate`/`EndObjectUpdate` trocam `g_PlayerInstance`, `g_PlayerCharacter` e
  `g_PlayerPart` para o Jogador 2 enquanto rodam os nós das coisas mais perto dele (`FollowsSecond`). Isso faz inimigos,
  minhocas e gatilhos reagirem aos dois. Também é a fonte da maioria dos bugs: comandos de roteiro rodando no Jogador 2.
- **Mochila:** `StartRide`, `EndRide`, `BackpackFrame` (Select, L1+R1 ou F5). O Jogador 1 nunca é levado para outra área
  na mochila do Jogador 2: sai onde estava sendo carregado.
- **Áreas:** `LeashFrame`. O Jogador 2 pode ficar em área vizinha que o jogo roda por inteiro
  (`steppedKinds == EveryKindStepped`). Se ficar 3 s fora do chão lá, é recriado. Numa área que o jogo não roda, é recriado
  em 2 s. A coleira (220/300) só vale na mesma área.
- **Cutscenes:** `CutsceneWaitHolds` não espera mais. Desfaz a troca, leva o Jogador 1 até o Jogador 2 se foi ele quem
  disparou (inclusive entre áreas) e põe o Jogador 2 na mochila. **Depois de toda cutscene e de toda volta ao checkpoint,
  o Jogador 2 é recriado do zero** (`g_FreshAfterRestart`, `g_OutWhenMade`) e sai ao lado do Jogador 1 em 1 s.
- **Proteções:** `KeepPlayersApart` (controle e "o jogador" no personagem certo), `CameraWatchdog` (câmera longe ou em
  outra área por 1,5 s é recolocada; aos 3 s, `ShowCamera(Follow)`), e a rede de segurança do controle (2 s sem pad).
- **Câmera de terceira pessoa:** `OrbitCamera`/`OrbitCameras` (F7 alterna com a do jogo).
- **Duas janelas:**
  - `TwoViewsFrame` escolhe a câmera do próximo quadro e define o ritmo por janela. `ViewRate`: 90 para a janela 1, 60
    para a janela 2 (`TWIN_P1_FPS`/`TWIN_P2_FPS`).
  - `TakeTwoViewsPace` é usado em `graphics/display.cpp` (`WaitForVBlank`).
  - **A imagem aparece 2 quadros depois de desenhada**: `ShowFrame` usa `views[(frame + 1) % 3]`.
  - Ritmo adaptativo: com quadros acima de 95% do orçamento, a janela 1 cai para 60 (`g_Reduced`) e volta depois de 3 s com
    folga.
  - `ui/features.cpp` `FrameRate()` devolve a soma (150) com dois jogadores.

Modo online: `coopgame.cpp`, `coopsession.cpp` e `coopnet.cpp`, ligados por `Coop::Active()`. No online, o parceiro é um
boneco (puppet) sem física. A troca de "o jogador" e o `NearerPlayer` ficam desligados no online.

## Regras de jogo combinadas com o usuário

- Barcos e portas de fase: **quem entra é o Jogador 1**, com o Jogador 2 na mochila. Assim chegaram ao Ice Lab sem bug.
- Cutscene: começa na hora, Jogador 2 na mochila, depois os dois juntos.
- O `coop\Jogar\GUIA-PARSEC.txt` é o guia dos jogadores. **Atualize sempre que mudar algum comportamento.**

## Para testar (correções de 11/11 ainda não jogadas)

| Commit | O quê |
|---|---|
| (este) | Jogador 2 antigo tirado da área ao ser recriado (`RetireSecond`): ficava dormindo lá e voltava como clone do Jogador 1 na próxima morte |
| 2190d5c | Cutscene disparada pelo Jogador 2 em outra área leva o Jogador 1 até lá; mochila que soltava na hora; câmera em outra área |
| 1db3854 | Jogador 2 recriado depois de toda cutscene (ficava flutuando sem andar) |
| a66837d | Janela 1 cai para 60 quando o quadro não cabe (galinhas perto do nitro); Jogador 1 sai da mochila onde estava; Jogador 2 preso no ar numa área vizinha |
| 49a983b | Imagem vai para a janela certa com 90/60 (testado: 90/60 funciona) |
| 76b2321 | Jogador 2 recriado depois do checkpoint (mortes em loop depois do nitro) |

## Em aberto

- Lutas de chefe sem cutscene (Tikimon já deu problema; a proteção `KeepPlayersApart` foi feita para ele).
- Merge da 1.2.0 do original.
- No GitHub, só o usuário pode fazer: branch padrão do fork para `coop-online` e arquivar o `projeto-crashTwin-multiplayer`.
- Online: Crash verde e atalhos do controle sem confirmação.
- Desempenho: o custo vem da emulação (VU1 e thread de desenho, ~5–7 ms por quadro em cenas cheias). Otimizar isso seria
  mexer no emulador gráfico.
