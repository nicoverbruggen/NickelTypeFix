include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickeltypefix.so
override SOURCES  += src/config.c src/nickeltypefix.cc src/shape_cache.cc

# The vertical-text fix uses QString (KepubBookReader::pageStyleCss returns QString and
# writingDirectionFromString takes QString const&), so link Qt5Core. NickelHook.mk turns
# PKGCONF entries into the right -I/-l flags from the nickeltc sysroot.
# Fixes 3 and 5 patch QtGui code, and Fix 9 assigns QGlyphRun paints to pages,
# so link Qt5Gui too.
override PKGCONF  += Qt5Core Qt5Gui

# Fix 12 shapes through QTextEngine, which lives in Qt's private headers. pkg-config reports an
# unsysrooted include dir, so find the versioned private tree in the toolchain sysroot instead and
# derive the QtCore one beside it. The private headers are Qt's code rather than ours and are not
# warning-clean, so they come in as system includes to keep -Werror pointed at this repo. An empty
# result leaves the flags off and the compile fails with a missing header, which is the right
# failure: the fix cannot be built without them.
NTF_SYSROOT    := $(shell $(CROSS_COMPILE)gcc -print-sysroot)
NTF_QT_PRIVDIR := $(firstword $(wildcard $(NTF_SYSROOT)/usr/include/QtGui/*/QtGui/private))
NTF_QT_GUIPRIV := $(patsubst %/QtGui/private,%,$(NTF_QT_PRIVDIR))
NTF_QT_COREPRIV := $(subst /include/QtGui/,/include/QtCore/,$(NTF_QT_GUIPRIV))
ifneq ($(NTF_QT_PRIVDIR),)
override CXXFLAGS += -isystem $(NTF_QT_GUIPRIV)  -isystem $(NTF_QT_GUIPRIV)/QtGui \
                     -isystem $(NTF_QT_COREPRIV) -isystem $(NTF_QT_COREPRIV)/QtCore
endif

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/doc:$(NTF_CONFIG_DIR)/doc res/uninstall:$(NTF_CONFIG_DIR)/uninstall

NTF_DEV_BUILD ?= 0
ifneq ($(NTF_DEV_BUILD),0)
ifneq ($(NTF_DEV_BUILD),1)
$(error NTF_DEV_BUILD must be 0 or 1)
endif
endif
override CPPFLAGS += -DNTF_DEV_BUILD=$(NTF_DEV_BUILD)

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickeltypefix.so
.PHONY: strip

ifeq ($(NTF_CONFIG_DIR),)
override NTF_CONFIG_DIR := /mnt/onboard/.adds/nickel-type-fix
endif

override CPPFLAGS += -DNTF_CONFIG_DIR='"$(NTF_CONFIG_DIR)"' -DNTF_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NTF_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
