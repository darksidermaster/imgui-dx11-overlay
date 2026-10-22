# Overlay educacional com Dear ImGui + DirectX 11

Exemplo de estudo em C++20 para Windows x64. Uma DLL desenha um painel interativo
por cima de um aplicativo hospedeiro que simula um jogo, usando DirectX 11. O
host carrega a DLL com `LoadLibraryW` e valida o comportamento em ambiente
controlado.

Escopo: somente interface e tratamento de entrada. Não há alteração da lógica do
jogo, automação, leitura/escrita de memória do jogo nem qualquer contorno de
proteção. O único efeito externo é o desenho do painel e o consumo de mensagens
de entrada quando um controle do painel está ativo.

## Dependências e por que existem

O painel precisa desenhar dentro do mesmo back buffer que o jogo apresenta. A
forma mais direta e compatível de fazer isso sem tocar no código do jogo é
interceptar `IDXGISwapChain::Present` e desenhar antes de chamar o original.

- **DirectX 11** (`d3d11`, `dxgi`, `d3dcompiler`): renderização do painel. O
  backend `imgui_impl_dx11` cria os shaders e o layout de vértices, e restaura o
  estado de pipeline que altera. O exemplo salva/restaura também o render target.
- **Dear ImGui v1.92.9b**: widgets, layout e captura de entrada.
- **MinHook v1.3.4**: instala o hook em `Present` e em `ResizeBuffers`. Sem ele,
  seria necessário um proxy de swapchain. O hook é removido no detach.
- **Win32**: janela, swapchain e mensagens de teclado/mouse.

Todas as dependências são baixadas e verificadas por hash no `configure` do
CMake. O primeiro configure precisa de acesso à rede; depois o build é offline.

`ResizeBuffers` precisa ser interceptado porque o host libera o render target e
recria o back buffer ao redimensionar. Sem isso, o painel desenharia em um alvo
já liberado.

## Estrutura

```
.
  CMakeLists.txt
  README.md
  src/overlay/overlay_api.h   API exportada pela DLL (C ABI)
  src/overlay/overlay.cpp     hooks, ciclo de vida, painel e captura de entrada
  src/overlay/dllmain.cpp     DllMain mínimo + thread de auto-attach
  src/host/main.cpp           "jogo" de teste: janela DX11, WASD, LoadLibrary
```

## Compilar no Visual Studio 2022

Pré-requisitos: Visual Studio 2022 com a carga de trabalho **Desenvolvimento para
desktop com C++**, Windows SDK 10 e CMake (incluso no VS).

### Opção A: abrir a pasta no Visual Studio

1. `Arquivo > Abrir > Pasta...` e selecione a pasta do projeto.
2. O VS detecta o `CMakeLists.txt` e executa o configure (baixa ImGui e MinHook).
3. Selecione a configuração `x64-Release` (ou `x64-Debug`) e compile.
4. Defina `overlay_host.exe` como destino de inicialização.

### Opção B: gerar uma solução e compilar na linha de comando

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

A solução fica em `build/rea_imgui_overlay.sln`. Os artefatos ficam em
`build/Release/`:

- `rea_overlay.dll`
- `overlay_host.exe`

O `CMakeLists.txt` copia a DLL para o diretório do host após o build; os dois
arquivos precisam estar lado a lado porque o host usa
`LoadLibraryW(L"rea_overlay.dll")` relativo ao próprio executável.

## Executar

```powershell
.\build\Release\overlay_host.exe          # DLL carrega e usa ReaOverlayAttach
.\build\Release\overlay_host.exe --auto   # simula injecao: DLL carrega em outra thread e se auto-anexa
```

O modo `--auto` reproduz o cenário de injeção: um `LoadLibraryW` em thread
separada, 1,5 s depois do laço de renderização começar, sem nenhuma chamada a
`ReaOverlayAttach`. É o caminho usado quando a DLL é injetada num jogo.

Controles:

| Entrada | Efeito |
| --- | --- |
| `Insert` | mostra/oculta o painel |
| Mouse sobre o painel | cliques, arraste de slider e foco de texto são consumidos pela interface |
| `Tema claro` | alterna cores do painel (checkbox) |
| `Transparencia do painel` | slider que altera o alpha do fundo do painel |
| `Nome do observador` | campo de texto; a saudação é atualizada |
| `Incrementar contador` / `Zerar` | botões de demonstração |
| `WASD` | move o "jogador" do host (funciona com o painel aberto, fora de um campo de texto) |
| Clique fora do painel | o host conta o clique; clique no painel não conta |
| `Esc` | fecha o host |

A barra de título mostra quantos cliques e teclas o host recebeu e se o overlay
está anexado. Isso permite verificar na prática que a interface não vaza cliques
para o jogo.

## Como o exemplo funciona

### Auto-attach (uso por injeção)

A DLL não depende de o host chamar nada. No `DLL_PROCESS_ATTACH` ela cria uma
thread (`ReaOverlayAutoStart`); essa thread, já fora do loader lock:

1. cria uma swapchain descartável só para ler a vtable do DXGI (todos os
   swapchains do processo compartilham a mesma vtable);
2. instala hooks globais de `Present`, `Present1` e `ResizeBuffers` com MinHook;
3. no primeiro `Present` de uma janela visível do próprio processo (cliente
   maior que 64×64), chama `AttachToSwapChain` na thread que apresenta;
4. a partir daí renderiza o painel antes de repassar ao `Present` original.

Se o jogo ainda não criou o dispositivo, os hooks esperam o primeiro `Present`.
Isso cobre injeção antes ou depois da inicialização gráfica.

Como anexar manualmente e desligar o automático:

```cpp
ReaOverlaySetAutoAttach(0);   // antes de qualquer Present
ReaOverlayInstallHooks();      // opcional se quiser só os hooks
ReaOverlayAttach(hwnd, swapChain);
```

Um log é gravado em `%TEMP%\rea_overlay.log` (e em `OutputDebugString`). Ele
mostra se os hooks foram instalados, qual swapchain foi escolhida, em que thread
e se a entrada está disponível:

```
[rea_overlay 6104] DLL carregada (pid=6104); iniciando auto-attach
[rea_overlay 6104] hooks globais instalados (Present=...)
[rea_overlay 6104] anexado via auto-attach (hwnd=..., entrada=ok)
```

Se aparecer apenas "hooks prontos; aguardando o primeiro Present", o jogo não
está apresentando por DXGI (outro renderizador) ou a janela ainda não existe.
Se aparecer "entrada=indisponivel", o painel desenha, mas os controles não
recebem mouse/teclado porque a janela pertence a outra thread.

### Anexação explícita e hooks

`ReaOverlayAttach(hwnd, swapChain)` é o caminho manual. Ele garante que os hooks
globais existam e então:

1. valida janela, swapchain e thread (precisa ser a thread que chama `Present`);
2. obtém o dispositivo e o contexto imediato (`GetDevice`/`GetImmediateContext`);
3. inicializa os backends do ImGui;
4. instala o `WndProc` por `SetWindowLongPtrW`.

Os hooks de vtable ficam em `InstallGlobalHooks`: `Present` (índice 8),
`Present1` (índice 22) e `ResizeBuffers` (índice 13).

`ReaOverlayDetach()` desfaz tudo na ordem inversa: restaura o `WndProc`, remove
os hooks, desliga o ImGui, libera render target, contexto, dispositivo e a
referência da swapchain. O auto-attach também é desligado, para a DLL não se
reanexar.

O `DllMain` não inicializa nada além de `DisableThreadLibraryCalls` e da criação
da thread. Hooks, COM, janela e ImGui não podem ser inicializados sob o loader
lock.

### Desenho

`PresentHook` detecta o swapchain alvo e a thread de renderização. Antes de
chamar o `Present` original:

```cpp
ImGui_ImplDX11_NewFrame();
ImGui_ImplWin32_NewFrame();
ImGui::NewFrame();
// widgets
ImGui::Render();
ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
```

O backend do ImGui restaura a maior parte do estado de pipeline, mas não o
render target. Por isso o exemplo faz `OMGetRenderTargets` antes e
`OMSetRenderTargets` depois, devolvendo ao jogo o alvo que ele havia vinculado.

`ResizeBuffersHook` libera o `ID3D11RenderTargetView` antes de chamar o original,
porque o back buffer precisa estar sem referências para ser redimensionado. O RTV
é recriado preguiçosamente no próximo frame (`EnsureBackbufferResources`).

### Entrada

O `WndProc` da janela é substituído por `HookedWndProc`. A ordem é importante:

1. `Insert` é tratado primeiro e não chega ao jogo (down e up).
2. As mensagens são entregues ao `ImGui_ImplWin32_WndProcHandler`.
3. Se o painel está visível e o ImGui quer capturar o mouse, mensagens de mouse
   são consumidas (`return 0`).
4. Teclado só é consumido quando um campo/controle está ativo
   (`WantTextInput || IsAnyItemActive`), para que `WASD` continue no jogo.
5. Qualquer outra mensagem segue para o `WndProc` original.

`WantCaptureMouse` é recalculado no `NewFrame()` seguinte, então o primeiro
clique logo após mover o mouse pode chegar antes da atualização. Para evitar esse
vazamento, o exemplo guarda o retângulo do painel no último frame e bloqueia
cliques dentro dele independentemente da flag.

Ao ocultar/mostrar o painel, `ClearInputKeys()` e `ClearInputMouse()` descartam
estado preso, e o contador de captura é zerado para o host.

### Inicialização e encerramento seguros

- `ReaOverlayAttach` desfaz cada passo concluído quando um passo posterior falha;
  `ReaOverlayLastError()` retorna o motivo em texto.
- `ReaOverlayDetach` é idempotente e restaura o `WndProc` antes de liberar a
  DLL.
- O host chama `ReaOverlayDetach` e `FreeLibrary` **antes** de destruir a
  swapchain e o dispositivo.
- `io.IniFilename` e `io.LogFilename` são desativados para não gravar arquivos no
  diretório do jogo.

## API da DLL

```cpp
int  WINAPI ReaOverlayAttach(HWND hwnd, IDXGISwapChain* swapChain);
void WINAPI ReaOverlayDetach();
int  WINAPI ReaOverlayIsAttached();
int  WINAPI ReaOverlayIsVisible();
void WINAPI ReaOverlayToggle();
int  WINAPI ReaOverlayWantsMouseInput();
int  WINAPI ReaOverlayWantsKeyboardInput();
const char* WINAPI ReaOverlayLastError();
void WINAPI ReaOverlaySetAutoAttach(int enabled);
void WINAPI ReaOverlayInstallHooks();
```

`ReaOverlayWantsMouseInput`/`ReaOverlayWantsKeyboardInput` existem para um host
que redireciona entrada por conta própria (por exemplo, um jogo que filtra
cliques antes do `WndProc`). O host de teste usa essas consultas para contar
apenas eventos que chegaram ao "jogo".

## Limitações didáticas

- Um overlay em processo depende do jogo usar DirectX 11 e de a swapchain ser
  acessível na thread que chama `Present`. Jogos com outro renderizador exigem
  outro backend.
- Hooks por patch de código podem ser bloqueados por anti-cheat; este exemplo é
  para um host próprio, não para uso em jogos de terceiros.
- O painel não trata DPI por monitor nem múltiplas swapchains. A fonte é a padrão
  do ImGui, então o texto é ASCII/Latin.
- Se o jogo usar `DXGI_SWAP_EFFECT_FLIP_*`, o exemplo continua válido, mas exige
  cuidado extra com o render target; por isso o alvo é sempre revalidado.
- O auto-attach escolhe a primeira swapchain de uma janela visível do processo.
  Jogos com várias janelas ou swapchains precisam desligá-lo e chamar
  `ReaOverlayAttach` na swapchain desejada.

## Verificação realizada

Compilado com MSVC 19.41 (VS 2022), CMake 3.29, Windows SDK 10.0.22621, x64
Release, sem warnings. Testado manualmente no host deste repositório:

- modo explícito: painel renderizado sobre o fundo animado, `Insert`
  ocultando/reexibindo, checkbox de tema, slider, campo de texto e botão do
  contador; cliques no painel não chegaram ao host e cliques fora chegaram;
- modo `--auto`: `LoadLibraryW` em outra thread 1,5 s após o laço de renderização
  iniciar, sem nenhuma chamada a `ReaOverlayAttach`; a DLL instalou os hooks e
  anexou no primeiro `Present`, com `entrada=ok` no log, painel visível e
  `Insert` funcionando.

Nenhuma verificação foi feita contra jogos de terceiros.
