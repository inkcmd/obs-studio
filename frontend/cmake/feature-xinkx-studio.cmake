# XINKX Studio is compiled into the OBS frontend; it is not a runtime plugin.
target_sources(
  obs-studio
  PRIVATE
    xinkx/StudioController.cpp
    xinkx/StudioController.hpp
    xinkx/StudioWorkspace.cpp
    xinkx/StudioWorkspace.hpp
)

target_compile_definitions(obs-studio PRIVATE XINKX_STUDIO=1)
