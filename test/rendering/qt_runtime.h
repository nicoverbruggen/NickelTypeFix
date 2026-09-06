#ifndef NTF_TEST_QT_RUNTIME_H
#define NTF_TEST_QT_RUNTIME_H

#include <QtCore/QDebug>
#include <cstdint>
#ifdef NTF_TEST_NICKELTC_QT
#include <dlfcn.h>
#endif

inline void ntf_test_select_shaper(bool ng)
{
#ifdef NTF_TEST_NICKELTC_QT
    // Toolchain Qt ignores QT_HARFBUZZ. Refuse a different layout before using these offsets.
    // This is test setup for the pinned QtGui binary, never a selector for device firmware.
    void *shape = dlsym(RTLD_DEFAULT, "_ZNK11QTextEngine9shapeTextEi");
    Dl_info info = {nullptr, nullptr, nullptr, nullptr};
    if (!shape || !dladdr(shape, &info) ||
        ((uintptr_t)shape & ~uintptr_t(1)) - (uintptr_t)info.dli_fbase != 0x1219c4)
        qFatal("FAIL: unexpected toolchain QtGui layout");
    unsigned char *base = static_cast<unsigned char *>(info.dli_fbase);
    unsigned char *flag = *reinterpret_cast<unsigned char **>(base + 0x2e11b0);
    if (flag != base + 0x2e3d78 || *flag > 1)
        qFatal("FAIL: unexpected toolchain shaper selector");
    *flag = ng;
#else
    Q_UNUSED(ng);
#endif
}
#endif
