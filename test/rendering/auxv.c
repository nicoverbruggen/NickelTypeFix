#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

// Qt 5.2 reads /proc/self/auxv directly. Under qemu-user that file belongs to the host
// emulator, so its word size and CPU flags can differ from the ARM process. getauxval
// reads the guest loader's auxiliary vector. Give Qt the ARM HWCAP entry it needs.
// This shim is preloaded only into the test process, never into Nickel.
int open64(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, unsigned);
        va_end(args);
    }
    if (strcmp(path, "/proc/self/auxv") != 0)
        return syscall(SYS_open, path, flags, mode);

    int fds[2];
    if (pipe2(fds, flags & O_CLOEXEC) != 0) return -1;
    const unsigned long auxv[] = {AT_HWCAP, getauxval(AT_HWCAP), AT_NULL, 0};
    ssize_t count = write(fds[1], auxv, sizeof auxv);
    int error = errno;
    close(fds[1]);
    if (count != sizeof auxv) {
        close(fds[0]);
        errno = count < 0 ? error : EIO;
        return -1;
    }
    return fds[0];
}
