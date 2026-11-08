# XINKX Studio is compiled into the OBS frontend; it is not a runtime plugin.
target_sources(
  obs-studio
  PRIVATE
    xinkx/StudioController.cpp
    xinkx/StudioController.hpp
    xinkx/StudioWorkspace.cpp
    xinkx/StudioWorkspace.hpp
    xinkx/MultiStreamManager.cpp
    xinkx/MultiStreamManager.hpp
    xinkx/MultiStreamWorkspace.cpp
    xinkx/MultiStreamWorkspace.hpp
    xinkx/StreamAccounts.cpp
    xinkx/StreamAccounts.hpp
    xinkx/OAuthLogin.cpp
    xinkx/OAuthLogin.hpp
)

target_compile_definitions(obs-studio PRIVATE XINKX_STUDIO=1)

if(WIN32)
  target_link_libraries(obs-studio PRIVATE crypt32)
endif()
