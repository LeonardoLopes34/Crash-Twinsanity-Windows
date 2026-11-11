# Crash Twinsanity Online Co-op: plano técnico

## O que os repositórios são de verdade

| | Crash-Twinsanity-Windows | MindOver-Recomp |
|---|---|---|
| Tipo | **Descompilação completa em C++** (Smartkin) + camada nativa SDL3/OpenGL | **Recompilação estática** do binário do Xbox 360 (PowerPC → C++ gerado) |
| Engine | Traveller's Tales Oxford (Twinsanity) | Radical Entertainment (Titans / MoM) |
| Código legível? | Sim: `src/game/*.cpp` com nomes (`GameController`, `FollowCamera`, `VideoController`...) | Não: o jogo é `sub_8223xxxx`; só os hooks do port em `src/` são legíveis |
| Rede | Nenhuma | Nenhuma (co-op só local) |
| Exige | ISO **PAL** própria (SLES-52568), a mesma nos dois PCs | ISO Xbox 360 USA própria |

**Conclusão:** dá para reutilizar o MoM só como **referência de design**, não como código. As engines são diferentes, e os
modelos e animações do Carbon Crash estão em outro formato (P3D da Radical), que não é o mesmo do Twinsanity.

## Descobertas no código do Twinsanity que ajudam

- `include/game/player.h`: o jogo **já tem dois personagens jogáveis** (`g_PlayerCharacter` e `g_PlayerCharacter2`),
  porque a campanha original controla Crash e Cortex juntos.
- `GameController::SwitchCharacter` (`gamecontroller.cpp:792`) troca qual instância recebe o controle e a câmera.
- Pareamentos que já existem (`gamecontroller.cpp:1733`): `PairingHumiliskate` (Crash surfando em Cortex, a "prancha"),
  `PairingRollerbrawl` e `PairingTied` (`LinkCharacters`). Existe também `hoverboard.cpp`.
- Cutscenes: `StartReadCutscene` (`cutsceneplayer.cpp:114`) já fica **retornando 0 e esperando** enquanto a música não
  está pronta. É o ponto exato para encaixar o "Aguardando Jogador 2".
- Câmera: `followcamera.cpp`, `followrig.cpp` e `camerarig.cpp` têm a câmera inteira. Já existe
  `SetFollowCameraIgnoresTriggers` (`followrig.cpp:800`): os ângulos forçados vêm de *triggers* do cenário, que dá para
  ignorar. O port já tem mouse look (`src/platform/native/ui/mouselook.cpp`).
- No MoM, a mochila é a máscara: estados `NOT_JOINED / MASK / IN_GAME / ENTERING_MASK` e `CActionForceEnterBackpack`
  (`docs/findings/26-more-local-players.md`). Vale copiar essa máquina de estados como design.

## Arquitetura de rede proposta

Modelo "**cada um simula o próprio Crash, o host manda no mundo**" (o mesmo que o sm64coopdx usou na descompilação do
Mario 64):

- **Transporte:** UDP com ENet, uma porta só. Primeiro por IP do Radmin VPN; depois, opcionalmente, NAT punch-through
  para dispensar o Radmin.
- **Cada PC** roda o jogo completo, com a própria câmera, resolução e FPS. Cada um envia 20 a 30 vezes por segundo:
  posição, rotação, animação, estado (pulando, girando, morto, mochila).
- **Jogador remoto** = uma instância "fantoche" de Crash no mundo local, interpolada, sem física própria.
- **Host autoritativo** para os eventos do mundo: caixas quebradas, Wumpa, máscaras, inimigos mortos, checkpoints,
  progresso salvo e troca de área. O cliente pede, o host confirma e repassa.
- **Áreas:** o jogo carrega pedaços do mapa (chunks) em volta do jogador. Os dois precisam estar na mesma área; se um se
  afastar demais, ele entra na mochila automaticamente.

## Mecânicas

1. **Mochila:** P2 aperta um botão e o personagem some. O PC dele passa a seguir o Crash do P1 com a **própria câmera**,
   e a posição dele vai junto. Apertar de novo faz sair ao lado do P1. É o "anti-bug" universal: cutscene, boss, fase de
   veículo ou distância grande forçam a mochila.
2. **Cutscene sincronizada:** quando o script vai tocar uma cutscene, o host segura em `StartReadCutscene` e mostra
   "Aguardando Jogador 2..." na parte de baixo da tela. Começa quando o P2 entra na região, ou quando aperta "Teleportar
   até o parceiro", ou depois de um tempo limite (o P2 vai para a mochila). Os dois tocam a cutscene no mesmo frame de
   início, e quem não é host apenas assiste.
3. **Prancha:** usar o veículo Humiliskate/hoverboard que já existe; o P2 vai "montado" como no pareamento original.
4. **Carbon Crash:** primeiro como o modelo do Crash com recoloração escura (shader ou tint só na instância do P2).
   Importar o modelo real dos jogos da Radical seria um projeto à parte (conversor P3D → formato do Twinsanity).
5. **Câmera nova (opção):** ignorar os triggers de ângulo forçado, desligar a recentralização automática, órbita livre
   com mouse/analógico e distância ajustável.

## Ordem de desenvolvimento

| Fase | Entrega | Risco |
|---|---|---|
| 0 | Compilar o port original na sua máquina (ferramentas + ISO PAL) | baixo |
| 1 | **Câmera livre** como opção (single player; ótima para aprender o código) | baixo |
| 2 | Rede + **fantasma**: ver o Crash do amigo andando no seu jogo | médio |
| 3 | **Mochila** | médio |
| 4 | Sincronizar mundo: caixas, Wumpa, inimigos, checkpoints, troca de fase | alto |
| 5 | **Cutscenes com espera** | médio |
| 6 | Visual do Carbon Crash (recolor) | baixo |
| 7 | Prancha co-op, ajustes de bosses | alto |

## Riscos conhecidos

- Há cerca de 170 referências a "o jogador" (`g_PlayerCharacter`) espalhadas pelo código. Inimigos, scripts e câmera só
  enxergam o P1 de cada PC. A estratégia acima (fantoche + host autoritativo) evita ter que mexer em todas.
- Os bosses e as fases de perseguição são roteirizados para um personagem: lá a mochila deve ser forçada.
- Os dois jogadores precisam da **mesma ISO PAL** e da mesma versão do mod.
