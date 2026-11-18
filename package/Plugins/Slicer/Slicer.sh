#!/bin/bash
# Zynthian package management script for the Slicer LV2 plugin.
#
#   Slicer.sh install | uninstall | installed | git_version | git_repo
#
# install: clones the repository, builds the plugin on the device, installs
# the LV2 bundle into the Zynthian plugins folder, registers it in the engine
# list (category Sampler) and installs the UI widget. Restart the Zynthian UI
# afterwards to see it in the engine list.
#
# Your slices (zynthian-my-data/slicer) and SFZ exports are kept on uninstall.

ZYNTHIAN_DIR="${ZYNTHIAN_DIR:-/zynthian}"
ZYNTHIAN_SW_DIR="${ZYNTHIAN_SW_DIR:-$ZYNTHIAN_DIR/zynthian-sw}"
ZYNTHIAN_UI_DIR="${ZYNTHIAN_UI_DIR:-$ZYNTHIAN_DIR/zynthian-ui}"
ZYNTHIAN_PLUGINS_DIR="${ZYNTHIAN_PLUGINS_DIR:-$ZYNTHIAN_DIR/zynthian-plugins}"

DOWNLOAD_URL="${SLICER_REPO_URL:-https://github.com/xjaner/zynthian-slicer.git}"
SRC_DIR="$ZYNTHIAN_SW_DIR/zynthian-slicer"
BUNDLE_DIR="$ZYNTHIAN_PLUGINS_DIR/lv2/slicer.lv2"
WIDGET_DIR="$ZYNTHIAN_UI_DIR/zyngui/widgets_user"
WIDGET="$WIDGET_DIR/zynthian_widget_slicer.py"

if [ -x "$ZYNTHIAN_DIR/venv/bin/python3" ]; then
    PYTHON="$ZYNTHIAN_DIR/venv/bin/python3"
else
    PYTHON="python3"
fi

do_install() {
    set -e
    rm -rf "$SRC_DIR"
    git clone --depth 1 "$DOWNLOAD_URL" "$SRC_DIR"
    cd "$SRC_DIR"
    make -j3
    # Stage the bundle: Zynthian copies it into its plugins folder and lists it
    STAGE="$SRC_DIR/build/stage"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/slicer.lv2"
    cp slicer.lv2/manifest.ttl slicer.lv2/slicer.ttl slicer.lv2/slicer.so "$STAGE/slicer.lv2/"
    "$PYTHON" package/zynthian_register.py install "$STAGE"
    mkdir -p "$WIDGET_DIR"
    cp widget/zynthian_widget_slicer.py "$WIDGET"
    set +e
    echo "installed"
}

do_uninstall() {
    if [[ $(is_installed) == "installed" ]]; then
        if [ -f "$SRC_DIR/package/zynthian_register.py" ]; then
            "$PYTHON" "$SRC_DIR/package/zynthian_register.py" uninstall
        fi
        rm -rf "$BUNDLE_DIR"
        rm -f "$WIDGET"
        rm -rf "$SRC_DIR"
        echo "uninstalled"
    else
        echo "not installed"
    fi
}

is_installed() {
    if [[ -f "$BUNDLE_DIR/slicer.so" && -f "$WIDGET" ]]; then
        echo "installed"
    else
        echo "not installed"
    fi
}

git_repo() {
    echo $DOWNLOAD_URL
}

git_version() {
    git ls-remote "$DOWNLOAD_URL" HEAD | awk '{ print $1 }'
}

if [[ "$1" == "install" ]]; then
    do_install
elif [[ "$1" == "uninstall" ]]; then
    do_uninstall
elif [[ "$1" == "installed" ]]; then
    echo $(is_installed)
elif [[ "$1" == "git_version" ]]; then
    echo $(git_version)
elif [[ "$1" == "git_repo" ]]; then
    echo $(git_repo)
fi
