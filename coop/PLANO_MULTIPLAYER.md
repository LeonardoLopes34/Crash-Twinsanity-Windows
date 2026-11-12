# Plano: multiplayer de verdade (dois jogadores locais)

Branch: `multiplayer-de-verdade` (sai do `coop-online`). Escrito em 11/11/2026.

## O objetivo

Tratar o jogo como se ele tivesse sido feito para dois jogadores desde o começo. Os dois Crashes são jogadores completos.
O mundo é um só e existe para os dois. Nenhum dos dois é "o jogador de verdade" com o outro pendurado nele.

## Análise do que foi feito até agora

O modo Parsec funciona, mas cresceu em cima de três escolhas que geram a maior parte dos bugs:

| Escolha atual | Por que dá problema | O que fazer |
|---|---|---|
| **Troca de "o jogador"** (`BeginObjectUpdate`/`EndObjectUpdate`): enquanto roda as coisas perto do Jogador 2, o mod diz ao jogo que o jogador é o Jogador 2 | Tudo que pergunta "quem é o jogador?" durante a troca recebe o Jogador 2, inclusive roteiros, cutscenes e câmera. Um gatilho ativado por um jogador age no outro | Cada lugar do jogo que pergunta pelo jogador passa a perguntar **qual** jogador: o que encostou, o mais perto ou o dono da cutscene. A troca é apagada |
| **Mundo só em volta do Jogador 1**: o carregamento das áreas tem um foco só | O Jogador 2 numa área longe congela e precisa ser recriado | Foco duplo: as áreas em volta dos dois rodam (**feito**, passo 1) |
| **Alternar quadros entre as janelas**: cada quadro atualiza o mundo e desenha uma tela só | Cada janela fica com parte dos quadros, as duas mostram instantes diferentes e foi preciso travar o FPS, atrasar a imagem 2 quadros e criar um ritmo adaptativo | Atualizar o mundo uma vez e desenhar as duas vistas; depois, desenhar as duas em paralelo |

Muitas proteções existem só por causa dessas três escolhas: recriar o Jogador 2 depois de cutscene e checkpoint, mochila
forçada, coleira, vigia da câmera, `KeepPlayersApart`, "preso no ar na área vizinha". Elas devem ser removidas à medida
que a causa é resolvida, e não antes.

## Onde o jogo pergunta pelo jogador

Contagem no código do jogo (`src/game`):

- **85 usos** de `g_PlayerInstance`, `g_PlayerCharacter`, `g_PlayerPart` e das funções `PlayerInstance()`/`PlayerPart()`,
  em 19 arquivos;
- mais **~40 usos** do personagem jogado (`progress.characters[...]`, `play.character`), em 15 arquivos.

Os roteiros das fases chegam ao jogador só por esses pontos (os comandos em `commands*.cpp` e as condições em
`conditionchecks.cpp`), então revisar esses pontos cobre o jogo inteiro.

| Arquivo | Usos | Tipo | Resposta certa |
|---|---|---|---|
| `conditionchecks.cpp` | 36 | Condições dos roteiros: o jogador está agachado, correndo, caindo, em cima de mim, com quanta vida, perto | O jogador **daquele objeto que pergunta** (o que ele está mirando, ou o mais perto) |
| `nodecontrollers.cpp` | 14 | Controladores de animação e aparência ligados ao jogador | O jogador dono do modelo, ou o mais perto |
| `commandsroutes.cpp` | 6 | Inimigos e objetos que seguem ou miram o jogador | O jogador **mais perto** |
| `commandsmotion.cpp`, `commandscharacters.cpp`, `commandsagents.cpp`, `commandsnode.cpp` | 18 | Comandos que movem, teleportam ou animam o jogador | Em cutscene: **o dono da cutscene**; fora dela: quem ativou |
| `gamecontroller.cpp`, `progress.cpp`, `gamecontext.cpp` | ~20 | Estado do jogo: checkpoint, troca de fase, personagem jogado, foco do carregamento | O "líder" (normalmente o Jogador 1; o dono durante uma cutscene) |
| `trajectory.cpp`, `pickups.cpp`, `perception.cpp`, `behaviours.cpp`, `objectnode*.cpp`, `motion.cpp`, `characterstate.cpp`, `characterframe.cpp` | ~15 | Itens voando até o jogador, visão dos inimigos, "esse objeto é o jogador?" | Quem pegou / o mais perto / **qualquer** dos jogadores |
| `commandsfocus.cpp`, `commandscamera.cpp` | ~3 | Câmera e foco | A câmera de **cada** jogador |

Os ajudantes certos já existem em parte (`Coop::PlayerFor`, `Coop::NearerPlayer`, com a escolha estável de quem cada coisa
segue). O trabalho é aplicá-los ponto por ponto em vez de trocar o jogador inteiro.

## Regras de jogo (decididas com os jogadores)

- **A cutscene é de quem ativou.** O dono é "o jogador" para o roteiro. As duas telas mostram a mesma cena, o outro
  jogador fica escondido e no fim aparece ao lado do dono. Num chefe, os dois começam juntos na arena.
- **Gatilhos são de quem encostou.** O que é do jogador (animação, posição, câmera, item) acontece com quem ativou; o outro
  continua onde está.
- **O que muda o mundo vale para os dois** (checkpoint, troca de fase, porta que fecha), com regra definida, nunca por
  acidente.
- **Ainda para decidir:** a bola com o Cortex, os veículos e os chefes sem cutscene (o que o Jogador 2 faz em cada um).

## Ordem de trabalho

1. **Mundo vivo em volta dos dois** (feito neste branch, falta testar em jogo). `chunkloading.cpp` segue as ligações das
   áreas também a partir da área do Jogador 2. `TWIN_ONE_FOCUS=1` volta ao jeito antigo para comparar.
2. **Infraestrutura de jogadores:** uma lista de jogadores (instância, personagem, parte, controle, câmera) e as perguntas
   `JogadorDe(objeto)`, `JogadorMaisPerto(ponto)`, `DonoDaCutscene()`, `Lider()`.
3. **Condições dos roteiros** (`conditionchecks.cpp`, 36 pontos): o maior grupo e o mais mecânico, porque cada condição já
   sabe qual objeto pergunta.
4. **Inimigos, itens e visão** (rotas, trajetórias, percepção, itens).
5. **Cutscenes com dono** (começado neste branch, falta testar em jogo). Ativada pelo Jogador 2, a troca de "o jogador" é
   mantida durante a cutscene inteira (`StartLead`/`FinishLead` no `secondplayer.cpp`): o roteiro, a câmera e o foco do
   carregamento ficam no Jogador 2. O Jogador 1 vai para a mochila dele (mesma área) ou fica parado onde está (outra área).
   `TWIN_CUTSCENE_FIRST=1` volta ao jeito antigo. Também: sem câmera na janela 1 durante o jogo, a câmera do jogo volta em
   1 s (era a tela cinza).
6. **Apagar a troca** (`BeginObjectUpdate`/`EndObjectUpdate`) e as proteções que ficarem sem motivo.
7. **Duas telas sem alternar:** atualizar o mundo uma vez e desenhar as duas vistas; depois, desenhar em paralelo.

Cada passo é um commit separado e é testado em jogo antes do próximo.

## Riscos a conferir no teste do passo 1

- **Memória:** mais áreas carregadas ao mesmo tempo. Ver se o jogo reclama de memória ou trava ao carregar quando os dois
  estão longe.
- **Processador:** mais áreas rodando. Comparar as linhas `performance:` do log com e sem `TWIN_ONE_FOCUS=1`.
- **Coisas que supõem uma área só:** sons, câmera e checkpoint ainda seguem o foco do Jogador 1.
