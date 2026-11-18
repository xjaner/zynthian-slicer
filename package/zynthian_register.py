#!/usr/bin/env python3
"""Register or remove the Slicer plugin in Zynthian's engine list.

Uses zynthian-ui's own zynthian_lv2 module (the code webconf uses to install
LV2 plugins), loaded straight from its file so the rest of zynthian-ui isn't
imported.

    zynthian_register.py install BUNDLE_PARENT_DIR
    zynthian_register.py uninstall
"""

import os
import sys
import importlib.util

ENGINE_KEY = "JV/Slicer"
CATEGORY = "Sampler"


def load_zynthian_lv2():
    ui_dir = os.environ.get("ZYNTHIAN_UI_DIR", "/zynthian/zynthian-ui")
    path = os.path.join(ui_dir, "zyngine", "zynthian_lv2.py")
    spec = importlib.util.spec_from_file_location("zynthian_lv2", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # Loads the engine list
    return module


def install(bundle_parent):
    lv2 = load_zynthian_lv2()
    error = lv2.add_engine(bundle_parent, enable=True, force=True)
    if error:
        print(f"Can't register the Slicer plugin: {error}", file=sys.stderr)
        return 1
    if ENGINE_KEY not in lv2.engines:
        print(f"Slicer plugin not found after install ({ENGINE_KEY})", file=sys.stderr)
        return 1
    # LV2 instruments default to "Synth"; list it with the samplers
    lv2.engines[ENGINE_KEY]["CAT"] = CATEGORY
    lv2.engines[ENGINE_KEY]["ENABLED"] = True
    lv2.save_engines()
    return 0


def uninstall():
    lv2 = load_zynthian_lv2()
    if ENGINE_KEY in lv2.engines:
        lv2.remove_engine(ENGINE_KEY)  # Deletes the bundle and the cache entry
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "install":
        sys.exit(install(sys.argv[2]))
    if len(sys.argv) == 2 and sys.argv[1] == "uninstall":
        sys.exit(uninstall())
    print(__doc__, file=sys.stderr)
    sys.exit(2)
