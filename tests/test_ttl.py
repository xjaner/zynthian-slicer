#!/usr/bin/env python3
"""Check slicer.ttl against how Zynthian reads LV2 plugins, and against the C code.

Zynthian quirks (zyngine/zynthian_lv2.py, zynthian_engine_jalv.py) this guards:
- Properties are numbered from the quantity of control input ports + 1, in the
  same dictionary as the ports (keyed by port index): a control input port
  with a higher index is overwritten by a property and disappears.
- A Float property without scale points fails to build (KeyError 'drawbar')
  and disappears.

Needs rdflib (present on Zynthian).
"""

import os
import re
import sys
import unittest

from rdflib import Graph, Namespace, URIRef, RDF

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")

LV2 = Namespace("http://lv2plug.in/ns/lv2core#")
ATOM = Namespace("http://lv2plug.in/ns/ext/atom#")
PATCH = Namespace("http://lv2plug.in/ns/ext/patch#")
PG = Namespace("http://lv2plug.in/ns/ext/port-groups#")
PPROPS = Namespace("http://lv2plug.in/ns/ext/port-props#")
RDFS = Namespace("http://www.w3.org/2000/01/rdf-schema#")
PLUGIN = URIRef("https://github.com/xjaner/zynthian-slicer")
INDEX = LV2["index"]  # Namespace is a str: .index would be str.index


def load():
    g = Graph()
    for name in ("manifest.ttl", "slicer.ttl"):
        g.parse(os.path.join(ROOT, "slicer.lv2", name), format="turtle")
    return g


def c_port_symbols(source):
    """Port symbols in the order of a C enum starting at P_GAIN."""
    start = source.index("P_GAIN")
    enum = source[start:source.index("}", start)]
    enum = re.sub(r"//[^\n]*", "", enum)
    names = []
    for item in enum.split(","):
        m = re.match(r"\s*P_([A-Z0-9_]+)", item)
        if m and m.group(1) not in ("ACTIVE_LAST", "COUNT"):
            names.append(m.group(1).lower())
    symbols = []
    for n in names:
        symbols += [f"active_{i}" for i in range(6)] if n == "active_0" else [n]
    return symbols


class TtlTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.g = load()
        g = cls.g
        cls.ports = {int(g.value(p, INDEX)): p for p in g.objects(PLUGIN, LV2.port)}
        cls.properties = list(g.objects(PLUGIN, PATCH.writable))

    def symbol(self, node):
        return str(self.g.value(node, LV2.symbol))

    def test_ports_contiguous(self):
        self.assertEqual(sorted(self.ports), list(range(len(self.ports))))

    def test_zynthian_numbering_keeps_every_control(self):
        g = self.g
        # Like zynthian_lv2.get_plugin_ports()
        info = {}
        for i, port in sorted(self.ports.items()):
            if (port, RDF.type, LV2.InputPort) in g and (port, RDF.type, LV2.ControlPort) in g:
                info[i] = self.symbol(port)
        i = len(info)
        for prop in self.properties:
            i += 1
            info[i] = self.symbol(prop)
        expected = {self.symbol(p) for p in self.properties}
        expected |= {self.symbol(p) for p in self.ports.values()
                     if (p, RDF.type, LV2.InputPort) in g and (p, RDF.type, LV2.ControlPort) in g}
        self.assertEqual(set(info.values()), expected)

    def test_properties_zynthian_can_build(self):
        g = self.g
        for prop in self.properties:
            rng = g.value(prop, RDFS.range)
            scale_points = list(g.objects(prop, LV2.scalePoint))
            if rng in (ATOM.Path, ATOM.Int, ATOM.Bool) or len(scale_points) > 1:
                continue
            self.fail(f"Property {self.symbol(prop)} ({rng}) would be dropped by Zynthian")

    def test_ports_match_c_enum(self):
        with open(os.path.join(ROOT, "src", "plugin.c")) as fh:
            plugin_c = fh.read()
        with open(os.path.join(HERE, "test_host.c")) as fh:
            host_c = fh.read()
        ttl = [self.symbol(self.ports[i]) for i in sorted(self.ports)]
        self.assertEqual(c_port_symbols(plugin_c), ttl)
        self.assertEqual(c_port_symbols(host_c), ttl)

    def test_pages(self):
        g = self.g
        pages = {}
        for node in list(self.ports.values()) + self.properties:
            group = g.value(node, PG.group)
            if group is None:
                continue
            gdp = int(g.value(group, LV2.displayPriority))
            dp = int(g.value(node, LV2.displayPriority) or 0) + 1000000 * gdp
            pages.setdefault((gdp, str(g.value(group, LV2.name))), []).append((dp, self.symbol(node)))
        layout = [(name, [s for _, s in sorted(ctrls, reverse=True)])
                  for (_, name), ctrls in sorted(pages.items(), reverse=True)]
        self.assertEqual(layout, [
            ("Sample", ["sample", "gain", "tune", "base_note"]),
            ("Slice", ["slice_mode", "slice_count", "slice_beats", "slice_subdivision"]),
            ("Detect", ["threshold", "min_gap", "detect"]),
            ("Edit", ["slice", "slice_start", "slice_end", "play_mode"]),
            ("View", ["zoom", "polyphony"]),
            ("Export", ["export_sfz"]),
        ])


if __name__ == "__main__":
    sys.exit(unittest.main())
