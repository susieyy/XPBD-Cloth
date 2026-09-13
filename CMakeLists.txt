cmake_minimum_required(VERSION 3.29)
project(XPBDCloth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

include(CTest)

option(XPBD_CLOTH_BUILD_APP "Build the Vulkan GUI simulator" ON)
option(XPBD_CLOTH_BUILD_VULKAN_PROBE "Build the headless Vulkan GPU probe" OFF)
set(XPBD_CLOTH_VULKAN_DRIVER_FILES "" CACHE FILEPATH
    "Optional VK_DRIVER_FILES value for the Vulkan probe CTest")

add_library(XPBDClothNumericReference STATIC
  src/numeric/reference_math.cpp
)
set_target_properties(XPBDClothNumericReference PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(XPBDClothNumericReference PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/numeric
)

add_executable(XPBDClothNumericProbe tools/numeric_probe.cpp)
set_target_properties(XPBDClothNumericProbe PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(XPBDClothNumericProbe PRIVATE XPBDClothNumericReference)

if(BUILD_TESTING)
  add_executable(XPBDClothNumericTests tests/numeric_reference_tests.cpp)
  set_target_properties(XPBDClothNumericTests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(XPBDClothNumericTests PRIVATE XPBDClothNumericReference)
  add_test(NAME XPBDClothNumericTests COMMAND XPBDClothNumericTests)
endif()

if(XPBD_CLOTH_BUILD_VULKAN_PROBE)
  find_package(Vulkan REQUIRED)
  find_program(XPBD_GLSLC NAMES glslc REQUIRED)

  set(VULKAN_AREA_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_area.comp)
  set(VULKAN_AREA_SPV ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv/solve_area.comp.spv)
  set(VULKAN_WIND_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/wind.comp)
  set(VULKAN_WIND_SPV ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv/wind.comp.spv)
  set(VULKAN_STAGE_NAMES
    integrate solve_stretch solve_shear solve_bend apply_deltas solve_lra collide_sdf
    update_velocity tri_normal vertex_normal)
  add_custom_command(
    OUTPUT ${VULKAN_AREA_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv
    COMMAND ${XPBD_GLSLC} --target-env=vulkan1.3 -O -g
            ${VULKAN_AREA_SHADER} -o ${VULKAN_AREA_SPV}
    DEPENDS ${VULKAN_AREA_SHADER} shaders/common/simulation_common.glsl
    VERBATIM
  )

  set(VULKAN_STAGE_SPV)
  foreach(STAGE_NAME IN LISTS VULKAN_STAGE_NAMES)
    set(STAGE_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/${STAGE_NAME}.comp)
    set(STAGE_SPV ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv/${STAGE_NAME}.comp.spv)
    add_custom_command(
      OUTPUT ${STAGE_SPV}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv
      COMMAND ${XPBD_GLSLC} --target-env=vulkan1.3 -O -g
              ${STAGE_SHADER} -o ${STAGE_SPV}
      DEPENDS ${STAGE_SHADER} shaders/common/simulation_common.glsl
      VERBATIM
    )
    list(APPEND VULKAN_STAGE_SPV ${STAGE_SPV})
  endforeach()
  add_custom_command(
    OUTPUT ${VULKAN_WIND_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders/spv
    COMMAND ${XPBD_GLSLC} --target-env=vulkan1.3 -O -g
            ${VULKAN_WIND_SHADER} -o ${VULKAN_WIND_SPV}
    DEPENDS ${VULKAN_WIND_SHADER} shaders/common/simulation_common.glsl
    VERBATIM
  )

  add_executable(XPBDClothVulkanAreaProbe
    tools/vulkan_area_probe.cpp ${VULKAN_AREA_SPV} ${VULKAN_WIND_SPV})
  set_target_properties(XPBDClothVulkanAreaProbe PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(XPBDClothVulkanAreaProbe PRIVATE Vulkan::Vulkan)
  target_compile_definitions(XPBDClothVulkanAreaProbe PRIVATE
    XPBD_AREA_SHADER_PATH="${VULKAN_AREA_SPV}"
    XPBD_WIND_SHADER_PATH="${VULKAN_WIND_SPV}"
  )
  add_executable(XPBDClothVulkanStageProbe
    tools/vulkan_stage_probe.cpp ${VULKAN_STAGE_SPV})
  set_target_properties(XPBDClothVulkanStageProbe PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(XPBDClothVulkanStageProbe PRIVATE Vulkan::Vulkan)
  foreach(STAGE_NAME IN LISTS VULKAN_STAGE_NAMES)
    string(TOUPPER ${STAGE_NAME} STAGE_DEFINE)
    target_compile_definitions(XPBDClothVulkanStageProbe PRIVATE
      XPBD_${STAGE_DEFINE}_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/shaders/spv/${STAGE_NAME}.comp.spv")
  endforeach()
  if(BUILD_TESTING)
    add_test(NAME XPBDClothVulkanAreaProbe COMMAND XPBDClothVulkanAreaProbe)
    add_test(NAME XPBDClothVulkanStageProbe COMMAND XPBDClothVulkanStageProbe)
    if(XPBD_CLOTH_VULKAN_DRIVER_FILES)
      set_tests_properties(XPBDClothVulkanAreaProbe XPBDClothVulkanStageProbe PROPERTIES
        ENVIRONMENT "VK_DRIVER_FILES=${XPBD_CLOTH_VULKAN_DRIVER_FILES}")
    endif()
  endif()
endif()

if(XPBD_CLOTH_BUILD_APP)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(KTX CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)

set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

set_source_files_properties(${IMGUI_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_subdirectory(external/vulkan_radix_sort)

file(GLOB SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/simulation/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/model/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/interaction/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_executable(XPBDCloth ${SRC})

target_precompile_headers(XPBDCloth PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/util/pch.h>"
)

if (MSVC)

target_compile_options(XPBDCloth PRIVATE 
    $<$<CONFIG:Debug>:/Od /RTC1>                         # Debug
    $<$<CONFIG:Release>:/O2 /Ob2 /DNDEBUG>               # Release
)
add_custom_command(TARGET XPBDCloth POST_BUILD
COMMAND ${CMAKE_COMMAND} -E make_directory 
"$<TARGET_FILE_DIR:XPBDCloth>/assets/"
COMMAND ${CMAKE_COMMAND} -E copy_directory 
"${CMAKE_SOURCE_DIR}/assets/" "$<TARGET_FILE_DIR:XPBDCloth>/assets/"
COMMAND ${CMAKE_COMMAND} -E make_directory 
"$<TARGET_FILE_DIR:XPBDCloth>/shaders"
COMMAND ${CMAKE_COMMAND} -E copy_directory 
"${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:XPBDCloth>/shaders")

endif()


add_library(VulkanCppModule STATIC)
add_library(Vulkan::cppm ALIAS VulkanCppModule)

target_link_libraries(VulkanCppModule PUBLIC Vulkan::Vulkan)
target_include_directories(VulkanCppModule PRIVATE "${Vulkan_INCLUDE_DIR}")

target_compile_definitions(VulkanCppModule PUBLIC
  VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
  VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1
)

target_sources(VulkanCppModule
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES
    BASE_DIRS "${Vulkan_INCLUDE_DIR}"
    FILES     "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm")

target_link_libraries(XPBDCloth
  PRIVATE
    Vulkan::cppm
    glfw
    imgui
    KTX::ktx
    vk_radix_sort
    fmt::fmt-header-only
)
target_include_directories(XPBDCloth
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/simulation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interaction
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (NOT GLSLC_EXECUTABLE)
  find_program(GLSLC_EXECUTABLE NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
endif()

if (NOT GLSLC_EXECUTABLE)
  find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
  if (NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "Could not find glslc or glslangValidator.")
  endif()
endif()

function(compile_glsl_shaders TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

  set(SHADERS_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders/spv)
  file(MAKE_DIRECTORY ${SHADERS_OUTPUT_DIR})

  set(SPIRV_OUTPUTS "")

  foreach(SRC IN LISTS ARG_SOURCES)
    get_filename_component(BASENAME ${SRC} NAME)
    set(OUT ${SHADERS_OUTPUT_DIR}/${BASENAME}.spv)

    get_filename_component(EXT ${SRC} EXT)
    string(TOLOWER ${EXT} EXT)

    if (GLSLC_EXECUTABLE)
      add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLC_EXECUTABLE}
                --target-env=vulkan1.3
                -O -g
                ${SRC}
                -o ${OUT}
        DEPENDS ${SRC}
        COMMENT "Compiling GLSL ${BASENAME} -> ${OUT}"
        VERBATIM
      )
    else()
      add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLANG_VALIDATOR}
                -V --target-env vulkan1.3
                -g
                -o ${OUT} ${SRC}
        DEPENDS ${SRC}
        COMMENT "Compiling GLSL ${BASENAME} -> ${OUT}"
        VERBATIM
      )
    endif()

    list(APPEND SPIRV_OUTPUTS ${OUT})
  endforeach()

  add_custom_target(${TARGET} DEPENDS ${SPIRV_OUTPUTS})
endfunction()

set(OBJECT_RENDER_SHADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/model.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/model.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/cloth.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/cloth.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/softbody.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/softbody.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/skybox.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/skybox.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/lighting.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/lighting.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/skinned_model.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/skinned_model.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/debug_capsule.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/debug_capsule.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/shadow_model.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/shadow_particle.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/infinite_grid.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/object/infinite_grid.frag
)
compile_glsl_shaders(object_render_shaders_target SOURCES ${OBJECT_RENDER_SHADERS})

set(SIMULATION_SHADERS 
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/wind.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/integrate.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/clear_lambdas.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_stretch.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_shear.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_bend.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_area.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_self_collision.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/apply_deltas.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/solve_lra.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/collide_sdf.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/update_velocity.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/tri_normal.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/simulation/vertex_normal.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hash/build_hash.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hash/build_cell.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hash/build_neighbor.comp
)
compile_glsl_shaders(simulation_shaders_target SOURCES ${SIMULATION_SHADERS})

add_dependencies(XPBDCloth object_render_shaders_target simulation_shaders_target)
endif()
