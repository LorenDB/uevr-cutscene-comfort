# CutsceneComfort, a UEVR plugin.
#
# The plugin only needs headers from a UEVR checkout plus the small renderlib
# helper the example plugins share. Nothing links against the UEVR build
# itself, the plugin talks to the runtime through the C ABI in uevr/API.h.

cmake_minimum_required(VERSION 3.24)
project(CutsceneComfort CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(UEVR_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../UEVR" CACHE PATH "Path to a UEVR source checkout with submodules")

set(UEVR_INCLUDE "${UEVR_REPO}/include")
set(RENDERLIB "${UEVR_REPO}/examples/renderlib")
set(IMGUI "${UEVR_REPO}/dependencies/submodules/imgui")
set(GLM "${UEVR_REPO}/dependencies/submodules/glm")

foreach(marker
    "${UEVR_INCLUDE}/uevr/Plugin.hpp"
    "${RENDERLIB}/rendering/d3d11.hpp"
    "${IMGUI}/imgui.cpp"
    "${GLM}/glm/glm.hpp")
    if(NOT EXISTS "${marker}")
        message(FATAL_ERROR "Missing ${marker}. Set UEVR_REPO to a UEVR checkout that has its submodules.")
    endif()
endforeach()

add_library(CutsceneComfort SHARED
    src/Plugin.cpp
    src/Vignette.cpp
    src/VignetteD3D11.cpp
    ${RENDERLIB}/rendering/d3d11.cpp
    src/RenderD3D12.cpp
    ${RENDERLIB}/imgui/imgui_impl_dx11.cpp
    ${RENDERLIB}/imgui/imgui_impl_dx12.cpp
    ${RENDERLIB}/imgui/imgui_impl_win32.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_demo.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
)

target_include_directories(CutsceneComfort PRIVATE
    ${UEVR_INCLUDE}
    ${RENDERLIB}
    ${IMGUI}
    ${GLM}
)

target_compile_definitions(CutsceneComfort PRIVATE NOMINMAX)

target_link_libraries(CutsceneComfort PRIVATE
    d3d11
    d3d12
    dxgi
    d3dcompiler
    user32
    comctl32
)

set_target_properties(CutsceneComfort PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

include(CTest)
if(BUILD_TESTING)
    add_executable(CutsceneComfortShaderTest
        tests/ShaderCompileTest.cpp
    )
    target_include_directories(CutsceneComfortShaderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(CutsceneComfortShaderTest PRIVATE NOMINMAX)
    target_link_libraries(CutsceneComfortShaderTest PRIVATE d3dcompiler)
    add_test(NAME CutsceneComfort.ShaderCompile COMMAND CutsceneComfortShaderTest)

    add_executable(CutsceneComfortUnvrTest
        tests/UnvrTransitionTest.cpp
    )
    target_include_directories(CutsceneComfortUnvrTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME CutsceneComfort.UnvrTransition COMMAND CutsceneComfortUnvrTest)
endif()
