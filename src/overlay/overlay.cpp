#include "overlay_api.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <windowsx.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// O backend Win32 nao declara o handler no header (para nao arrastar
// <windows.h>); a propria documentacao pede esta declaracao no .cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

// Indices dos metodos na vtable de IDXGISwapChain (herdada de IUnknown).
constexpr size_t kPresentIndex = 8;
constexpr size_t kResizeBuffersIndex = 13;
constexpr size_t kPresent1Index = 22;

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct OverlayState {
  std::atomic<bool> attached{false};
  std::atomic<bool> visible{true};
  std::atomic<bool> clearInput{false};
  std::atomic<bool> wantsMouse{false};
  std::atomic<bool> wantsKeyboard{false};
  std::atomic<DWORD> renderThreadId{0};
  std::atomic<bool> autoAttach{true};
  std::atomic<bool> hooking{false};

  HWND hwnd = nullptr;
  IDXGISwapChain* swapChain = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;

  void* presentTarget = nullptr;
  void* present1Target = nullptr;
  void* resizeTarget = nullptr;
  PresentFn presentOriginal = nullptr;
  Present1Fn present1Original = nullptr;
  ResizeBuffersFn resizeOriginal = nullptr;

  WNDPROC originalWndProc = nullptr;
  bool inputThreadMatches = false;
  bool minhookInitialized = false;
  bool hooksEnabled = false;
  bool imguiReady = false;

  bool lightTheme = false;
  int counter = 0;
  float panelAlpha = 0.90f;
  char observer[64] = "observador";

  // Retangulo do painel no ultimo frame. Usado para bloquear cliques mesmo
  // antes de o ImGui recalcular o hover no proximo NewFrame().
  ImVec2 panelMin{0.0f, 0.0f};
  ImVec2 panelMax{0.0f, 0.0f};
  bool panelRectValid = false;
};

OverlayState g;
std::mutex g_attachMutex;
HANDLE g_autoThread = nullptr;
thread_local char g_lastError[512] = {};

void Log(const char* format, ...) {
  char tempPath[MAX_PATH] = {};
  const DWORD length = GetTempPathA(MAX_PATH, tempPath);
  if (length == 0 || length >= MAX_PATH) {
    return;
  }
  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%srea_overlay.log", tempPath);

  char message[1024] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  OutputDebugStringA("[rea_overlay] ");
  OutputDebugStringA(message);
  OutputDebugStringA("\n");

  FILE* file = nullptr;
  if (fopen_s(&file, path, "a") == 0 && file != nullptr) {
    std::fprintf(file, "[rea_overlay %lu] %s\n", static_cast<unsigned long>(GetCurrentProcessId()),
                 message);
    std::fclose(file);
  }
}

void SetError(const char* message) {
  std::snprintf(g_lastError, sizeof(g_lastError), "%s", message != nullptr ? message : "");
  Log("erro: %s", message != nullptr ? message : "");
}

bool IsMouseMessage(UINT msg) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      return true;
    default:
      return false;
  }
}

bool IsKeyboardMessage(UINT msg) {
  switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_UNICHAR:
      return true;
    default:
      return false;
  }
}

bool EnsureBackbufferResources() {
  if (g.rtv != nullptr) {
    return true;
  }
  if (g.swapChain == nullptr || g.device == nullptr) {
    return false;
  }

  ID3D11Texture2D* backBuffer = nullptr;
  if (FAILED(g.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr) {
    return false;
  }
  const HRESULT hr = g.device->CreateRenderTargetView(backBuffer, nullptr, &g.rtv);
  backBuffer->Release();
  return SUCCEEDED(hr);
}

void ReleaseBackbufferResources() {
  if (g.rtv != nullptr) {
    g.rtv->Release();
    g.rtv = nullptr;
  }
}

void DrawPanel() {
  ImGui::SetNextWindowPos(ImVec2(28.0f, 28.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(g.panelAlpha);

  if (ImGui::Begin("Painel de estudo - DirectX 11", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted("DLL de exemplo: interface sobre o jogo");
    ImGui::Separator();

    if (ImGui::Checkbox("Tema claro", &g.lightTheme)) {
      if (g.lightTheme) {
        ImGui::StyleColorsLight();
      } else {
        ImGui::StyleColorsDark();
      }
    }
    ImGui::SetItemTooltip("Alterna as cores do painel.");

    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Transparencia do painel", &g.panelAlpha, 0.25f, 1.0f, "%.2f");

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Nome do observador", g.observer, IM_ARRAYSIZE(g.observer));
    ImGui::Text("Ola, %s.", g.observer);

    ImGui::Separator();
    ImGui::Text("Contador: %d", g.counter);
    if (ImGui::Button("Incrementar contador")) {
      ++g.counter;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zerar")) {
      g.counter = 0;
    }

    ImGui::Separator();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Mouse preso pelo painel: %s", io.WantCaptureMouse ? "sim" : "nao");
    ImGui::Text("Teclado preso pelo painel: %s", io.WantCaptureKeyboard ? "sim" : "nao");
    ImGui::Text("FPS: %.1f", io.Framerate);
    ImGui::TextUnformatted("Insert mostra/oculta este painel.");

    g.panelMin = ImGui::GetWindowPos();
    g.panelMax = ImVec2(g.panelMin.x + ImGui::GetWindowSize().x,
                        g.panelMin.y + ImGui::GetWindowSize().y);
    g.panelRectValid = true;
  }
  ImGui::End();
}

bool IsInsidePanel(int x, int y) {
  if (!g.panelRectValid || !g.visible.load()) {
    return false;
  }
  return static_cast<float>(x) >= g.panelMin.x && static_cast<float>(x) <= g.panelMax.x &&
         static_cast<float>(y) >= g.panelMin.y && static_cast<float>(y) <= g.panelMax.y;
}

// Converte a posicao do evento para coordenadas de cliente e informa se ela
// esta sobre o painel. WM_MOUSEWHEEL carrega coordenadas de tela.
bool MouseMessageHitsPanel(UINT msg, LPARAM lParam) {
  POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
    if (g.hwnd == nullptr || !ScreenToClient(g.hwnd, &pt)) {
      return false;
    }
  }
  return IsInsidePanel(pt.x, pt.y);
}

void RenderOverlayFrame() {
  if (!g.imguiReady) {
    return;
  }

  if (g.clearInput.exchange(false)) {
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
  }

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  const bool visible = g.visible.load();
  if (visible) {
    DrawPanel();
  }
  ImGui::Render();

  const ImGuiIO& io = ImGui::GetIO();
  g.wantsMouse.store(visible && io.WantCaptureMouse);
  g.wantsKeyboard.store(visible && io.WantCaptureKeyboard);

  if (!visible || g.context == nullptr || !EnsureBackbufferResources()) {
    return;  // Nada a desenhar; o frame ImGui ja foi encerrado com Render().
  }

  // O backend do ImGui salva/restaura o estado de pipeline, mas nao o render
  // target. Guardamos e restauramos para nao roubar o alvo do jogo.
  ID3D11RenderTargetView* previousRtv = nullptr;
  ID3D11DepthStencilView* previousDsv = nullptr;
  g.context->OMGetRenderTargets(1, &previousRtv, &previousDsv);

  ID3D11RenderTargetView* rtv = g.rtv;
  g.context->OMSetRenderTargets(1, &rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  g.context->OMSetRenderTargets(1, &previousRtv, previousDsv);

  if (previousRtv != nullptr) {
    previousRtv->Release();
  }
  if (previousDsv != nullptr) {
    previousDsv->Release();
  }
}

bool IsTargetSwapChain(IDXGISwapChain* swapChain) {
  return g.attached.load() && swapChain == g.swapChain &&
         GetCurrentThreadId() == g.renderThreadId.load();
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  // Insert alterna o painel e nao chega ao jogo (down e up).
  if (wParam == VK_INSERT &&
      (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
    if (msg == WM_KEYDOWN && (lParam & 0x40000000) == 0) {
      g.visible.store(!g.visible.load());
      g.clearInput.store(true);
    }
    return 0;
  }

  // Apenas a thread dona do contexto ImGui pode alimentar o backend. Quando a
  // janela pertence a outra thread, o painel ainda desenha, mas os controles
  // nao recebem entrada (evita corrida no contexto do ImGui).
  if (g.imguiReady && g.inputThreadMatches &&
      GetCurrentThreadId() == g.renderThreadId.load()) {
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    if (msg == WM_SETCURSOR && handled != 0) {
      return handled;
    }

    if (g.visible.load()) {
      const ImGuiIO& io = ImGui::GetIO();
      // WantCaptureMouse pode ficar um frame atras do movimento real do mouse;
      // o retangulo do painel evita que esse primeiro clique vaze para o jogo.
      const bool overPanel = MouseMessageHitsPanel(msg, lParam);
      if ((io.WantCaptureMouse || overPanel) && IsMouseMessage(msg)) {
        return 0;  // O clique foi consumido pelos controles.
      }

      // Teclado: so bloqueia o jogo quando o texto/controle esta ativo. Assim
      // WASD continua controlando o jogo enquanto o painel esta apenas visivel.
      const bool typing = io.WantTextInput || ImGui::IsAnyItemActive();
      if (typing && wParam != VK_ESCAPE && IsKeyboardMessage(msg)) {
        return 0;
      }
    }
  }

  if (g.originalWndProc != nullptr) {
    return CallWindowProcW(g.originalWndProc, hwnd, msg, wParam, lParam);
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool InitializeImGui(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // Nao gravar imgui.ini no diretorio do jogo.
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().FontSizeBase = 17.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    SetError("ImGui_ImplWin32_Init falhou");
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplDX11_Init(device, context)) {
    SetError("ImGui_ImplDX11_Init falhou");
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  g.imguiReady = true;
  return true;
}

// Libera tudo o que AttachToSwapChain adquiriu, sem tocar nos hooks.
void ReleaseAttachedResources(bool restoreWindowProc) {
  g.attached.store(false);

  if (restoreWindowProc && g.hwnd != nullptr && IsWindow(g.hwnd) &&
      g.originalWndProc != nullptr) {
    SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.originalWndProc));
  }
  g.originalWndProc = nullptr;
  g.inputThreadMatches = false;

  if (g.imguiReady) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.imguiReady = false;
  }

  ReleaseBackbufferResources();

  if (g.context != nullptr) {
    g.context->Release();
    g.context = nullptr;
  }
  if (g.device != nullptr) {
    g.device->Release();
    g.device = nullptr;
  }
  if (g.swapChain != nullptr) {
    g.swapChain->Release();
    g.swapChain = nullptr;
  }

  g.hwnd = nullptr;
  g.renderThreadId.store(0);
  g.panelRectValid = false;
  g.wantsMouse.store(false);
  g.wantsKeyboard.store(false);
}

// Inicializa o overlay sobre uma swapchain concreta. Deve rodar na thread que
// chama Present (a "thread de renderizacao").
int AttachToSwapChain(HWND hwnd, IDXGISwapChain* swapChain, const char* origin) {
  if (hwnd == nullptr || !IsWindow(hwnd) || swapChain == nullptr) {
    SetError("janela ou swapchain invalidos");
    return REA_OVERLAY_ERR_ARGUMENT;
  }
  if (g.attached.load()) {
    return REA_OVERLAY_OK;
  }

  ID3D11Device* device = nullptr;
  if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    SetError("a swapchain nao pertence a um dispositivo Direct3D 11");
    return REA_OVERLAY_ERR_D3D;
  }
  ID3D11DeviceContext* context = nullptr;
  device->GetImmediateContext(&context);
  if (context == nullptr) {
    device->Release();
    SetError("contexto imediato Direct3D 11 indisponivel");
    return REA_OVERLAY_ERR_D3D;
  }

  g.hwnd = hwnd;
  g.swapChain = swapChain;
  g.swapChain->AddRef();
  g.device = device;
  g.context = context;
  g.renderThreadId.store(GetCurrentThreadId());

  if (!InitializeImGui(hwnd, device, context)) {
    ReleaseAttachedResources(false);
    return REA_OVERLAY_ERR_IMGUI;
  }

  // SetWindowLongPtrW pode ser chamado de qualquer thread, mas as mensagens
  // chegam na thread dona da janela. Se forem threads diferentes, o painel
  // desenha e o Insert funciona, porem os widgets nao recebem entrada.
  const DWORD windowThread = GetWindowThreadProcessId(hwnd, nullptr);
  g.inputThreadMatches = (windowThread == GetCurrentThreadId());

  SetLastError(0);
  const LONG_PTR previous =
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc));
  if (previous == 0 && GetLastError() != 0) {
    SetError("SetWindowLongPtrW falhou ao instalar o tratamento de entrada");
    ReleaseAttachedResources(false);
    return REA_OVERLAY_ERR_WINDOW;
  }
  g.originalWndProc = reinterpret_cast<WNDPROC>(previous);

  g.attached.store(true);
  Log("anexado via %s (hwnd=%p, swapchain=%p, thread=%lu, janela_thread=%lu, entrada=%s)",
      origin, static_cast<void*>(hwnd), static_cast<void*>(swapChain),
      static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(windowThread),
      g.inputThreadMatches ? "ok" : "indisponivel");
  if (!g.inputThreadMatches) {
    Log("aviso: a janela pertence a outra thread; o painel desenha, mas os "
        "controles nao receberao mouse/teclado");
  }
  return REA_OVERLAY_OK;
}

void RemoveHooks() {
  void* targets[] = {g.presentTarget, g.present1Target, g.resizeTarget};
  if (g.hooksEnabled) {
    // Desabilita apenas os nossos alvos; nao mexe em hooks de terceiros.
    for (void* target : targets) {
      if (target != nullptr) {
        MH_DisableHook(target);
      }
    }
    g.hooksEnabled = false;
  }

  for (void* target : targets) {
    if (target != nullptr) {
      MH_RemoveHook(target);
    }
  }
  g.presentTarget = nullptr;
  g.present1Target = nullptr;
  g.resizeTarget = nullptr;
  g.presentOriginal = nullptr;
  g.present1Original = nullptr;
  g.resizeOriginal = nullptr;

  if (g.minhookInitialized) {
    MH_Uninitialize();
    g.minhookInitialized = false;
  }
}

void MaybeAttachOnPresent(IDXGISwapChain* swapChain);

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
HRESULT WINAPI Present1Hook(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                            const DXGI_PRESENT_PARAMETERS* parameters);
HRESULT WINAPI ResizeBuffersHook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width,
                                 UINT height, DXGI_FORMAT format, UINT flags);

// Cria uma swapchain descartavel apenas para obter os enderecos de Present e
// ResizeBuffers. Todos os swapchains do processo compartilham a mesma vtable.
bool CreateDummySwapChain(IDXGISwapChain** outSwapChain, HWND* outWindow, ATOM* outClass) {
  const wchar_t kClass[] = L"ReaOverlayDummyWindow";
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  const ATOM atom = RegisterClassExW(&wc);

  HWND window = CreateWindowExW(0, kClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                nullptr, wc.hInstance, nullptr);
  if (window == nullptr) {
    if (atom != 0) {
      UnregisterClassW(kClass, wc.hInstance);
    }
    return false;
  }

  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferCount = 1;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = window;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapChain = nullptr;
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
      D3D11_SDK_VERSION, &desc, &swapChain, &device, &obtained, &context);
  if (FAILED(hr)) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &swapChain,
                                       &device, &obtained, &context);
  }
  if (context != nullptr) {
    context->Release();
  }
  if (device != nullptr) {
    device->Release();
  }
  if (FAILED(hr) || swapChain == nullptr) {
    Log("nao foi possivel criar swapchain de referencia (hr=0x%08lX)",
        static_cast<unsigned long>(hr));
    DestroyWindow(window);
    if (atom != 0) {
      UnregisterClassW(kClass, wc.hInstance);
    }
    return false;
  }

  *outSwapChain = swapChain;
  *outWindow = window;
  *outClass = atom;
  return true;
}

// Instala hooks globais de swapchain. A anexacao real acontece no primeiro
// Present do processo (ou na chamada explicita de ReaOverlayAttach).
bool InstallGlobalHooks() {
  std::lock_guard<std::mutex> lock(g_attachMutex);
  if (g.presentTarget != nullptr) {
    return true;  // Ja instalado.
  }

  IDXGISwapChain* dummy = nullptr;
  HWND dummyWindow = nullptr;
  ATOM dummyClass = 0;
  if (!CreateDummySwapChain(&dummy, &dummyWindow, &dummyClass)) {
    return false;
  }

  void** vtable = *reinterpret_cast<void***>(dummy);
  if (vtable == nullptr) {
    dummy->Release();
    DestroyWindow(dummyWindow);
    return false;
  }
  g.presentTarget = vtable[kPresentIndex];
  // Present1 pertence a IDXGISwapChain1; o slot existe na vtable de qualquer
  // swapchain criada por DXGI 1.1+ (o caso normal em Windows 10/11).
  g.present1Target = vtable[kPresent1Index];
  g.resizeTarget = vtable[kResizeBuffersIndex];

  dummy->Release();
  DestroyWindow(dummyWindow);
  if (dummyClass != 0) {
    UnregisterClassW(L"ReaOverlayDummyWindow", GetModuleHandleW(nullptr));
  }

  const MH_STATUS initStatus = MH_Initialize();
  if (initStatus == MH_OK) {
    g.minhookInitialized = true;
  } else if (initStatus != MH_ERROR_ALREADY_INITIALIZED) {
    SetError("MH_Initialize falhou");
    return false;
  }

  const struct {
    void* target;
    void* detour;
    void** original;
    const char* name;
  } hooks[] = {
      {g.presentTarget, reinterpret_cast<void*>(&PresentHook),
       reinterpret_cast<void**>(&g.presentOriginal), "Present"},
      {g.present1Target, reinterpret_cast<void*>(&Present1Hook),
       reinterpret_cast<void**>(&g.present1Original), "Present1"},
      {g.resizeTarget, reinterpret_cast<void*>(&ResizeBuffersHook),
       reinterpret_cast<void**>(&g.resizeOriginal), "ResizeBuffers"},
  };

  bool presentHooked = false;
  for (const auto& hook : hooks) {
    if (hook.target == nullptr) {
      continue;
    }
    const MH_STATUS status = MH_CreateHook(hook.target, hook.detour, hook.original);
    if (status == MH_OK) {
      presentHooked = presentHooked || hook.target == g.presentTarget;
    } else if (status != MH_ERROR_ALREADY_CREATED) {
      Log("MH_CreateHook(%s) falhou: %d", hook.name, static_cast<int>(status));
    } else {
      presentHooked = presentHooked || hook.target == g.presentTarget;
    }
  }

  if (!presentHooked) {
    RemoveHooks();
    SetError("nao foi possivel instalar o hook de Present");
    return false;
  }

  const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
  if (enableStatus != MH_OK) {
    Log("MH_EnableHook falhou: %d", static_cast<int>(enableStatus));
    RemoveHooks();
    SetError("nao foi possivel ativar os hooks");
    return false;
  }
  g.hooksEnabled = true;
  Log("hooks globais instalados (Present=%p)", g.presentTarget);
  return true;
}

// Procura o primeiro swapchain que pertenca a uma janela visivel do processo e
// anexa. Chamado de dentro do hook de Present, na thread correta.
int TryAutoAttach(IDXGISwapChain* swapChain) {
  if (g.attached.load() || !g.autoAttach.load() || g.imguiReady) {
    return REA_OVERLAY_OK;
  }

  HWND hwnd = nullptr;
  DXGI_SWAP_CHAIN_DESC desc = {};
  if (SUCCEEDED(swapChain->GetDesc(&desc))) {
    hwnd = desc.OutputWindow;
  }
  if (hwnd == nullptr) {
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))) && swapChain1 != nullptr) {
      swapChain1->GetHwnd(&hwnd);
      swapChain1->Release();
    }
  }
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return REA_OVERLAY_ERR_WINDOW;
  }
  DWORD processId = 0;
  GetWindowThreadProcessId(hwnd, &processId);
  if (processId != GetCurrentProcessId() || !IsWindowVisible(hwnd)) {
    return REA_OVERLAY_ERR_WINDOW;
  }

  // Rejeita swapchains minusculas (probes, composicao interna).
  RECT client = {};
  GetClientRect(hwnd, &client);
  if (client.right - client.left < 64 || client.bottom - client.top < 64) {
    return REA_OVERLAY_ERR_WINDOW;
  }

  std::lock_guard<std::mutex> lock(g_attachMutex);
  if (g.attached.load()) {
    return REA_OVERLAY_OK;
  }
  return AttachToSwapChain(hwnd, swapChain, "auto-attach");
}

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  PresentFn original = g.presentOriginal;
  if (original == nullptr) {
    return E_FAIL;
  }
  MaybeAttachOnPresent(swapChain);
  if (IsTargetSwapChain(swapChain)) {
    RenderOverlayFrame();
  }
  return original(swapChain, syncInterval, flags);
}

HRESULT WINAPI Present1Hook(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                            const DXGI_PRESENT_PARAMETERS* parameters) {
  Present1Fn original = g.present1Original;
  if (original == nullptr) {
    return E_FAIL;
  }
  MaybeAttachOnPresent(swapChain);
  if (IsTargetSwapChain(swapChain)) {
    RenderOverlayFrame();
  }
  return original(swapChain, syncInterval, flags, parameters);
}

HRESULT WINAPI ResizeBuffersHook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width,
                                 UINT height, DXGI_FORMAT format, UINT flags) {
  ResizeBuffersFn original = g.resizeOriginal;
  if (original == nullptr) {
    return E_FAIL;
  }
  if (IsTargetSwapChain(swapChain)) {
    ReleaseBackbufferResources();
  }
  return original(swapChain, bufferCount, width, height, format, flags);
}

void MaybeAttachOnPresent(IDXGISwapChain* swapChain) {
  if (!g.attached.load() && g.autoAttach.load() && g.presentTarget != nullptr) {
    TryAutoAttach(swapChain);
  }
}

// Thread iniciada pelo DllMain: instala os hooks sem violar o loader lock.
// O DllMain apenas cria a thread; ela roda depois que o loader libera a DLL.
DWORD WINAPI AutoAttachThread(LPVOID) {
  Log("DLL carregada (pid=%lu); iniciando auto-attach",
      static_cast<unsigned long>(GetCurrentProcessId()));
  for (int attempt = 0; attempt < 60 && g.autoAttach.load(); ++attempt) {
    if (g.attached.load()) {
      Log("auto-attach: ja anexado");
      return 0;
    }
    if (InstallGlobalHooks()) {
      Log("auto-attach: hooks prontos; aguardando o primeiro Present");
      return 0;
    }
    Sleep(500);
  }
  Log("auto-attach desistiu; use ReaOverlayAttach explicitamente");
  return 0;
}

}  // namespace

int WINAPI ReaOverlayAttach(HWND hwnd, IDXGISwapChain* swapChain) {
  if (hwnd == nullptr || !IsWindow(hwnd) || swapChain == nullptr) {
    SetError("janela ou swapchain invalidos");
    return REA_OVERLAY_ERR_ARGUMENT;
  }
  if (g.attached.load()) {
    return REA_OVERLAY_OK;
  }
  if (!InstallGlobalHooks()) {
    return REA_OVERLAY_ERR_HOOK;
  }

  std::lock_guard<std::mutex> lock(g_attachMutex);
  return AttachToSwapChain(hwnd, swapChain, "chamada explicita");
}

void WINAPI ReaOverlayDetach() {
  std::lock_guard<std::mutex> lock(g_attachMutex);
  g.autoAttach.store(false);
  ReleaseAttachedResources(true);
  RemoveHooks();
  Log("overlay removido");
}

int WINAPI ReaOverlayIsAttached() {
  return g.attached.load() ? 1 : 0;
}

int WINAPI ReaOverlayIsVisible() {
  return g.visible.load() ? 1 : 0;
}

void WINAPI ReaOverlayToggle() {
  g.visible.store(!g.visible.load());
  g.clearInput.store(true);
}

int WINAPI ReaOverlayWantsMouseInput() {
  return (g.attached.load() && g.visible.load() && g.wantsMouse.load()) ? 1 : 0;
}

int WINAPI ReaOverlayWantsKeyboardInput() {
  return (g.attached.load() && g.visible.load() && g.wantsKeyboard.load()) ? 1 : 0;
}

const char* WINAPI ReaOverlayLastError() {
  return g_lastError;
}

void WINAPI ReaOverlaySetAutoAttach(int enabled) {
  g.autoAttach.store(enabled != 0);
}

void WINAPI ReaOverlayInstallHooks() {
  InstallGlobalHooks();
}

// Chamado pelo DllMain. Separado para manter o DllMain minimo e deixar claro
// que nada pesado roda sob o loader lock.
extern "C" void ReaOverlayAutoStart() {
  HANDLE thread = CreateThread(nullptr, 0, AutoAttachThread, nullptr, 0, nullptr);
  if (thread != nullptr) {
    CloseHandle(thread);
  } else {
    Log("falha ao criar a thread de auto-attach (erro %lu)",
        static_cast<unsigned long>(GetLastError()));
  }
}
