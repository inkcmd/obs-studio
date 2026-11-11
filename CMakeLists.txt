cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(obs-studio VERSION ${OBS_VERSION_CANONICAL})

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/architecture.cmake")
  if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
    return()
  endif()
endif()

include(compilerconfig)
include(defaults)
include(helpers)

option(ENABLE_FRONTEND "Enable building with UI (requires Qt)" ON)
option(ENABLE_SCRIPTING "Enable scripting support" ON)
option(ENABLE_HEVC "Enable HEVC encoders" ON)

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
add_subdirectory(libobs-opengl)
if(OS_MACOS)
  add_subdirectory(libobs-metal)
endif()
add_subdirectory(plugins)

# On Windows the following x64/ARM64 plugin targets each start a nested x86
# MSBuild from POST_BUILD. They must not run concurrently: otherwise the nested
# builds race while writing build_x86's ZERO_CHECK.recipe.
if(OS_WINDOWS AND CMAKE_VS_PLATFORM_NAME MATCHES "^(x64|ARM64)$")
  set(_previous_x86_build_target "")
  foreach(_x86_build_target IN ITEMS graphics-hook get-graphics-offsets inject-helper obs-virtualcam-module)
    if(TARGET ${_x86_build_target})
      if(_previous_x86_build_target)
        add_dependencies(${_x86_build_target} ${_previous_x86_build_target})
      endif()
      set(_previous_x86_build_target ${_x86_build_target})
    endif()
  endforeach()
  unset(_previous_x86_build_target)
  unset(_x86_build_target)
endif()

add_subdirectory(test/test-input)

add_subdirectory(frontend)

message_configuration()
