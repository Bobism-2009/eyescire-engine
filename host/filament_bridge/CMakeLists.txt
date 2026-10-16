cmake_minimum_required(VERSION 3.19)
project(filament_bridge LANGUAGES CXX)

set(FROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/filament")
set(FOUT "${FROOT}/out-win")

add_library(filament_bridge SHARED ../filament_bridge.cpp ../filament_bridge.h)
target_compile_definitions(filament_bridge PRIVATE FILAMENT_BRIDGE_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(filament_bridge PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(filament_bridge PRIVATE /GR- /EHsc /W3)
endif()
target_include_directories(filament_bridge PRIVATE
    "${FROOT}/filament/include"
    "${FROOT}/filament/backend/include"
    "${FROOT}/libs/filamat/include"
    "${FROOT}/libs/filabridge/include"
    "${FROOT}/libs/utils/include"
    "${FROOT}/libs/math/include"
    "${FROOT}/libs/geometry/include"
)
target_link_libraries(filament_bridge PRIVATE
    "${FOUT}/filament/filament.lib"
    "${FOUT}/filament/backend/backend.lib"
    "${FOUT}/libs/bluegl/bluegl.lib"
    "${FOUT}/libs/filabridge/filabridge.lib"
    "${FOUT}/libs/filaflat/filaflat.lib"
    "${FOUT}/libs/filamat/filamat.lib"
    "${FOUT}/libs/geometry/geometry.lib"
    "${FOUT}/libs/utils/utils.lib"
    "${FOUT}/libs/math/math.lib"
    "${FOUT}/shaders/shaders.lib"
    "${FOUT}/third_party/smol-v/tnt/smol-v.lib"
    "${FOUT}/third_party/zstd/tnt/zstd.lib"
    "${FOUT}/third_party/mikktspace/mikktspace.lib"
    "${FOUT}/third_party/meshoptimizer/tnt/meshoptimizer.lib"
    "${FOUT}/third_party/glslang/tnt/glslang/glslang.lib"
    "${FOUT}/third_party/glslang/tnt/glslang/OSDependent/Windows/OSDependent.lib"
    "${FOUT}/third_party/glslang/tnt/SPIRV/SPIRV.lib"
    "${FOUT}/third_party/spirv-cross/tnt/spirv-cross-core.lib"
    "${FOUT}/third_party/spirv-cross/tnt/spirv-cross-glsl.lib"
    "${FOUT}/third_party/spirv-cross/tnt/spirv-cross-msl.lib"
    "${FOUT}/third_party/spirv-tools/source/SPIRV-Tools.lib"
    "${FOUT}/third_party/spirv-tools/source/opt/SPIRV-Tools-opt.lib"
    opengl32
    gdi32
    user32
    Shlwapi
)
set_target_properties(filament_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../../bin"
)
