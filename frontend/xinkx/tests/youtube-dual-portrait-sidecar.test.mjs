import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const stream = readFileSync(new URL('../MultiStreamWorkspace.cpp', import.meta.url), 'utf8');
const portrait = readFileSync(new URL('../PortraitCanvas.cpp', import.meta.url), 'utf8');

for (const text of [
  'xinkxYoutubeDualMode',
  'editYouTubeChannel(group, *accounts, id)',
  'auth.listYouTubeStreams(horizontal)',
  'auth.createYouTubeStream(horizontal, name, format',
  'rtmpsIngestionAddress',
  'authorizationSourceId == account.id',
  'entry.authorizationSourceId == id',
  'horizontal.destination.streamKey == vertical.destination.streamKey',
]) assert.ok(stream.includes(text), `missing unified YouTube behavior: ${text}`);

for (const text of [
  'xinkxPortraitEditor',
  'previewLayout->addWidget(side, 1)',
  'preview->pressed =',
  'preview->resized =',
  'obs_sceneitem_get_box_transform',
  'obs_sceneitem_set_visible',
  'obs_save_source(source)',
  'obs_load_source(sourceData)',
  'Создать XINKX',
  'obs_frontend_open_source_properties',
  'delete editor;',
]) assert.ok(portrait.includes(text), `missing portrait sidecar behavior: ${text}`);

console.log('XINKX dual-mode YouTube OAuth and adjacent native portrait canvas source contract OK');
