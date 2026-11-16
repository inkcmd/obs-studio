import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const native = read('../NativeSources.cpp');
const header = read('../NativeSources.hpp');
const basic = read('../../widgets/OBSBasic.cpp');

test('native XINKX browser children are released before global scene cleanup and CEF module unload', () => {
  assert.match(header, /void shutdownNativeBrowserSources\(\);/);
  const stop = basic.indexOf('xinkx::shutdownStudioOutputs(this);');
  const browsers = basic.indexOf('xinkx::shutdownNativeBrowserSources();', stop);
  const scenes = basic.indexOf('ClearSceneData();', browsers);
  assert.ok(stop >= 0 && browsers > stop && scenes > browsers);
  assert.match(native, /obs_enum_sources\(/);
  assert.match(native, /obs_source_update\(parent, shutdown\)/);
  assert.match(native, /xinkx_shutdown_browser/);
});

test('private CEF browser is detached once, and rendering holds a libobs reference', () => {
  assert.match(native, /std::exchange\(source->browser, nullptr\)/);
  assert.match(native, /obs_source_remove_active_child\(source->parent, browser\)/);
  assert.match(native, /obs_source_release\(browser\)/);
  assert.match(native, /obs_source_get_ref\(source->browser\)/);
  assert.match(native, /obs_source_video_render\(browser\)/);
  assert.match(native, /if \(source->shuttingDown\) return;/);
  assert.match(native, /obs_source_update\(browser, browserSettings\)/);
});


test('frontend browser docks request asynchronous CEF close before Qt owner teardown', () => {
  const start = basic.indexOf('void OBSBasic::closeWindow()');
  const closeBrowser = basic.indexOf('dock->cefWidget->closeBrowser();', start);
  const auth = basic.indexOf('auth.reset();', start);
  const clearDocks = basic.indexOf('ClearExtraBrowserDocks();', start);
  const clearScenes = basic.indexOf('ClearSceneData();', start);
  assert.ok(start >= 0 && closeBrowser > start);
  assert.ok(closeBrowser < auth && auth < clearDocks && clearDocks < clearScenes);
  assert.match(basic, /findChildren<BrowserDock \*>\(\)/);
  assert.match(basic, /cef && obs_browser_qcef_version\(\) >= 2/);
  assert.match(basic, /#include <docks\/BrowserDock\.hpp>/);
});
