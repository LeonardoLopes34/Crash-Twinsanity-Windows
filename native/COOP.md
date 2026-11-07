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

## O que já funciona (código pronto, testes escritos)

| Parte | Arquivo |
|---|---|
| Rede UDP: canal rápido de posição (30/s) + canal confiável e em ordem para eventos | `src/platform/native/coop/coopnet.*` |
| Sessão: conexão, reconexão, queda por tempo limite, ping, interpolação do parceiro (100 ms) | `coop/coopsession.*` |
| Mochila: entrar/sair, nunca os dois ao mesmo tempo (o host ganha), "coleira" por distância | `coop/coopsession.*`, `coop/coopgame.cpp` |
| Espera das cutscenes: o script que vai iniciar uma cutscene (comando 589) fica parado até os dois chegarem; começa junto; com o parceiro na mochila ou desconectado, não espera | `coop/coopgame.cpp`, gancho em `src/game/behaviours.cpp` |
| Aviso na parte de baixo da tela ("Aguardando Jogador 2...") com o texto do próprio jogo | `coop/coopgame.cpp` |
| Teste de conexão sem o jogo (`coopprobe`) e autoteste das regras (`coop_selftest`) | `native/coop-tests/` |

## Próximos passos (precisam do jogo rodando para validar)

1. **Ver o parceiro**: criar uma instância "fantoche" do Crash e colocá-la a cada frame no estado de `Session::Partner`
   (posição, rotação, animação). Hoje a posição já chega; falta desenhar.
2. **Esconder o personagem na mochila** (hoje ele só é levado junto, atrás do parceiro) e desligar a colisão dele.
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
