cmake_minimum_required(VERSION 3.6...3.31)

project(CurvedStereoGS LANGUAGES C CXX)
# Keep the established executable name for existing scripts and the Qt launcher.
set(CURVED_STEREO_GS_RENDERER_TARGET CurvedStereoGS)

# Load version from VERSION file and expose to CMake and C++
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CURVED_STEREO_GS_VERSION_RAW)
string(STRIP "${CURVED_STEREO_GS_VERSION_RAW}" CURVED_STEREO_GS_VERSION)
message(STATUS "CURVED_STEREO_GS_VERSION: ${CURVED_STEREO_GS_VERSION}")

# Add the cmake folder to the module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(NvproCore2 REQUIRED)
find_package(OpenGL REQUIRED)

# set with -DDISABLE_DEFAULT_SCENE=ON on command line or use cmake-gui to set the parameter
# if default scene is not disabled at build using this option, one can still disable it by 
# providing "-loadDefaultScene 0" to the sample command line
option(DISABLE_DEFAULT_SCENE "If set to true the default scene is not downloaded by CMake, and the sample does not try to open it when starting up with no scene as parameter" OFF)

# DLSS Ray Reconstruction support
option(USE_DLSS "Use DLSS-RR for denoising" OFF)

# Download the default scene
if (NOT DISABLE_DEFAULT_SCENE)
  add_definitions(-DWITH_DEFAULT_SCENE_FEATURE)
  download_files(FILENAMES flowers_1.zip EXTRACT)
endif()

#####################################################################################
# additions of 3rdparty

#####
# Include Vulkan Radix Sort
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/vrdx)

set_property(TARGET vk_radix_sort PROPERTY FOLDER "ThirdParty/vrdx")
set_property(TARGET spine_slang PROPERTY FOLDER "ThirdParty/vrdx")
set_property(TARGET downsweep_slang PROPERTY FOLDER "ThirdParty/vrdx")
set_property(TARGET downsweep_key_value_slang PROPERTY FOLDER "ThirdParty/vrdx")
set_property(TARGET upsweep_slang PROPERTY FOLDER "ThirdParty/vrdx")

#####
# Include spz (uses zlib from nvpro_core2)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/spz)
set_property(TARGET spz PROPERTY FOLDER "ThirdParty")

#####################################################################################
# Source files for this project
#
file(GLOB SOURCE_FILES src/*.*)
file(GLOB SHADER_FILES shaders/*.slang shaders/*.h)
file(GLOB EXTERN_FILES 3rdparty/miniply/*.*)

list(REMOVE_ITEM SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.hpp
)

set(CURVED_STEREO_GS_MAIN1_SOURCE_FILES ${SOURCE_FILES})
set(CURVED_STEREO_GS_KZ_SOURCE_FILES ${SOURCE_FILES})
set(CURVED_STEREO_GS_NV_SOURCE_FILES ${SOURCE_FILES})
list(REMOVE_ITEM SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main1.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mainnv.cpp
)
list(REMOVE_ITEM CURVED_STEREO_GS_MAIN1_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mainnv.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.h
)
list(REMOVE_ITEM CURVED_STEREO_GS_KZ_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main1.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mainnv.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.h
)
list(REMOVE_ITEM CURVED_STEREO_GS_NV_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main1.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/active_stereo_presenter.h
)

set(CURVED_STEREO_GS_TEST_SOURCE_FILES ${SOURCE_FILES})
list(REMOVE_ITEM CURVED_STEREO_GS_TEST_SOURCE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/src 
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders 
  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/miniply
  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/vrdx
  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/spz/src/cc)

#####################################################################################
# Executable
#

add_executable(${CURVED_STEREO_GS_RENDERER_TARGET} ${SOURCE_FILES} ${COMMON_SOURCE_FILES} ${PACKAGE_SOURCE_FILES} ${SHADER_FILES} ${EXTERN_FILES})

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${CURVED_STEREO_GS_RENDERER_TARGET})

target_compile_definitions(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE IMGUI_DEFINE_MATH_OPERATORS )
target_compile_definitions(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE CURVED_STEREO_GS_VERSION="${CURVED_STEREO_GS_VERSION}")

add_executable(CurvedStereoGS_Main1
  ${CURVED_STEREO_GS_MAIN1_SOURCE_FILES}
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
  ${SHADER_FILES}
  ${EXTERN_FILES}
)

target_compile_definitions(CurvedStereoGS_Main1 PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(CurvedStereoGS_Main1 PRIVATE CURVED_STEREO_GS_VERSION="${CURVED_STEREO_GS_VERSION}")

add_executable(CurvedStereoGS_KZ
  ${CURVED_STEREO_GS_KZ_SOURCE_FILES}
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
  ${SHADER_FILES}
  ${EXTERN_FILES}
)

target_compile_definitions(CurvedStereoGS_KZ PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(CurvedStereoGS_KZ PRIVATE CURVED_STEREO_GS_VERSION="${CURVED_STEREO_GS_VERSION}")

add_executable(CurvedStereoGS_NV
  ${CURVED_STEREO_GS_NV_SOURCE_FILES}
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
  ${SHADER_FILES}
  ${EXTERN_FILES}
)

target_compile_definitions(CurvedStereoGS_NV PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(CurvedStereoGS_NV PRIVATE CURVED_STEREO_GS_VERSION="${CURVED_STEREO_GS_VERSION}")

add_executable(CurvedStereoGS_Test
  ${CMAKE_CURRENT_SOURCE_DIR}/src/test.cpp
  ${CURVED_STEREO_GS_TEST_SOURCE_FILES}
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
  ${SHADER_FILES}
  ${EXTERN_FILES}
)

target_compile_definitions(CurvedStereoGS_Test PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(CurvedStereoGS_Test PRIVATE CURVED_STEREO_GS_VERSION="${CURVED_STEREO_GS_VERSION}")

#####################################################################################
# common source code needed for this sample
#
source_group(common FILES 
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
)
source_group("Shader Files" FILES ${SHADER_FILES})
source_group("Source Files" FILES ${SOURCE_FILES})
source_group("Extern Files" FILES ${EXTERN_FILES})

#####################################################################################
# Linkage
#

# Linking with other libraries
# Link libraries and include directories
target_link_libraries(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE
  nvpro2::nvapp

  nvpro2::nvvkglsl
  nvpro2::nvslang
  nvpro2::nvshaders_host
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgpu_monitor
  gl_headers
  OpenGL::GL
  vk_radix_sort
  spz::spz
  tinygltf
)

target_link_libraries(CurvedStereoGS_Main1 PRIVATE
  nvpro2::nvapp

  nvpro2::nvvkglsl
  nvpro2::nvslang
  nvpro2::nvshaders_host
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgpu_monitor
  vk_radix_sort
  spz::spz
  tinygltf
)

target_link_libraries(CurvedStereoGS_KZ PRIVATE
  nvpro2::nvapp

  nvpro2::nvvkglsl
  nvpro2::nvslang
  nvpro2::nvshaders_host
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgpu_monitor
  vk_radix_sort
  spz::spz
  tinygltf
)

target_link_libraries(CurvedStereoGS_NV PRIVATE
  nvpro2::nvapp

  nvpro2::nvvkglsl
  nvpro2::nvslang
  nvpro2::nvshaders_host
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgpu_monitor
  vk_radix_sort
  spz::spz
  tinygltf
)

target_link_libraries(CurvedStereoGS_Test PRIVATE
  nvpro2::nvapp

  nvpro2::nvvkglsl
  nvpro2::nvslang
  nvpro2::nvshaders_host
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgpu_monitor
  OpenGL::GL
  vk_radix_sort
  spz::spz
  tinygltf
)

# Optional Qt control center.  The renderer remains a separate process so its
# GLFW/Vulkan loop and native WGL quad-buffer pixel format are unaffected.
option(CURVED_STEREO_GS_BUILD_QT_CONTROL_CENTER "Build the Qt control center for the stereo renderer" ON)
if(CURVED_STEREO_GS_BUILD_QT_CONTROL_CENTER)
  find_package(Qt6 6.5 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    add_executable(CurvedStereoGS_ControlCenter WIN32 qt/qt_control_center.cpp)
    target_compile_features(CurvedStereoGS_ControlCenter PRIVATE cxx_std_20)
    target_link_libraries(CurvedStereoGS_ControlCenter PRIVATE Qt6::Widgets)
    add_dependencies(CurvedStereoGS_ControlCenter ${CURVED_STEREO_GS_RENDERER_TARGET})
    set_target_properties(CurvedStereoGS_ControlCenter PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/_bin"
    )

    if(WIN32)
      find_program(CURVED_STEREO_GS_WINDEPLOYQT windeployqt
        HINTS "${Qt6_DIR}/../../../bin"
        NO_DEFAULT_PATH)
      if(CURVED_STEREO_GS_WINDEPLOYQT)
        add_custom_command(TARGET CurvedStereoGS_ControlCenter POST_BUILD
          COMMAND "${CURVED_STEREO_GS_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                  "$<TARGET_FILE:CurvedStereoGS_ControlCenter>"
          COMMENT "Deploying Qt runtime beside CurvedStereoGS_ControlCenter")
      endif()
    endif()
  else()
    message(STATUS "Qt 6.5+ Widgets not found: CurvedStereoGS_ControlCenter will not be built")
  endif()
endif()

add_project_definitions(${CURVED_STEREO_GS_RENDERER_TARGET})
add_project_definitions(CurvedStereoGS_Main1)
add_project_definitions(CurvedStereoGS_KZ)
add_project_definitions(CurvedStereoGS_NV)
add_project_definitions(CurvedStereoGS_Test)

#####################################################################################
# Adding DLSS if selected
if(USE_DLSS)
  set(DLSS_VERSION "310.5.0")
  find_package(NGX REQUIRED)
  target_link_libraries(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE ngx)
  target_link_libraries(CurvedStereoGS_Main1 PRIVATE ngx)
  target_link_libraries(CurvedStereoGS_KZ PRIVATE ngx)
  target_link_libraries(CurvedStereoGS_NV PRIVATE ngx)
  target_compile_definitions(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE USE_DLSS)
  target_compile_definitions(CurvedStereoGS_Main1 PRIVATE USE_DLSS)
  target_compile_definitions(CurvedStereoGS_KZ PRIVATE USE_DLSS)
  target_compile_definitions(CurvedStereoGS_NV PRIVATE USE_DLSS)
  target_sources(${CURVED_STEREO_GS_RENDERER_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.hpp
  )
  target_sources(CurvedStereoGS_Main1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.hpp
  )
  target_sources(CurvedStereoGS_KZ PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.hpp
  )
  target_sources(CurvedStereoGS_NV PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_wrapper.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_denoiser.hpp
  )
endif()

#####################################################################################
# copies files
#

copy_to_runtime_and_install(${CURVED_STEREO_GS_RENDERER_TARGET}
  DIRECTORIES "${NVSHADERS_DIR}/nvshaders"
  FILES ${Slang_GLSLANG} ${DLSS_DLLS}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)

copy_to_runtime_and_install(CurvedStereoGS_Main1
  DIRECTORIES "${NVSHADERS_DIR}/nvshaders"
  FILES ${Slang_GLSLANG} ${DLSS_DLLS}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)

copy_to_runtime_and_install(CurvedStereoGS_KZ
  DIRECTORIES "${NVSHADERS_DIR}/nvshaders"
  FILES ${Slang_GLSLANG} ${DLSS_DLLS}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)

copy_to_runtime_and_install(CurvedStereoGS_NV
  DIRECTORIES "${NVSHADERS_DIR}/nvshaders"
  FILES ${Slang_GLSLANG} ${DLSS_DLLS}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)

copy_to_runtime_and_install(CurvedStereoGS_Test
  DIRECTORIES "${NVSHADERS_DIR}/nvshaders"
  FILES ${Slang_GLSLANG}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)
