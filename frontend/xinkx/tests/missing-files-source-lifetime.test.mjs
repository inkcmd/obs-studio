import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = path => readFileSync(new URL(`../../../${path}`, import.meta.url), 'utf8');
const missing = read('libobs/obs-missing-files.c');
const source = read('libobs/obs-source.c');
const image = read('plugins/image-source/image-source.c');
const slideshow = read('plugins/image-source/obs-slideshow.c');
const slideshowMk2 = read('plugins/image-source/obs-slideshow-mk2.c');
const vlc = read('plugins/vlc-video/vlc-video-source.c');

// A missing-file entry may outlive the collection whose sources it describes.
// Each callback must temporarily acquire a live source and release it after use.
assert.match(missing, /file->weak_src = obs_source_get_weak_source\(\(obs_source_t \*\)src\)/);
assert.match(missing, /obs_weak_source_get_source\(file->weak_src\)/);
assert.match(missing, /if \(source\) \{\s*obs_source_replace_missing_file\(file->callback, source, new_path, file->data\);\s*obs_source_release\(source\);/);
assert.match(missing, /obs_weak_source_release\(file->weak_src\)/);
assert.doesNotMatch(missing, /file->src\s*=\s*src/);
assert.match(source, /cb\(source->context.data, new_path, data\)/);

// Slideshow and VLC pass their original path as callback user data, but OBS
// frees the original settings before the user presses the Save button.
assert.match(missing, /file->data = data == \(const void \*\)path \? file->file_path : data/);
for (const plugin of [slideshow, slideshowMk2, vlc]) {
  assert.match(plugin, /OBS_MISSING_FILE_SOURCE, source, \(void \*\)path\)/);
}
assert.match(image, /OBS_MISSING_FILE_SOURCE, s->source, NULL/);
console.log('INK missing files: weak source lifetime and owned path callback data OK');
