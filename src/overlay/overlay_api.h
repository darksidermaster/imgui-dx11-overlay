#pragma once

#include <windows.h>

#ifdef REA_OVERLAY_EXPORTS
#define REA_OVERLAY_API extern "C" __declspec(dllexport)
#else
#define REA_OVERLAY_API extern "C"
#endif

struct IDXGISwapChain;

enum ReaOverlayResult : int {
  REA_OVERLAY_OK = 0,
  REA_OVERLAY_ERR_ARGUMENT = 1,
  REA_OVERLAY_ERR_ALREADY_ATTACHED = 2,
  REA_OVERLAY_ERR_D3D = 3,
  REA_OVERLAY_ERR_IMGUI = 4,
  REA_OVERLAY_ERR_HOOK = 5,
  REA_OVERLAY_ERR_WINDOW = 6,
  REA_OVERLAY_ERR_UNKNOWN = 7,
};

// Anexa o overlay a um swapchain DirectX 11 que pertence a `hwnd`.
// Deve ser chamado fora de DllMain, na thread que chama Present. Necessario
// apenas quando o auto-attach foi desligado.
REA_OVERLAY_API int WINAPI ReaOverlayAttach(HWND hwnd, IDXGISwapChain* swapChain);

// Remove hooks, subclass da janela e libera ImGui/COM.
// Chame antes de destruir o swapchain e antes de FreeLibrary.
REA_OVERLAY_API void WINAPI ReaOverlayDetach();

REA_OVERLAY_API int WINAPI ReaOverlayIsAttached();
REA_OVERLAY_API int WINAPI ReaOverlayIsVisible();
REA_OVERLAY_API void WINAPI ReaOverlayToggle();

// Consultas uteis para um host de teste decidir o que fazer com o mouse.
REA_OVERLAY_API int WINAPI ReaOverlayWantsMouseInput();
REA_OVERLAY_API int WINAPI ReaOverlayWantsKeyboardInput();

// Detalhe textual do ultimo erro de Attach (buffer valido ate a proxima chamada
// no mesmo thread).
REA_OVERLAY_API const char* WINAPI ReaOverlayLastError();

// Auto-attach (padrao ligado): a DLL instala hooks de DXGI na carga e anexa ao
// primeiro Present de uma janela visivel do processo. Desligue antes de chamar
// ReaOverlayAttach manualmente.
REA_OVERLAY_API void WINAPI ReaOverlaySetAutoAttach(int enabled);
REA_OVERLAY_API void WINAPI ReaOverlayInstallHooks();
