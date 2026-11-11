# Crash Twinsanity Co-op

Mod de dois jogadores para o [Crash Twinsanity para PC](https://github.com/GioPixelGH/Crash-Twinsanity-Windows) (o port
nativo do GioPixelGH, feito sobre a descompilação do Smartkin). Este fork tem o port inteiro mais o mod, no branch
**`coop-online`**.

## Modos

| Modo | Como funciona | Situação |
|---|---|---|
| **Parsec** (foco atual) | O jogo roda num PC só, com duas janelas e uma câmera para cada jogador. O amigo joga de longe pelo Parsec, só com um controle. | Em teste |
| Online (Radmin VPN) | Cada jogador roda o jogo no próprio PC e os dois se conectam por UDP. | Pausado por enquanto |

## Onde está cada coisa

| O que | Onde |
|---|---|
| Código do co-op | `src/platform/native/coop/` (`secondplayer.cpp` é o modo Parsec; `coopgame.cpp`, `coopsession.cpp` e `coopnet.cpp` são o online) |
| Ganchos no jogo | `src/game/*.cpp`, nos trechos com `Coop::` |
| Documentação técnica | `native/COOP.md` e `coop/PLANO_COOP.md` |
| Testes da rede | `native/coop-tests/` |
| Pasta para jogar | `coop/Jogar/` (scripts e guias; o exe, as DLLs e as fontes chegam ao compilar) |
| Compilar | `coop/COMPILAR DO ZERO.bat` (a primeira vez) e `coop/COMPILAR.bat` (as outras) |

## Jogar pelo Parsec

1. Compile (abaixo). O exe vai para `coop/Jogar/`.
2. Siga o `coop/Jogar/GUIA-PARSEC.txt`: monitor virtual, Parsec no host e o amigo só com um controle.
3. Abra `coop/Jogar/DOIS JOGADORES (Parsec).bat`.

Cada um precisa da própria ISO **PAL** do jogo (SLES-52568). A ISO e o `SLES_525.68` nunca vão para o git.

## Compilar no Windows

Na primeira vez, dê dois cliques em **`coop/COMPILAR DO ZERO.bat`**. Ele:

1. cria o ambiente Python (`.venv`) com o Python do seu PC;
2. instala o splat, o ninja e o cmake pelo pip;
3. baixa o toolchain do PS2 para `build/ps2dev` e coloca as DLLs que ele precisa;
4. instala o LLVM-MinGW (clang 23) pelo winget. **Com clang mais antigo o jogo compila, mas trava ao abrir**;
5. compila e copia o exe, as DLLs e as fontes para `coop/Jogar/`.

Antes disso, uma vez só, tire o executável da sua ISO e divida-o, como no README do port:

```powershell
tar -xf "C:\caminho\Crash Twinsanity (Europe).iso" SLES_525.68
.venv\Scripts\python.exe tools\split.py
```

Depois de qualquer mudança no código, use **`coop/COMPILAR.bat`**, que só recompila o que mudou. A saída fica em
`compilar_log.txt`.

## Se algo der errado

- **O jogo não abre:** rode `coop/Jogar/DIAGNOSTICO.bat`. Ele grava o que aconteceu em `diagnostico.txt`.
- **Bug durante o jogo:** rode `coop/Jogar/PEGAR LOGS.bat` logo depois de fechar o jogo. Os logs vão para a pasta `Jogar`.

## Atualizar com o port original

O branch `main` deste fork espelha o original. Para trazer as novidades do GioPixelGH:

1. No GitHub, clique em **Sync fork** no `main`.
2. Traga o `main` atualizado para o co-op:
   ```powershell
   git fetch fork
   git checkout coop-online
   git merge fork/main
   ```
3. Resolva os conflitos (em geral nos ganchos de `src/game/`), compile e teste.
