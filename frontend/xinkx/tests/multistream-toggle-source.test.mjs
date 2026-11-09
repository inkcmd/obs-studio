// Source-level regression guard for the native RTMP menu state machine.
// Runtime streaming and OBS API calls still require a Windows build/test.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const code = readFileSync(new URL('../MultiStreamManager.cpp', import.meta.url), 'utf8');
const header = readFileSync(new URL('../MultiStreamManager.hpp', import.meta.url), 'utf8');

assert.match(code, /connect\(action, &QAction::toggled/);
assert.match(code, /if \(liveMode\)/);
assert.match(code, /if \(current\) restart\(\*current\)/);
assert.match(code, /else start\(account\)/);
assert.match(code, /session\.stats = OutputStats\{\}/);
assert.match(code, /if \(session\.stopping\) \{\s*session\.restartRequested = true/);
assert.match(code, /entry->restartRequested && manager->liveMode/);
assert.match(code, /void MultiStreamManager::stopAll\(bool force\)\s*\{\s*liveMode = false/);
assert.match(header, /bool restartRequested = false/);
assert.doesNotMatch(code, /action->setEnabled\(session->running && !session->stopping\)/);

console.log('XINKX multi-output toggle source contract: stop/restart, deferred resume and global stop OK');
