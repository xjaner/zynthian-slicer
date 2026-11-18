# Zynthian Slicer

An LV2 instrument for [Zynthian](https://zynthian.org) that splits an audio sample into slices played by
consecutive MIDI notes, with a Zynthian UI widget that shows the waveform and the slices.

- Slice by **beats** (beats × slices per beat), an even **grid**, or detected **transients**
- Edit the selected slice's **start**, **end** and **play mode** (one-shot, gate, loop); slices stay contiguous
- **Tune** pitch and speed together (±24 semitones), master **gain**, **polyphony** up to 16 voices
- Tap a slice on the waveform to select and audition it
- Hand edits are saved and restored with Zynthian snapshots
- **Export SFZ**: writes the slices as an instrument in the sfizz banks, with a tune control

It is a standalone package: zynthian-ui is not modified.

## Install

### As a Zynthian package

`package/Plugins/Slicer` is a [zynthian-packages](https://github.com/zynthian/zynthian-packages) entry.
Its script builds the plugin on the device:

```sh
bash package/Plugins/Slicer/Slicer.sh install
```

Then restart the Zynthian UI. The plugin appears in **MIDI Synth › Sampler** as *Slicer*.

`Slicer.sh uninstall` removes the plugin and the widget (your slice maps and SFZ exports are kept).

### By hand

Build dependencies: `lv2-dev`, `libsndfile1-dev`, `libsamplerate0-dev` (already on Zynthian).

```sh
make
make install LV2DIR=/zynthian/zynthian-plugins/lv2
```

`make install` also copies the widget to `$ZYNTHIAN_UI_DIR/zyngui/widgets_user/zynthian_widget_slicer.py`,
where Zynthian looks for widgets of LV2 plugins (by plugin name). Then search for new plugins in
webconf (or reinstall the engine list) and restart the UI.

## Controls

| Page   | Controls |
|--------|----------|
| Sample | sample (file), gain (dB), tune, base note |
| Slice  | slice mode (beats / grid / transient / manual), grid slices, beats, slices / beat |
| Detect | threshold, min gap (ms), detect |
| Edit   | slice, start ‰, end ‰, play mode |
| View   | zoom, polyphony |
| Export | export SFZ |

Start and end are in ‰ (tenths of a percent of the sample, 0–1000): a knob step is 1 %, or 0.1 % with
fine adjustment. Slice *n* plays on MIDI note *base note + n*. Note 0 (and *All notes off*) stops every voice.

Changing the slicing settings re-slices the sample until you edit a slice by hand. After that, the
edits are kept: press **detect** to slice again, or use slice mode **manual** to never re-slice.

## How it works

The plugin owns all the logic, so the control pages work whether or not the widget is shown.

- **Audio thread**: plays the voices and turns control changes into jobs for the LV2 worker. It never
  allocates, frees or touches files.
- **Worker thread**: loads and resamples the sample (libsndfile, libsamplerate), slices it, applies
  edits, reads and writes slice maps and exports SFZ. Each job returns a new slice table that the audio
  thread swaps in. Old buffers go back to the worker to be freed.
- **Properties** (`patch:Set`): sample, slice, start, end, play mode, detect, export. The plugin reports
  the real values back, so Zynthian's knobs follow the plugin (selecting a slice updates start/end).
  Detect and export are reset to 0 after each press, because Zynthian never sends triggers back to 0.
  Two Zynthian quirks shape the plugin description (checked by `tests/test_ttl.py`): control input
  ports come first, because Zynthian numbers properties right after them and would overwrite later
  ports; and numeric properties are integers, because Zynthian drops float properties.
- **Live slice file** `/tmp/zynthian-slicer-<id>.slices`: the current slices, rewritten after each change.
  The widget finds it through the `instance_id` monitor and re-reads it when `live_version` changes.
  Playing slices and the playhead come from the `active_*` and `playhead` monitors.
- **Saved slice maps** `zynthian-my-data/slicer/<hash>.slices`: hand-edited slices, named by content and
  saved one second after the edits settle. Their path is the hidden `slice_map` property, so snapshots
  store it and restore the edits. For half a second after a sample or slice map arrives, slice edits
  are ignored: that is a snapshot being restored, in no guaranteed order.

Slice maps are small text files:

```
# Zynthian Slicer slice map
version=1
sample=/zynthian/zynthian-my-data/files/Audio/loop.wav
frames=88200
manual=1
slice=0 11025 gate
slice=11025 35280 loop
...
```

## Tests

```sh
make test
```

1. Model unit tests (C)
2. Transient detection and SFZ output compared with the Python slicer engine this was ported from
   (`tests/reference`): the results must be identical
3. The plugin driven by a jalv-like test host (threaded worker, 4 KiB rings): loading, playing, slicing,
   editing, saving and restoring slice maps (including snapshot values in the worst order), audition,
   tune, base note, export
4. The plugin description, against how Zynthian reads it
5. The widget's logic, with fake Zynthian modules

They need Python 3 with numpy, soundfile and rdflib, which Zynthian has.

## Layout

```
src/            Plugin (C): plugin.c (LV2 + threads), dsp.c (voices), model.c (slicing),
                sample.c (loading), export.c (SFZ)
slicer.lv2/     LV2 bundle description (manifest.ttl, slicer.ttl)
widget/         Zynthian UI widget
package/        zynthian-packages entry and engine registration helper
tests/          Tests
```

## License

GPL-2.0-or-later, like zynthian-ui. Sample loading and the voice engine are based on zynthian-ui's
zynclippy library by Brian Walton and Fernando Moyano.
