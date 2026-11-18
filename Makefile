# Zynthian Slicer LV2 plugin
#
#   make                 Build slicer.lv2/slicer.so
#   make test            Build and run the tests (needs Python 3 with numpy + soundfile)
#   make install         Copy the bundle to $(LV2DIR) and the widget to $(WIDGETDIR)
#
# Dependencies: lv2, libsndfile, libsamplerate (development packages).
# Without pkg-config, pass DEPS_CFLAGS / DEPS_LIBS by hand.

CC ?= cc
PKG_CONFIG ?= pkg-config
OPT ?= -O2
# Pass WERROR=-Werror to treat warnings as errors (used for development)
WERROR ?=

PKGS = lv2 sndfile samplerate
DEPS_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
DEPS_LIBS ?= $(shell $(PKG_CONFIG) --libs sndfile samplerate 2>/dev/null || echo -lsndfile -lsamplerate)

CFLAGS += $(OPT) -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra $(WERROR) -Wno-unused-parameter $(DEPS_CFLAGS)
LDLIBS += $(DEPS_LIBS) -lm

BUNDLE = slicer.lv2
SOURCES = src/plugin.c src/dsp.c src/model.c src/sample.c src/export.c
HEADERS = $(wildcard src/*.h)

ZYNTHIAN_UI_DIR ?= /zynthian/zynthian-ui
LV2DIR ?= $(HOME)/.lv2
WIDGETDIR ?= $(ZYNTHIAN_UI_DIR)/zyngui/widgets_user

ifeq ($(shell uname -s),Darwin)
SHARED = -dynamiclib -undefined dynamic_lookup
else
SHARED = -shared -Wl,--no-undefined
endif

all: $(BUNDLE)/slicer.so

$(BUNDLE)/slicer.so: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

build/test_model: tests/test_model.c src/model.c src/sample.c src/export.c src/dsp.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_model.c src/model.c src/sample.c src/export.c src/dsp.c $(LDFLAGS) $(LDLIBS)

build/test_host: tests/test_host.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_host.c $(LDFLAGS) -ldl -lpthread -lm

test: all build/test_model build/test_host
	tests/run_tests.sh

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m 644 $(BUNDLE)/manifest.ttl $(BUNDLE)/slicer.ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)/
	install -m 755 $(BUNDLE)/slicer.so $(DESTDIR)$(LV2DIR)/$(BUNDLE)/
	install -d $(DESTDIR)$(WIDGETDIR)
	install -m 644 widget/zynthian_widget_slicer.py $(DESTDIR)$(WIDGETDIR)/

clean:
	rm -rf build $(BUNDLE)/slicer.so

.PHONY: all test install clean
