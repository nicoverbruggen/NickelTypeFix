#!/bin/sh
# Inside the test container. All outputs stay in its temporary filesystem.
set -eu
ulimit -c 0

repo="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
work="$(mktemp -d /tmp/nickeltypefix-rendering.XXXXXX)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
tc=/tc/arm-nickel-linux-gnueabihf
qt="$tc/arm-nickel-linux-gnueabihf/sysroot"
cxx="$tc/bin/arm-nickel-linux-gnueabihf-g++"
cc="$tc/bin/arm-nickel-linux-gnueabihf-gcc"
root=/opt/rendering/runtime
fonts=/opt/rendering/fonts
qemu=/opt/rendering/qemu/usr/bin/qemu-arm-static

"$cc" -std=c99 -Wall -Wextra -Werror -shared -fPIC "$repo/test/rendering/auxv.c" -o "$work/auxv.so"
"$qt/usr/bin/moc" "$repo/test/font-dropdown/font_dropdown_test.cc" -o "$work/font_dropdown_test.moc"

compile_qt() {
    "$cxx" -std=gnu++11 -mthumb -Wall -Wextra -Werror -fPIC -pthread \
        -DNTF_TEST_NICKELTC_QT -I"$repo/src" -I"$work" \
        -I"$qt/usr/include" -I"$qt/usr/include/QtCore" -I"$qt/usr/include/QtGui" \
        -I"$qt/usr/include/QtWidgets" \
        -isystem "$qt/usr/include/QtGui/5.2.1" -isystem "$qt/usr/include/QtGui/5.2.1/QtGui" \
        -isystem "$qt/usr/include/QtCore/5.2.1" -isystem "$qt/usr/include/QtCore/5.2.1/QtCore" \
        "$@" -L"$qt/usr/lib" -lQt5Widgets -lQt5Gui -lQt5Core -ldl
}
compile_qt "$repo/test/font-dropdown/font_dropdown_test.cc" "$repo/src/font_dropdown.cc" \
    -o "$work/font-dropdown"
compile_qt "$repo/test/rendering/shaping_test.cc" "$repo/src/detour.cc" \
    -o "$work/shaping"
"$cxx" -std=c++11 -mthumb -Wall -Wextra -Werror "$repo/test/detour/detour_test.cc" -o "$work/detour"
printf '%s\n' 'void nh_log(const char *fmt, ...);' > "$work/NickelHook.h"
"$cc" -std=gnu11 -mthumb -Wall -Wextra -Werror -pthread -I"$work" \
    "-DNTF_CONFIG_DIR=\"$work/log\"" "$repo/test/logging/log_test.c" -o "$work/logging"

mkdir "$work/empty-fonts" "$work/runtime"
export QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="$work/empty-fonts"
export QT_PLUGIN_PATH="$qt/usr/plugins" XDG_RUNTIME_DIR="$work/runtime"
chmod 700 "$XDG_RUNTIME_DIR"
run_arm() {
    timeout 120 "$qemu" -cpu cortex-a9 -L "$root" -E LD_PRELOAD="$work/auxv.so" \
        "$root/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3" \
        --library-path "$root/lib/arm-linux-gnueabihf:$qt/usr/lib:$qt/lib" "$@"
}
run_arm "$work/detour"
run_arm "$work/logging"
for shaper in ng old; do
    export QT_HARFBUZZ="$shaper"
    run_arm "$work/font-dropdown" "$fonts/Vollkorn-Regular.ttf" "$fonts/NotoSans-Regular.ttf"
    run_arm "$work/shaping" "$fonts" cache
    run_arm "$work/shaping" "$fonts" small-caps
done
