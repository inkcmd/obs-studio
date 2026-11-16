import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';

const file = (name) => readFileSync(new URL(`../../../${name}`, import.meta.url), 'utf8');

test('monitoring duplication stores a weak source reference, not a borrowed pointer', () => {
  const internal = file('libobs/obs-internal.h');
  const core = file('libobs/obs.c');
  assert.match(internal, /obs_weak_source_t \*monitoring_duplicating_source;/);
  assert.match(core, /obs_source_get_weak_source\(src\)/);
  assert.match(core, /pthread_mutex_lock\(&audio->monitoring_mutex\)/);
  assert.match(core, /obs_weak_source_release\(previous\)/);
  assert.match(core, /obs_weak_source_release\(audio->monitoring_duplicating_source\)/);
  assert.doesNotMatch(core, /obs_queue_task\(OBS_TASK_AUDIO, set_monitoring_duplication_source/);
});

test('audio callback holds a strong monitoring source ref through mixing', () => {
  const audio = file('libobs/obs-audio.c');
  const source = file('libobs/obs-source.c');
  assert.match(audio, /obs_weak_source_get_source\(audio->monitoring_duplicating_source\)/);
  assert.match(audio, /should_silence_monitored_source\(source, duplicating_source\)/);
  assert.match(audio, /obs_source_release\(duplicating_source\)/);
  assert.match(source, /obs_weak_source_references_source\(audio->monitoring_duplicating_source, src\)/);
});
