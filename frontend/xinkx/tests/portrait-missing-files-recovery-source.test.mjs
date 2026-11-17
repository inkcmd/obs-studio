import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = path => readFileSync(new URL(`../../../${path}`, import.meta.url), 'utf8');
const scenes = read('frontend/widgets/OBSBasic_SceneCollections.cpp');
const dialog = read('frontend/dialogs/OBSMissingFiles.cpp');
const portrait = read('frontend/xinkx/PortraitCanvas.cpp');
const header = read('frontend/xinkx/PortraitCanvas.hpp');

// The portrait editor serializes extra sources separately from the standard
// scene collection. Rebuild it before collecting the live missing-file list.
assert.match(scenes, /PortraitCanvas::instance\(\)\.sync\(\)/);
assert.match(scenes, /obs_enum_all_sources\(collectSource, liveFiles\)/);
assert.match(scenes, /obs_missing_files_destroy\(files\)/);
assert.match(scenes, /obs_missing_files_count\(liveFiles\) > 0/);

// Merely finding a path does not apply it. The Apply handler must refresh
// portrait source snapshots after OBS missing-file callbacks update sources.
assert.match(dialog, /obs_missing_file_issue_callback\(f, p\)/);
assert.match(dialog, /if \(replaced\) \{\s*xinkx::PortraitCanvas::instance\(\)\.persistRecoveredSourceSettings\(\)/);
assert.match(dialog, /main->SaveProject\(\)/);
assert.match(header, /void persistRecoveredSourceSettings\(\)/);
assert.match(portrait, /void PortraitCanvas::persistRecoveredSourceSettings\(\)/);
assert.match(portrait, /obs_get_source_by_uuid\(uuid\.toUtf8\(\)\.constData\(\)\)/);
assert.match(portrait, /obs_save_source\(source\)/);
assert.match(portrait, /if \(changed\) store\.setValue\(key,/);
console.log('INK missing files: portrait collection and persistent source snapshots OK');
