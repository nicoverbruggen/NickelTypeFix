include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickeltypefix.so
override SOURCES  += src/config.c src/nickeltypefix.cc

# The vertical-text fix uses QString (KepubBookReader::pageStyleCss returns QString and
# writingDirectionFromString takes QString const&), so link Qt5Core. NickelHook.mk turns
# PKGCONF entries into the right -I/-l flags from the nickeltc sysroot.
# Fix 6 (letter-spacing on spaces) hooks QTextLine::glyphRuns and works with
# QGlyphRun/QRawFont/QTextLine, so it needs Qt5Gui too.
override PKGCONF  += Qt5Core Qt5Gui

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/doc:$(NTF_CONFIG_DIR)/doc res/uninstall:$(NTF_CONFIG_DIR)/uninstall

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickeltypefix.so
.PHONY: strip

ifeq ($(NTF_CONFIG_DIR),)
override NTF_CONFIG_DIR := /mnt/onboard/.adds/nickel-type-fix
endif

override CPPFLAGS += -DNTF_CONFIG_DIR='"$(NTF_CONFIG_DIR)"' -DNTF_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NTF_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
