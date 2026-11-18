#!/usr/bin/python3
# -*- coding: utf-8 -*-
# ******************************************************************************
# ZYNTHIAN PROJECT: Zynthian GUI
#
# Zynthian widget for the Slicer LV2 plugin
# (https://github.com/xjaner/zynthian-slicer)
#
# Shows the sample waveform with its slices: numbered slice markers (MIDI
# notes), the selected slice, the slices that are playing and the playhead.
# Tap a slice to select and audition it.
#
# Install as zyngui/widgets_user/zynthian_widget_slicer.py: Zynthian loads it
# for the LV2 plugin named "Slicer".
#
# The plugin reports its slices through a "live" slice file,
# /tmp/zynthian-slicer-<instance_id>.slices, rewritten whenever they change.
# The instance_id and live_version output ports (monitors) tell the widget
# which file to read and when.
#
# ******************************************************************************
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# For a full copy of the GNU General Public License see the LICENSE.txt file.
#
# ******************************************************************************

import os
import logging
from math import pow
from threading import Thread
from os.path import basename

from zyngine.zynthian_signal_manager import zynsigman
from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base
from zyngui import zynthian_widget_audio_file as audio_file

LIVE_DIR = os.environ.get("ZYNTHIAN_SLICER_LIVE_DIR") or "/tmp"
ACTIVE_PORTS = 6    # active_0 .. active_5
ACTIVE_BITS = 24    # Slices per active_N port
DEFAULT_BASE_NOTE = 36


# ------------------------------------------------------------------------------
# Helpers (no GUI, unit tested)
# ------------------------------------------------------------------------------

def live_path(instance_id):
    return os.path.join(LIVE_DIR, f"zynthian-slicer-{int(instance_id)}.slices")


def parse_live(text):
    """Parse a slice file. Returns a dict, or None if it isn't valid."""
    info = {"sample": "", "frames": 0, "manual": False, "selected": 0,
            "base_note": DEFAULT_BASE_NOTE, "export": "", "export_error": "", "slices": []}
    version = None
    for line in text.splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            if key == "version":
                version = int(value)
            elif key in ("sample", "export", "export_error"):
                info[key] = value
            elif key in ("frames", "selected", "base_note"):
                info[key] = int(value)
            elif key == "manual":
                info["manual"] = value.strip() == "1"
            elif key == "slice":
                parts = value.split()
                info["slices"].append((int(parts[0]), int(parts[1]), parts[2] if len(parts) > 2 else "gate"))
        except (ValueError, IndexError):
            continue
    return info if version == 1 else None


def active_slices(monitors):
    """Return the set of playing slice indexes from the active_N monitors."""
    active = set()
    for port in range(ACTIVE_PORTS):
        bits = int(monitors.get(f"active_{port}", 0) or 0)
        for bit in range(ACTIVE_BITS):
            if bits >> bit & 1:
                active.add(port * ACTIVE_BITS + bit)
    return active


def view_offset(frames, zoom, slices, selected):
    """First frame shown: centred on the selected slice when zoomed in."""
    if not frames or zoom <= 1:
        return 0
    view = max(1, frames // zoom)
    offset = 0
    if 0 <= selected < len(slices):
        start, end = slices[selected][:2]
        offset = (start + end) // 2 - view // 2
    return max(0, min(offset, frames - view))


def slice_at(slices, frame):
    """Index of the slice containing a frame, or None."""
    for index, sl in enumerate(slices):
        if sl[0] <= frame < sl[1]:
            return index
    return None


def boundary_markers(slices, selected, active, to_x, capacity):
    """Slice boundary marker positions and kinds, most important first.

    Kinds: "selected" (both edges of the selected slice), "active" (start of a
    playing slice) and "normal". Markers on the same pixel are drawn once and
    the list is cut to the canvas capacity.
    Returns: (markers [(x, kind)], slice start x positions for the numbers)
    """
    starts = [to_x(sl[0]) for sl in slices]
    selected_x, active_x, normal_x = [], [], []
    for index, sl in enumerate(slices):
        x = starts[index]
        if index == selected:
            selected_x += [x, to_x(sl[1])]
        elif index in active:
            active_x.append(x)
        else:
            normal_x.append(x)
    if slices and selected != len(slices) - 1:
        # The last end is the only boundary that isn't another slice's start
        end_x = to_x(slices[-1][1])
        (active_x if len(slices) - 1 in active else normal_x).append(end_x)
    markers = []
    for kind, xs in (("selected", selected_x), ("active", active_x), ("normal", normal_x)):
        for x in xs:
            if not any(abs(x - mx) < 0.5 for mx, _ in markers):
                markers.append((x, kind))
    return markers[:max(0, capacity)], starts


# ------------------------------------------------------------------------------
# Widget
# ------------------------------------------------------------------------------

class zynthian_widget_slicer(audio_file.zynthian_widget_audio_file):

    def __init__(self, parent):
        super().__init__(parent)
        self.live = None            # Parsed live slice file
        self.live_key = None        # (instance_id, live_version) of self.live
        self.export_count = None
        self.signature = None       # What the markers were last drawn for
        self.auditioning = False
        self.active_marker_color = audio_file.hexcolor_to_opengl(zynthian_gui_config.color_on)

    def set_processor(self, processor):
        zynthian_widget_base.zynthian_widget_base.set_processor(self, processor)
        self.eng_type = self.ENG_NONE
        self.clip_info = None
        self.zctrl = self.processor.controllers_dict.get("sample") if self.processor else None
        self.live = None
        self.live_key = None
        self.export_count = None
        self.signature = None

    # -------------------------------------------------------------------------
    # Plugin state
    # -------------------------------------------------------------------------

    def _ctrl(self, symbol, default=0):
        try:
            value = self.processor.controllers_dict[symbol].value
            return default if value is None else value
        except (KeyError, AttributeError):
            return default

    def _update_live(self):
        """Re-read the live slice file when the plugin rewrote it."""
        monitors = self.monitors or {}
        instance_id = int(monitors.get("instance_id", 0) or 0)
        if not instance_id:
            return
        key = (instance_id, monitors.get("live_version"))
        if key != self.live_key:
            try:
                with open(live_path(instance_id), encoding="utf-8") as fh:
                    live = parse_live(fh.read())
                if live is not None:
                    self.live = live
                    self.live_key = key
            except OSError:
                pass
        export_count = monitors.get("export_count")
        if export_count is not None and export_count != self.export_count:
            if self.export_count is not None and self.live_key == key and self.live:
                self._show_export_result()
            self.export_count = export_count

    def _show_export_result(self):
        if self.live["export_error"]:
            message = f"SFZ export failed: {self.live['export_error']}"
        else:
            message = f"SFZ exported: {basename(self.live['export'])}"
        zynsigman.send_queued(zynsigman.S_GUI, zynsigman.SS_GUI_SHOW_MESSAGE, message=message)

    def _slices(self):
        live = self.live
        if live and live["sample"] == self.fpath:
            return live["slices"]
        return []

    def _selected(self):
        return self.live["selected"] if self.live else 0

    # -------------------------------------------------------------------------
    # Touch
    # -------------------------------------------------------------------------

    def on_canvas_press(self, event):
        """Select the tapped slice and play it, as a held key."""
        slices = self._slices()
        if not slices or not self.frames or self.zoom <= 0:
            return
        frame = self.offset + (event.x / self.width) * self.frames / self.zoom
        index = slice_at(slices, frame)
        if index is None:
            return
        try:
            self.processor.controllers_dict["slice"].set_value(index)
            self.processor.controllers_dict["audition"].set_value(index + 1)
            self.auditioning = True
        except Exception as e:
            logging.warning(f"Can't audition slice: {e}")

    def on_canvas_release(self, event):
        """Release the auditioned slice, as a key release (stops gate and loop slices)."""
        if self.auditioning:
            self.auditioning = False
            try:
                self.processor.controllers_dict["audition"].set_value(0)
            except Exception as e:
                logging.warning(f"Can't release audition: {e}")

    # -------------------------------------------------------------------------
    # Drawing
    # -------------------------------------------------------------------------

    def refresh_gui(self):
        if not self.widget_canvas.prog or not self.zctrl:
            return
        if self.loading or self.refreshing:
            return
        self.refreshing = True
        try:
            self._refresh()
        except Exception as e:
            logging.exception(f"Slicer widget refresh failed: {e}")
        finally:
            self.refreshing = False

    def _refresh(self):
        fpath = self.zctrl.value
        if not fpath or fpath == "_":
            if self.fpath:
                self.fpath = ""
                self.fname = ""
                self.channels = 0
                self.frames = 0
                self.samplerate = None
                self.widget_canvas.init_channels(0)
                self.widget_canvas.clear()
            self.info_text_var.set("No sample")
            return
        if fpath != self.fpath:
            # Load the waveform in the background
            self.fpath = fpath
            self.fname = basename(fpath)
            self.signature = None
            Thread(target=self.load_file, name="load_waveform", daemon=True).start()
            return
        if not self.widget_canvas.init_channels(self.channels):
            return

        self._update_live()
        slices = self._slices()
        selected = self._selected()
        monitors = self.monitors or {}

        zoom = max(1, int(self._ctrl("zoom", 1)))
        offset = view_offset(self.frames, zoom, slices, selected)
        vzoom = pow(1.26, float(self._ctrl("gain", 0.0)))
        if zoom != self.zoom or offset != self.offset or vzoom != self.vzoom:
            self.zoom = zoom
            self.offset = offset
            self.vzoom = vzoom
            self.refresh_waveform = True

        if self.refresh_waveform:
            length = self.frames // self.zoom
            self.offset = max(0, min(self.offset, self.frames - length))
            if not self.draw_waveform(self.offset, length, self.vzoom):
                return
            self.refresh_waveform = False
            self.update_markers = True

        if not self.frames:
            return
        f = self.width / self.frames * self.zoom

        def to_x(frame):
            return f * (frame - self.offset)

        active = active_slices(monitors)
        playhead = monitors.get("playhead", -1.0)
        playhead_frame = int(playhead * self.frames) if playhead is not None and playhead >= 0 else None
        playing = slice_at(slices, playhead_frame) if playhead_frame is not None else None

        signature = (tuple(slices), selected, tuple(sorted(active)), playing, self.zoom, self.offset, self.width)
        if signature != self.signature:
            self.signature = signature
            self.update_markers = True

        canvas = self.widget_canvas
        if self.update_markers:
            canvas.set_crop_markers(to_x(0), to_x(self.frames))
            # Shade the slice the playhead is in
            if playing is not None:
                canvas.set_loop_markers(to_x(slices[playing][0]), to_x(slices[playing][1]))
            else:
                canvas.reset_loop_markers()
            capacity = canvas.layout.markers_count // 2 if canvas.layout else 0
            markers, starts = boundary_markers(slices, selected, active, to_x, capacity)
            colors = {"selected": self.bmarker_color1, "active": self.active_marker_color,
                      "normal": self.bmarker_color2}
            canvas.set_beat_markers([x for x, _ in markers], [colors[kind] for _, kind in markers])
            # Number each slice with its MIDI note
            base_note = self.live["base_note"] if self.live else DEFAULT_BASE_NOTE
            canvas.set_marker_numbers(starts, base_note)

        if playhead_frame is not None:
            canvas.set_cursor_pos(to_x(playhead_frame))
        else:
            canvas.set_cursor_pos(-100)

        info = self.fname
        if slices:
            mode = slices[selected][2] if selected < len(slices) else ""
            info = f"{self.fname} [{selected + 1}/{len(slices)} {mode}]"
        elif self.samplerate:
            info = f"{self.fname} [{self.format_time(self.frames / self.samplerate)}]"
        n = (self.width // max(1, self.font_info.measure("x"))) - 2
        if len(info) > n > 3:
            info = info[:n - 3] + "..."
        self.info_text_var.set(info)

        canvas.update()
        self.update_markers = False
        # On first update => repeat redraw to ensure widget is not blank
        if self.first_update:
            self.refresh_waveform = True
            self.first_update = False

# ******************************************************************************
