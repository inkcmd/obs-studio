# XINKX Studio is compiled into the OBS frontend; it is not a runtime plugin.
target_sources(
  obs-studio
  PRIVATE
    xinkx/StudioController.cpp
    xinkx/StudioController.hpp
    xinkx/StudioWorkspace.cpp
    xinkx/StudioWorkspace.hpp
    xinkx/StudioAutomation.cpp
    xinkx/StudioAutomation.hpp
    xinkx/MultiStreamManager.cpp
    xinkx/MultiStreamManager.hpp
    xinkx/PortraitCanvas.cpp
    xinkx/PortraitCanvas.hpp
    xinkx/MultiStreamWorkspace.cpp
    xinkx/MultiStreamWorkspace.hpp
    xinkx/StreamAccounts.cpp
    xinkx/StreamAccounts.hpp
    xinkx/PlatformApi.cpp
    xinkx/PlatformApi.hpp
    xinkx/OAuthLogin.cpp
    xinkx/OAuthLogin.hpp
    xinkx/OverlayRuntime.cpp
    xinkx/OverlayRuntime.hpp
    xinkx/NativeSources.cpp
    xinkx/NativeSources.hpp
    xinkx/StudioSettingsPages.cpp
    xinkx/StudioSettingsPages.hpp
    xinkx/ChatGateway.cpp
    xinkx/ChatGateway.hpp
    xinkx/ChatWorkspace.cpp
	 xinkx/PresentationSettings.cpp
	 xinkx/PresentationSettings.hpp
	 xinkx/PresentationWorkspace.cpp
	 xinkx/BroadcastWorkspace.cpp
	 xinkx/YouTubeStudioBridge.cpp
	 xinkx/YouTubeStudioBridge.hpp
	 xinkx/GameProfilesWorkspace.cpp
	 xinkx/SceneJsonWorkspace.cpp
	 xinkx/DonationGateway.cpp
	 xinkx/DonationGateway.hpp
	 xinkx/DonationsWorkspace.cpp
    xinkx/NativeWebSocket.cpp
    xinkx/NativeWebSocket.hpp
    xinkx/assets/overlay.qrc
)

target_compile_definitions(obs-studio PRIVATE XINKX_STUDIO=1)

if(WIN32)
  target_link_libraries(obs-studio PRIVATE crypt32 winhttp)
endif()
