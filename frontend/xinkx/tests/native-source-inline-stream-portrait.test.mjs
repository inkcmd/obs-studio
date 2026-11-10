// Native XINKX integration contract. Run a Windows build for C++/Qt validation.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = filename => readFileSync(new URL(`../${filename}`, import.meta.url), 'utf8');
const sources = read('NativeSources.cpp');
const streams = read('MultiStreamWorkspace.cpp');
const portrait = read('PortraitCanvas.cpp');

// Every native XINKX source exposes the same saved local catalog; selecting a
// preset copies its settings to the instance without changing the catalog.
assert.match(sources, /QSettings storage\(path, QSettings::IniFormat\)/);
assert.match(sources, /catalog\/presets/);
assert.match(sources, /obs_property_set_modified_callback\(preset, presetModified\)/);
assert.match(sources, /userPresetId\(entry\) != selected/);
assert.match(sources, /obs_properties_add_button2\(properties, "saveXinkxPreset"/);
assert.match(sources, /xinkxAppearance/);

// Stream IDs are chosen from the authenticated channel's inline list, not a
// free-text field or a separate picker. Creation remains one focused dialog.
assert.match(streams, /xinkxInlineYouTubeStreams/);
assert.match(streams, /youtubeStreamsLoaded/);
assert.match(streams, /youtubeStreamCreated/);
assert.match(streams, /youtubeId->currentData\(\)\.toString\(\)/);
assert.doesNotMatch(streams, /youtubeId->text\(\)/);
assert.match(streams, /createYouTubeStream\(picker, title->text\(\)\.trimmed\(\)/);
assert.match(streams, /applyStreamResolution/);

// The portrait editor lives in OBS's central preview layout with visible
// resize handles, mouse transforms and independent persistent source choices.
assert.match(portrait, /xinkxPortraitEditor/);
assert.match(portrait, /obs_sceneitem_get_box_transform\(layers\.at\(row\)\.item, &box\)/);
assert.match(portrait, /preview->pressed =/);
assert.match(portrait, /preview->resized =/);
assert.match(portrait, /previewLayout->addWidget\(side, 1\)/);
assert.match(portrait, /extraKey\(sceneName\)/);

console.log('XINKX native source presets, inline YouTube streams and portrait editor contract OK');
