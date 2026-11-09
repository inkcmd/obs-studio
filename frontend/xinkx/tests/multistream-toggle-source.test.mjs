// Source-level regression guard for the native RTMP menu state machine.
// Runtime streaming and OBS API calls still require a Windows build/test.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const code = readFileSync(new URL('../MultiStreamManager.cpp', import.meta.url), 'utf8');
const header = readFileSync(new URL('../MultiStreamManager.hpp', import.meta.url), 'utf8');

assert.match(code, /connect\(action, &QAction::toggled/);
assert.match(code, /if \(liveMode\)/);
assert.match(code, /if \(current\) requestRestart\(\*current\)/);
assert.match(code, /else start\(account\)/);
assert.match(code, /session\.stats = OutputStats\{\}/);
assert.match(code, /QTimer::singleShot\(500, this, \[this, serial\]/);
assert.match(code, /entry->stopping \|\| \(entry->output && obs_output_active\(entry->output\)\)/);
assert.match(code, /entry->restartRequested && manager->liveMode/);
assert.match(code, /void MultiStreamManager::stopAll\(bool force\)\s*\{\s*liveMode = false/);
assert.match(header, /bool restartRequested = false/);
assert.match(header, /bool restartPending = false/);
assert.match(code, /Duplicate ingest destination blocked/);
assert.match(code, /obs_output_get_total_frames\(output\)/);
assert.match(code, /PortraitCanvas::instance\(\)\.sync\(\)/);
assert.doesNotMatch(code, /action->setEnabled\(session->running && !session->stopping\)/);

console.log('XINKX multi-output toggle source contract: stop/restart, deferred resume and global stop OK');
