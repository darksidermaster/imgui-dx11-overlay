#include <windows.h>

extern "C" void ReaOverlayAutoStart();

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    // Nada pesado aqui: hooks, COM, janela e ImGui nao podem ser inicializados
    // sob o loader lock. A thread so roda depois que o loader libera a DLL.
    ReaOverlayAutoStart();
  }
  return TRUE;
}
