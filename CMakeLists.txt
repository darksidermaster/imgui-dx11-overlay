cmake_minimum_required(VERSION 3.21)

project(rea_imgui_overlay LANGUAGES C CXX)

# Extrai as dependencias com o timestamp do download (CMP0135).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(NOT WIN32)
  message(FATAL_ERROR "Este exemplo usa Win32, DirectX 11 e MinHook; compile no Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencias fixadas por versao e hash. O primeiro configure precisa de rede.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  URL_HASH SHA256=21D8A0A565E85DCE943E375DB00812C2F3F0AB21F3F0F7964E364A63422D7F99
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  # O repositorio do Dear ImGui nao publica CMakeLists.txt; por isso populamos
  # as fontes e definimos o alvo manualmente.
  FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi)

FetchContent_Declare(
  minhook
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
  URL_HASH SHA256=1AEBEAE4CA898330C507860ACC2FCA2EB335FE446A3A2B8444C3BF8B2660A14E
)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
  FetchContent_Populate(minhook)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(MINHOOK_HDE ${minhook_SOURCE_DIR}/src/hde/hde64.c)
else()
  set(MINHOOK_HDE ${minhook_SOURCE_DIR}/src/hde/hde32.c)
endif()

add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${MINHOOK_HDE}
)
target_include_directories(minhook PUBLIC
  ${minhook_SOURCE_DIR}/include
)
target_include_directories(minhook PRIVATE
  ${minhook_SOURCE_DIR}/src
  ${minhook_SOURCE_DIR}/src/hde
)

# ---------------------------------------------------------------------------
# DLL de overlay
# ---------------------------------------------------------------------------
add_library(rea_overlay SHARED
  src/overlay/overlay.cpp
  src/overlay/dllmain.cpp
  src/overlay/overlay_api.h
)
target_link_libraries(rea_overlay PRIVATE imgui minhook d3d11 dxgi comctl32)
target_compile_definitions(rea_overlay PRIVATE
  REA_OVERLAY_EXPORTS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

# ---------------------------------------------------------------------------
# Aplicativo hospedeiro ("jogo" de teste)
# ---------------------------------------------------------------------------
add_executable(overlay_host WIN32
  src/host/main.cpp
)
target_include_directories(overlay_host PRIVATE src/overlay)
target_link_libraries(overlay_host PRIVATE d3d11 dxgi d3dcompiler)
target_compile_definitions(overlay_host PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

if(MSVC)
  foreach(target rea_overlay overlay_host)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endforeach()
endif()

# Deixe a DLL ao lado do executavel para que o teste funcione sem copiar nada.
add_custom_command(TARGET overlay_host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:rea_overlay>
          $<TARGET_FILE_DIR:overlay_host>
  COMMENT "Copiando rea_overlay.dll para o diretorio do host"
)
