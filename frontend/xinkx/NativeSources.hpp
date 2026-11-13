#pragma once

namespace xinkx {

// Register XINKX as genuine OBS input types (Sources -> +).
// Their video is rendered by the bundled obs-browser module.
void registerNativeSources();

// Close private CEF browser children before OBS releases the scene collection.
// Safe to call more than once; only affects XINKX-native source types.
void shutdownNativeBrowserSources();

} // namespace xinkx
