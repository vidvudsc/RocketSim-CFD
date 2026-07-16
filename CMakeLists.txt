cmake_minimum_required(VERSION 3.24)
project(RocketSim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_program(GLSLC glslc REQUIRED)

set(FLOW_SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/flow.comp.spv")
add_custom_command(OUTPUT "${FLOW_SHADER_SPV}"
    COMMAND "${GLSLC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/flow.comp" -O -o "${FLOW_SHADER_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/flow.comp"
    VERBATIM)
add_custom_target(flow_shader DEPENDS "${FLOW_SHADER_SPV}")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

add_executable(rocketsim
    src/main.cpp
    src/FlowSolver.cpp
    src/FlowTexture.cpp
    src/VulkanFlowBackend.cpp
    src/HeadlessVulkanContext.cpp
    src/SimulationUI.cpp
    src/GifExporter.cpp
    src/BakeManager.cpp
)
target_include_directories(rocketsim PRIVATE src)
target_link_libraries(rocketsim PRIVATE imgui Vulkan::Vulkan glfw)
add_dependencies(rocketsim flow_shader)
target_compile_definitions(rocketsim PRIVATE ROCKETSIM_FLOW_SPV="${FLOW_SHADER_SPV}")
target_compile_options(rocketsim PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
)

include(CTest)
if(BUILD_TESTING)
    add_executable(solver_smoke tests/solver_smoke.cpp src/FlowSolver.cpp src/VulkanFlowBackend.cpp)
    target_include_directories(solver_smoke PRIVATE src)
    target_compile_options(solver_smoke PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    )
    target_link_libraries(solver_smoke PRIVATE Vulkan::Vulkan)
    add_test(NAME solver_smoke COMMAND solver_smoke)
    add_executable(gif_export_smoke tests/gif_export_smoke.cpp src/FlowSolver.cpp src/GifExporter.cpp src/VulkanFlowBackend.cpp src/HeadlessVulkanContext.cpp)
    target_include_directories(gif_export_smoke PRIVATE src)
    target_compile_options(gif_export_smoke PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    )
    target_link_libraries(gif_export_smoke PRIVATE Vulkan::Vulkan)
    target_compile_definitions(gif_export_smoke PRIVATE ROCKETSIM_FLOW_SPV="${FLOW_SHADER_SPV}")
    add_dependencies(gif_export_smoke flow_shader)
    add_test(NAME gif_export_smoke COMMAND gif_export_smoke)
    add_executable(bake_smoke tests/bake_smoke.cpp src/FlowSolver.cpp src/BakeManager.cpp src/GifExporter.cpp src/VulkanFlowBackend.cpp src/HeadlessVulkanContext.cpp)
    target_include_directories(bake_smoke PRIVATE src)
    target_compile_options(bake_smoke PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    )
    target_link_libraries(bake_smoke PRIVATE Vulkan::Vulkan)
    target_compile_definitions(bake_smoke PRIVATE ROCKETSIM_FLOW_SPV="${FLOW_SHADER_SPV}")
    add_dependencies(bake_smoke flow_shader)
    add_test(NAME bake_smoke COMMAND bake_smoke)
    add_executable(gpu_solver_smoke tests/gpu_solver_smoke.cpp src/FlowSolver.cpp src/VulkanFlowBackend.cpp)
    target_include_directories(gpu_solver_smoke PRIVATE src)
    target_link_libraries(gpu_solver_smoke PRIVATE Vulkan::Vulkan)
    target_compile_definitions(gpu_solver_smoke PRIVATE ROCKETSIM_FLOW_SPV="${FLOW_SHADER_SPV}")
    add_dependencies(gpu_solver_smoke flow_shader)
    target_compile_options(gpu_solver_smoke PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    )
    add_test(NAME gpu_solver_smoke COMMAND gpu_solver_smoke)
endif()

if(APPLE)
    # MoltenVK implements Vulkan on macOS. Homebrew's Vulkan package exports it
    # through Vulkan::Vulkan, and GLFW supplies the required Metal surface flags.
    target_compile_definitions(rocketsim PRIVATE VK_ENABLE_BETA_EXTENSIONS)
endif()
