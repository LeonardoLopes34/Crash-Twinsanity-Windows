# Co-op online (em desenvolvimento)

Dois jogadores, cada um no próprio PC, com a própria tela, câmera, resolução e FPS, no mesmo mundo. Um PC é o **host**, o
outro o **cliente**. Funciona por LAN, Radmin VPN, Hamachi ou porta aberta no roteador (UDP 27420).

## Como ligar

Crie `coop.ini` na pasta do jogo (ou use as variáveis `TWIN_COOP*`, que têm prioridade):

```ini
# Host:
role=host
name=Jogador 1
port=27420

# Cliente (no PC do amigo):
# role=join
# join=26.123.45.67      (o IP do host no Radmin VPN)
# name=Jogador 2
# skin=1                 (1 = Carbon Crash; ainda só a escolha, o visual vem depois)
# leash=0                (distância máxima até cair na mochila; 0 = sem limite)
```

Sem `coop.ini` o jogo é o original.

## Teclas

| Tecla | O quê |
|---|---|
| F5 | Entrar/sair da mochila do parceiro |
| F6 | Na espera de uma cutscene: puxar o parceiro até você, ou ir até ele |
| F7 | Câmera livre (ligada por padrão; `camera=original` no coop.ini ou `TWIN_CAMERA=original` começa desligada) |

### Câmera livre

A pé, a câmera só gira quando o jogador gira (analógico direito, mouse). Ela não volta sozinha para trás do personagem
(`UpdateHolds`), não gira junto com a corrida (`StepWalkYaw`) e não aceita os gatilhos de câmera das fases (`OfferCamera`).
Obstáculos aproximam a câmera em vez de girá-la ou incliná-la (`PushByProbes`), e a inclinação volta à última escolhida pelo
jogador. L1/R1 continuam trazendo a câmera para trás do personagem. Com veículo (Humiliskate, Rollerbrawl, hoverboard) e na
morte, a câmera é a original. Testado com `TWIN_LOG_CAMERA=1` correndo em círculos: original, a rotação passeou entre -24° e 36°
sozinha; livre, ficou parada em -44,8°.

## O que já funciona (código pronto, testes escritos)

| Parte | Arquivo |
|---|---|
| Rede UDP: canal rápido de posição (30/s) + canal confiável e em ordem para eventos | `src/platform/native/coop/coopnet.*` |
| Sessão: conexão, reconexão, queda por tempo limite, ping, interpolação do parceiro (100 ms) | `coop/coopsession.*` |
| Mochila: entrar/sair, nunca os dois ao mesmo tempo (o host ganha), "coleira" por distância | `coop/coopsession.*`, `coop/coopgame.cpp` |
| Espera das cutscenes: o script que vai iniciar uma cutscene (comando 589) fica parado até os dois chegarem; começa junto; com o parceiro na mochila ou desconectado, não espera | `coop/coopgame.cpp`, gancho em `src/game/behaviours.cpp` |
| Aviso na parte de baixo da tela ("Aguardando Jogador 2...") com o texto do próprio jogo | `coop/coopgame.cpp` |
| Teste de conexão sem o jogo (`coopprobe`) e autoteste das regras (`coop_selftest`) | `native/coop-tests/` |

| **Ver o parceiro**: um boneco com o modelo do personagem dele (só o modelo, sem física nem scripts), na posição e rotação dele, com a pose dos 51 ossos enviada pela rede (872 bytes, 30/s) e aplicada antes de desenhar. Testado em 2026-11-07 com duas janelas: o boneco aparece na praia na mesma pose do Crash original | `coop/coopgame.cpp` (`UpdatePuppet`, `ModelDrawn`), gancho em `src/game/ogi.cpp` |
| O jogo não pausa ao perder o foco durante o co-op | `ui/options.cpp` |

Teste num PC só: `--quick` vai direto para a praia, e `TWIN_COOP_TEST_OFFSET=3` no host desloca a posição enviada para o
boneco não ficar dentro do outro Crash.

| **Mochila de verdade**: quem entra fica invisível (modelo e sombra), sem controle, imune a dano e a itens, e é levado na posição do parceiro com a própria câmera; ao sair, reaparece ao lado. Testada em 2026-11-07 com duas janelas (`TWIN_COOP_TEST_BACKPACK=15,40`: entra aos 15 s de conexão, sai aos 40) | `coop/coopgame.cpp` (`BackpackFrame`), ganchos em `ogi.cpp` e `shadows.cpp` |

| **Caixas**: o ataque do jogador numa caixa vai para o outro PC, onde a mesma caixa (achada pela área, ID e posição) leva o mesmo golpe, dado pelo boneco do parceiro, e quebra pelo próprio script do jogo. O conteúdo dela fica só no PC de quem quebrou. Se o outro jogador estiver longe (o jogo só roda os scripts de perto), o golpe espera até ele chegar a 15 unidades. Testado em 2026-11-07: a caixa do Aku Aku quebrou nos dois PCs | `coop/coopgame.cpp` (`AgentAttacked`, `ApplyWorld`), ganchos em `agents.cpp` e `commandsnode.cpp` |
| **Frutas**: o contador é compartilhado (cada fruta que um pega soma nos dois), e a fruta do cenário que um pegou some do mundo do outro. Testado: o host pegou 1, o cliente recebeu 1 e a fruta sumiu lá | `WumpaPicked`, gancho em `commandsgame.cpp` |

Testes num PC só: `TWIN_COOP_TEST_CRATE=12` (o host vai até a caixa mais próxima e gira nela), `TWIN_COOP_TEST_PICKUP=12` (o host
pega a fruta mais próxima), `TWIN_COOP_LOG_CRATES=1` (o estado da caixa 3 s depois, no log).

## Próximos passos

1. Testar o boneco andando, pulando e girando (precisa de alguém jogando numa das janelas).
2. Caixas quebradas por explosão (TNT, nitro) quebram nos dois PCs sozinhas, mas soltam frutas nos dois; itens como a máscara
   aparecem nos dois PCs. Lembrar caixas/frutas de uma área para quando o outro jogador entrar nela depois.
2. Mostrar na mochila algo nas costas de quem carrega (como a máscara do Mind over Mutant), e dar sombra ao boneco do parceiro.
3. **Eventos do mundo** pelo host: caixas, Wumpa, máscaras, checkpoints, troca de fase (`Session::SendWorld`).
4. **Carbon Crash**: recolorir só a instância do P2.
5. **Câmera livre** (opção): ignorar os gatilhos de câmera (`SetFollowCameraIgnoresTriggers`) e a recentralização.
6. Prancha co-op (Humiliskate/hoverboard) e ajustes de chefes (mochila forçada).

## Testar sem o jogo

```powershell
cmake -S native/coop-tests -B build/coop-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build/coop-tests
build/coop-tests/coop_selftest.exe
build/coop-tests/coopprobe.exe host                # no PC do host
build/coop-tests/coopprobe.exe join 26.x.x.x Nome  # no PC do amigo
```
