#!/usr/bin/env python3
"""Tests for the Slicer widget without Zynthian.

The Zynthian GUI modules are replaced by small fakes, so this checks the
widget's own logic: reading the live slice file, markers, selection by touch,
audition and export messages.
"""

import os
import sys
import types
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LIVE_DIR = tempfile.mkdtemp(prefix="slicer-widget-")
os.environ["ZYNTHIAN_SLICER_LIVE_DIR"] = LIVE_DIR


# ------------------------------------------------------------------------------
# Fakes for the Zynthian modules the widget imports
# ------------------------------------------------------------------------------

messages = []


def install_fakes():
    zyngine = types.ModuleType("zyngine")
    sigman_mod = types.ModuleType("zyngine.zynthian_signal_manager")

    class SigMan:
        S_GUI = 1
        SS_GUI_SHOW_MESSAGE = 2

        def send_queued(self, signal, subsignal, **kwargs):
            messages.append(kwargs.get("message"))

    sigman_mod.zynsigman = SigMan()
    zyngine.zynthian_signal_manager = sigman_mod

    zyngui = types.ModuleType("zyngui")
    config = types.ModuleType("zyngui.zynthian_gui_config")
    config.color_on = "#ff0000"

    base = types.ModuleType("zyngui.zynthian_widget_base")

    class zynthian_widget_base:
        def __init__(self, parent):
            self.processor = None
            self.monitors = None
            self.width = 400
            self.height = 100

        def set_processor(self, processor):
            self.processor = processor

    base.zynthian_widget_base = zynthian_widget_base

    audio_file = types.ModuleType("zyngui.zynthian_widget_audio_file")

    def hexcolor_to_opengl(hex_str):
        return tuple(int(hex_str[i:i + 2], 16) / 255 for i in (1, 3, 5))

    class FakeCanvas:
        def __init__(self):
            self.prog = True
            self.layout = types.SimpleNamespace(markers_count=2 * (400 // 16))
            self.calls = {}

        def __getattr__(self, name):
            def record(*args):
                self.calls[name] = args
                return True
            return record

    class zynthian_widget_audio_file(zynthian_widget_base):
        ENG_NONE = 0

        def __init__(self, parent):
            super().__init__(parent)
            self.widget_canvas = FakeCanvas()
            self.fpath = None
            self.fname = ""
            self.frames = 0
            self.channels = 0
            self.samplerate = None
            self.loading = False
            self.refreshing = False
            self.refresh_waveform = False
            self.update_markers = False
            self.first_update = True
            self.offset = 0
            self.zoom = 1
            self.vzoom = 1
            self.bmarker_color1 = (1, 1, 1)
            self.bmarker_color2 = (0.5, 0.5, 0.5)
            self.font_info = types.SimpleNamespace(measure=lambda s: 8)
            self.info_text_var = types.SimpleNamespace(value="", set=lambda v: setattr(self, "info", v))
            self.drawn = None

        def load_file(self):
            # Pretend to read the file (the real one uses soundfile)
            self.frames = 96000
            self.channels = 2
            self.samplerate = 48000
            self.refresh_waveform = True

        def draw_waveform(self, start, length, vzoom=1.0):
            self.drawn = (start, length, vzoom)
            return True

        @staticmethod
        def format_time(time):
            return f"{time:.3f}"

    audio_file.hexcolor_to_opengl = hexcolor_to_opengl
    audio_file.zynthian_widget_audio_file = zynthian_widget_audio_file

    zyngui.zynthian_gui_config = config
    zyngui.zynthian_widget_base = base
    zyngui.zynthian_widget_audio_file = audio_file
    sys.modules.update({
        "zyngine": zyngine,
        "zyngine.zynthian_signal_manager": sigman_mod,
        "zyngui": zyngui,
        "zyngui.zynthian_gui_config": config,
        "zyngui.zynthian_widget_base": base,
        "zyngui.zynthian_widget_audio_file": audio_file,
    })


install_fakes()
sys.path.insert(0, os.path.join(HERE, "..", "widget"))
import zynthian_widget_slicer as w  # noqa: E402


class Ctrl:
    def __init__(self, value):
        self.value = value
        self.sent = []

    def set_value(self, value, send=True):
        self.value = value
        self.sent.append(value)


LIVE = """# Zynthian Slicer slice map
version=1
sample=/data/loop.wav
frames=96000
manual=1
selected=1
base_note=36
slice=0 24000 gate
slice=24000 48000 loop
slice=48000 72000 one-shot
slice=72000 96000 gate
"""


class HelperTests(unittest.TestCase):

    def test_parse_live(self):
        info = w.parse_live(LIVE)
        self.assertEqual(info["sample"], "/data/loop.wav")
        self.assertEqual(info["frames"], 96000)
        self.assertTrue(info["manual"])
        self.assertEqual(info["selected"], 1)
        self.assertEqual(info["slices"][1], (24000, 48000, "loop"))
        self.assertIsNone(w.parse_live("version=2\n"))
        self.assertEqual(w.parse_live("version=1\nsample=/a=b.wav\nslice=bad\n")["sample"], "/a=b.wav")

    def test_active_slices(self):
        self.assertEqual(w.active_slices({"active_0": 5.0, "active_1": 1.0}), {0, 2, 24})
        self.assertEqual(w.active_slices({"active_5": float(1 << 6)}), {126})
        self.assertEqual(w.active_slices({}), set())

    def test_view_offset(self):
        slices = w.parse_live(LIVE)["slices"]
        self.assertEqual(w.view_offset(96000, 1, slices, 1), 0)
        # x4 => 24000 frames shown, centred on slice 2 (48000..72000)
        self.assertEqual(w.view_offset(96000, 4, slices, 2), 48000)
        # Clamped at the end
        self.assertEqual(w.view_offset(96000, 2, slices, 3), 48000)

    def test_slice_at(self):
        slices = w.parse_live(LIVE)["slices"]
        self.assertEqual(w.slice_at(slices, 0), 0)
        self.assertEqual(w.slice_at(slices, 47999.5), 1)
        self.assertIsNone(w.slice_at(slices, 96000))

    def test_boundary_markers(self):
        slices = w.parse_live(LIVE)["slices"]
        markers, starts = w.boundary_markers(slices, 1, {2}, lambda f: f / 240, 100)
        self.assertEqual(starts, [0, 100, 200, 300])
        self.assertEqual(markers, [(100, "selected"), (200, "selected"), (0, "normal"),
                                   (300, "normal"), (400, "normal")])
        # The selected end is also slice 2's start: drawn once, as selected
        markers, _ = w.boundary_markers(slices, 0, {1}, lambda f: f / 240, 100)
        self.assertEqual(markers[:3], [(0, "selected"), (100, "selected"), (200, "normal")])
        self.assertEqual(len(w.boundary_markers(slices, 1, set(), lambda f: f / 240, 2)[0]), 2)


class WidgetTests(unittest.TestCase):

    def setUp(self):
        messages.clear()
        self.ctrls = {"sample": Ctrl("/data/loop.wav"), "slice": Ctrl(1), "audition": Ctrl(0),
                      "zoom": Ctrl(1), "gain": Ctrl(0.0)}
        self.processor = types.SimpleNamespace(controllers_dict=self.ctrls)
        self.widget = w.zynthian_widget_slicer(None)
        self.widget.set_processor(self.processor)
        with open(w.live_path(42), "w") as fh:
            fh.write(LIVE)
        self.widget.monitors = {"instance_id": 42.0, "live_version": 1.0, "export_count": 0.0,
                                "playhead": -1.0}

    def refresh(self):
        self.widget.refresh_gui()

    def test_refresh_draws_slices(self):
        self.refresh()                      # Starts loading the waveform
        self.widget.load_file()             # (normally a thread)
        self.refresh()
        canvas = self.widget.widget_canvas
        self.assertEqual(self.widget.drawn, (0, 96000, 1.0))
        xs, colors = canvas.calls["set_beat_markers"]
        self.assertEqual(xs[:2], [100.0, 200.0])          # Selected slice edges first
        self.assertEqual(canvas.calls["set_marker_numbers"], ([0.0, 100.0, 200.0, 300.0], 36))
        self.assertEqual(canvas.calls["set_cursor_pos"], (-100,))
        self.assertEqual(self.widget.info, "loop.wav [2/4 loop]")

        # A playing slice: cursor and shading
        self.widget.monitors.update({"playhead": 0.6, "active_0": 4.0})
        self.refresh()
        self.assertAlmostEqual(canvas.calls["set_cursor_pos"][0], 240.0)
        self.assertEqual(canvas.calls["set_loop_markers"], (200.0, 300.0))

        # Zoom x4 centres on the selected slice
        self.ctrls["zoom"].value = 4
        self.refresh()
        self.assertEqual(self.widget.offset, 24000)
        self.assertEqual(self.widget.drawn, (24000, 24000, 1.0))

    def test_new_live_version_is_read(self):
        self.refresh()
        self.widget.load_file()
        self.refresh()
        with open(w.live_path(42), "w") as fh:
            fh.write(LIVE.replace("selected=1", "selected=3"))
        self.refresh()
        self.assertEqual(self.widget._selected(), 1)  # Same version: not re-read
        self.widget.monitors["live_version"] = 2.0
        self.refresh()
        self.assertEqual(self.widget._selected(), 3)

    def test_slices_of_another_sample_are_hidden(self):
        self.ctrls["sample"].value = "/data/other.wav"
        self.refresh()
        self.widget.load_file()
        self.refresh()
        self.assertEqual(self.widget._slices(), [])

    def test_touch_selects_and_auditions(self):
        self.refresh()
        self.widget.load_file()
        self.refresh()
        self.widget.on_canvas_press(types.SimpleNamespace(x=250))
        self.assertEqual(self.ctrls["slice"].sent, [2])
        self.assertEqual(self.ctrls["audition"].sent, [3])
        self.widget.on_canvas_release(types.SimpleNamespace(x=250))
        self.assertEqual(self.ctrls["audition"].sent, [3, 0])

    def test_export_message(self):
        self.refresh()
        self.widget.load_file()
        self.refresh()
        self.assertEqual(messages, [])  # The first count seen isn't an export
        with open(w.live_path(42), "w") as fh:
            fh.write(LIVE + "export=/sfz/loop/loop.sfz\n")
        self.widget.monitors.update({"live_version": 2.0, "export_count": 1.0})
        self.refresh()
        self.assertEqual(messages, ["SFZ exported: loop.sfz"])
        with open(w.live_path(42), "w") as fh:
            fh.write(LIVE + "export_error=no slices\n")
        self.widget.monitors.update({"live_version": 3.0, "export_count": 2.0})
        self.refresh()
        self.assertEqual(messages[-1], "SFZ export failed: no slices")

    def test_no_sample(self):
        self.ctrls["sample"].value = None
        self.refresh()
        self.assertEqual(self.widget.info, "No sample")


if __name__ == "__main__":
    unittest.main()
