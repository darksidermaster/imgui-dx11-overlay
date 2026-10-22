#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cwchar>
#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"ReaOverlayHostWindow";
constexpr wchar_t kWindowTitle[] = L"Host de teste - jogo simulado (DirectX 11)";

struct HostState {
  HWND hwnd = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapChain = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;

  HMODULE overlayModule = nullptr;
  using AttachFn = int(WINAPI*)(HWND, IDXGISwapChain*);
  using DetachFn = void(WINAPI*)();
  using BoolFn = int(WINAPI*)();
  using LastErrorFn = const char*(WINAPI*)();
  using SetIntFn = void(WINAPI*)(int);
  AttachFn attach = nullptr;
  DetachFn detach = nullptr;
  BoolFn wantsMouse = nullptr;
  BoolFn wantsKeyboard = nullptr;
  BoolFn isAttached = nullptr;
  LastErrorFn lastError = nullptr;
  SetIntFn setAutoAttach = nullptr;
  bool autoMode = false;

  // Estado do "jogo" de teste.
  float playerX = 0.5f;
  float playerY = 0.5f;
  int clicksSeenByGame = 0;
  int keysSeenByGame = 0;
  float lastClickX = 0.0f;
  float lastClickY = 0.0f;
  unsigned long long frames = 0;
  ULONGLONG lastTitleUpdate = 0;

  bool keyDown[256] = {};
};

HostState g;

std::wstring ExecutableDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring full(path, length);
  const size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

void ReleaseRenderTarget() {
  if (g.rtv != nullptr) {
    g.rtv->Release();
    g.rtv = nullptr;
  }
}

bool CreateRenderTarget() {
  ID3D11Texture2D* backBuffer = nullptr;
  if (FAILED(g.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
    return false;
  }
  const HRESULT hr = g.device->CreateRenderTargetView(backBuffer, nullptr, &g.rtv);
  backBuffer->Release();
  return SUCCEEDED(hr);
}

void LoadOverlay() {
  const std::wstring dllPath = ExecutableDirectory() + L"\\rea_overlay.dll";
  g.overlayModule = LoadLibraryW(dllPath.c_str());
  if (g.overlayModule == nullptr) {
    MessageBoxW(g.hwnd,
                (L"Nao foi possivel carregar rea_overlay.dll.\n\n" + dllPath).c_str(),
                L"Host de teste", MB_ICONERROR | MB_OK);
    return;
  }

  g.attach = reinterpret_cast<HostState::AttachFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayAttach"));
  g.detach = reinterpret_cast<HostState::DetachFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayDetach"));
  g.wantsMouse = reinterpret_cast<HostState::BoolFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayWantsMouseInput"));
  g.wantsKeyboard = reinterpret_cast<HostState::BoolFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayWantsKeyboardInput"));
  g.isAttached = reinterpret_cast<HostState::BoolFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayIsAttached"));
  g.lastError = reinterpret_cast<HostState::LastErrorFn>(
      GetProcAddress(g.overlayModule, "ReaOverlayLastError"));
  g.setAutoAttach = reinterpret_cast<HostState::SetIntFn>(
      GetProcAddress(g.overlayModule, "ReaOverlaySetAutoAttach"));

  if (g.attach == nullptr || g.detach == nullptr) {
    MessageBoxW(g.hwnd, L"rea_overlay.dll nao exporta ReaOverlayAttach/Detach.",
                L"Host de teste", MB_ICONERROR | MB_OK);
    return;
  }

  if (g.autoMode) {
    // Valida o auto-attach: a DLL instala os hooks sozinha e anexa no primeiro
    // Present, sem nenhuma chamada de ReaOverlayAttach.
    if (g.setAutoAttach != nullptr) {
      g.setAutoAttach(1);
    }
    return;
  }

  if (g.attach(g.hwnd, g.swapChain) != 0) {
    const char* detail = g.lastError != nullptr ? g.lastError() : "";
    std::string message = "ReaOverlayAttach falhou: ";
    message += detail;
    MessageBoxA(g.hwnd, message.c_str(), "Host de teste", MB_ICONERROR | MB_OK);
  }
}

void UnloadOverlay() {
  if (g.detach != nullptr && g.isAttached != nullptr && g.isAttached() != 0) {
    g.detach();
  }
  if (g.overlayModule != nullptr) {
    FreeLibrary(g.overlayModule);
    g.overlayModule = nullptr;
  }
  g.attach = nullptr;
  g.detach = nullptr;
  g.wantsMouse = nullptr;
  g.wantsKeyboard = nullptr;
  g.isAttached = nullptr;
  g.lastError = nullptr;
  g.setAutoAttach = nullptr;
}

bool CreateDeviceAndSwapChain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferCount = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

  const HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
      D3D11_SDK_VERSION, &desc, &g.swapChain, &g.device, &obtained, &g.context);
  if (FAILED(hr)) {
    return false;
  }
  return CreateRenderTarget();
}

void ResizeSwapChain(UINT width, UINT height) {
  if (g.swapChain == nullptr || width == 0 || height == 0) {
    return;
  }
  ReleaseRenderTarget();
  if (SUCCEEDED(g.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
    CreateRenderTarget();
  }
}

void UpdateGameInput(float deltaSeconds) {
  const float speed = 0.35f * deltaSeconds;
  if (g.keyDown['W']) g.playerY -= speed;
  if (g.keyDown['S']) g.playerY += speed;
  if (g.keyDown['A']) g.playerX -= speed;
  if (g.keyDown['D']) g.playerX += speed;
  if (g.playerX < 0.0f) g.playerX = 0.0f;
  if (g.playerY < 0.0f) g.playerY = 0.0f;
  if (g.playerX > 1.0f) g.playerX = 1.0f;
  if (g.playerY > 1.0f) g.playerY = 1.0f;
}

void UpdateWindowTitle() {
  const ULONGLONG now = GetTickCount64();
  if (now - g.lastTitleUpdate < 250) {
    return;
  }
  g.lastTitleUpdate = now;

  const bool overlayActive = g.isAttached != nullptr && g.isAttached() != 0;
  wchar_t title[256] = {};
  _snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE,
               L"%s | WASD move | cliques do jogo: %d | teclas do jogo: %d | overlay: %s",
               kWindowTitle, g.clicksSeenByGame, g.keysSeenByGame,
               overlayActive ? L"anexado (Insert)" : L"ausente");
  SetWindowTextW(g.hwnd, title);
}

void RenderFrame() {
  if (g.context == nullptr || g.swapChain == nullptr || g.rtv == nullptr) {
    return;
  }

  // Fundo animado que imita a imagem do jogo.
  const float t = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
  const float clear[4] = {
      0.06f + 0.04f * std::sin(t),
      0.08f + 0.04f * std::sin(t * 0.7f),
      0.12f,
      1.0f,
  };
  g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
  g.context->ClearRenderTargetView(g.rtv, clear);

  D3D11_VIEWPORT viewport = {};
  RECT client = {};
  GetClientRect(g.hwnd, &client);
  viewport.Width = static_cast<float>(client.right - client.left);
  viewport.Height = static_cast<float>(client.bottom - client.top);
  viewport.MaxDepth = 1.0f;
  g.context->RSSetViewports(1, &viewport);

  // Sem shaders aqui: o host desenha apenas um fundo variável. O painel e a
  // interacao visual sao responsabilidade da DLL de overlay.
  g.swapChain->Present(1, 0);
  ++g.frames;
}

void ShutdownGraphics() {
  ReleaseRenderTarget();
  if (g.context != nullptr) {
    g.context->Release();
    g.context = nullptr;
  }
  if (g.swapChain != nullptr) {
    g.swapChain->Release();
    g.swapChain = nullptr;
  }
  if (g.device != nullptr) {
    g.device->Release();
    g.device = nullptr;
  }
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SIZE:
      ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
      return 0;

    case WM_KEYDOWN: {
      const bool repeated = (lParam & 0x40000000) != 0;
      if (wParam < ARRAYSIZE(g.keyDown)) {
        g.keyDown[wParam] = true;
      }
      // O host só conta teclas quando o overlay não está capturando o teclado.
      if (!repeated) {
        const bool captured = g.wantsKeyboard != nullptr && g.wantsKeyboard() != 0;
        if (!captured) {
          ++g.keysSeenByGame;
        }
      }
      if (wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
      }
      return 0;
    }

    case WM_KEYUP:
      if (wParam < ARRAYSIZE(g.keyDown)) {
        g.keyDown[wParam] = false;
      }
      return 0;

    case WM_LBUTTONDOWN: {
      RECT client = {};
      GetClientRect(hwnd, &client);
      const int width = client.right - client.left;
      const int height = client.bottom - client.top;
      const bool captured = g.wantsMouse != nullptr && g.wantsMouse() != 0;
      if (!captured && width > 0 && height > 0) {
        g.lastClickX = static_cast<float>(GET_X_LPARAM(lParam)) / static_cast<float>(width);
        g.lastClickY = static_cast<float>(GET_Y_LPARAM(lParam)) / static_cast<float>(height);
        ++g.clicksSeenByGame;
      }
      return 0;
    }

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g.autoMode = commandLine != nullptr && std::wcsstr(commandLine, L"--auto") != nullptr;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = HostWndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClass;
  if (RegisterClassExW(&wc) == 0) {
    return 1;
  }

  RECT rect = {0, 0, 1280, 720};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  g.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  if (g.hwnd == nullptr) {
    return 1;
  }

  if (!CreateDeviceAndSwapChain(g.hwnd)) {
    MessageBoxW(g.hwnd, L"Falha ao criar o dispositivo Direct3D 11.",
                L"Host de teste", MB_ICONERROR | MB_OK);
    DestroyWindow(g.hwnd);
    return 1;
  }

  ShowWindow(g.hwnd, showCommand);
  UpdateWindow(g.hwnd);
  if (g.autoMode) {
    // Simula um injetor: LoadLibrary chamado de outra thread, depois que o
    // laco de renderizacao ja esta rodando. E o cenario real de injecao.
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD {
          Sleep(1500);
          LoadOverlay();
          return 0;
        },
        nullptr, 0, nullptr);
    if (thread != nullptr) {
      CloseHandle(thread);
    }
  } else {
    LoadOverlay();
  }

  LARGE_INTEGER frequency = {};
  LARGE_INTEGER previous = {};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&previous);

  MSG msg = {};
  bool running = true;
  while (running) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) {
        running = false;
      }
    }
    if (!running) {
      break;
    }

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    const float delta =
        static_cast<float>(now.QuadPart - previous.QuadPart) / static_cast<float>(frequency.QuadPart);
    previous = now;

    UpdateGameInput(delta);
    RenderFrame();
    UpdateWindowTitle();
  }

  // Ordem importante: parar o overlay antes de destruir swapchain/dispositivo.
  UnloadOverlay();
  ShutdownGraphics();
  if (IsWindow(g.hwnd)) {
    DestroyWindow(g.hwnd);
  }
  UnregisterClassW(kWindowClass, instance);
  return 0;
}
