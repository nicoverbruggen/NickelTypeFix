#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int permissions(const void *p)
{
    FILE *f = fopen("/proc/self/maps", "r");
    assert(f);
    char line[1024], flags[5];
    unsigned long lo, hi;
    int result = -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, flags) != 3) continue;
        if ((uintptr_t)p < lo || (uintptr_t)p >= hi) continue;
        result = (flags[0] == 'r' ? PROT_READ : 0) |
                 (flags[1] == 'w' ? PROT_WRITE : 0) |
                 (flags[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(f);
    return result;
}

static const int rx = PROT_READ | PROT_EXEC;
static const unsigned char body[] = {
    // push {r4,lr}; sub sp,#8; mov r4,r0; movs r0,#42; add sp,#8; pop {r4,pc}
    0x10,0xb5,0x82,0xb0,0x04,0x46,0x2a,0x20,0x02,0xb0,0x10,0xbd
};

enum Fault {
    None, NoMaps, BadPage, NoMemory, TrampolineProtect, TrampolineCorruption,
    TargetProtect, PartialTargetProtect, TargetCorruption, TargetRestore, TargetChanged,
    RollbackProtect, RollbackCorruption, RollbackRestore
};
static Fault fault = None;
static void *target_page = 0;
static uint32_t *target_entry = 0;
static void *trampoline = 0;
static int rx_calls = 0, rwx_calls = 0, stores = 0;

static void *test_mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset)
{
    if (fault == NoMemory) return MAP_FAILED;
#if UINTPTR_MAX > UINT32_MAX
    // The installer emits 32-bit ARM pointers. Hints also work under qemu-user, which may
    // ignore MAP_32BIT. Production refuses a trampoline outside the 32-bit address range.
    static uintptr_t next = 0x10000000;
    if (!addr) { addr = (void *)next; next += 0x100000; }
#ifdef MAP_32BIT
    flags |= MAP_32BIT;
#endif
#endif
    void *p = mmap(addr, size, prot, flags, fd, offset);
    if (target_page) trampoline = p;
    return p;
}

static int test_mprotect(void *addr, size_t size, int prot)
{
    if (addr == trampoline && fault == TrampolineProtect) return -1;
    if (addr == trampoline && fault == TargetChanged) {
        size_t page = sysconf(_SC_PAGESIZE);
        assert(mprotect(target_page, page, PROT_READ | PROT_WRITE) == 0);
        ((unsigned char *)target_entry)[0] ^= 1;
        assert(mprotect(target_page, page, rx) == 0);
    }
    if (addr == target_page) {
        if (prot == rx) {
            ++rx_calls;
            if ((fault == TargetRestore && rx_calls == 1) || fault == RollbackRestore) return -1;
        } else if (prot == (rx | PROT_WRITE)) {
            ++rwx_calls;
            if (fault == TargetProtect) return -1;
            if (fault == PartialTargetProtect && rwx_calls == 1) {
                assert(mprotect(addr, size, prot) == 0);
                return -1;
            }
            if (fault == RollbackProtect && rwx_calls == 2) return -1;
        }
    }
    return mprotect(addr, size, prot);
}

static void test_store(uint32_t *addr, uint32_t value, int order)
{
    if (addr == target_entry) {
        ++stores;
        if ((stores == 1 && (fault == TargetCorruption || fault == RollbackProtect)) ||
            fault == RollbackCorruption) value ^= 1;
    }
    __atomic_store_n(addr, value, order);
}

static void *test_memcpy(void *dest, const void *src, size_t size)
{
    void *result = memcpy(dest, src, size);
    if (dest == trampoline && fault == TrampolineCorruption) ((unsigned char *)dest)[0] ^= 1;
    return result;
}

static FILE *test_fopen(const char *path, const char *mode)
{
    return fault == NoMaps ? 0 : fopen(path, mode);
}

static long test_sysconf(int key)
{
    return fault == BadPage ? -1 : sysconf(key);
}

#define mmap test_mmap
#define mprotect test_mprotect
#define memcpy test_memcpy
#define fopen test_fopen
#define sysconf test_sysconf
#define __atomic_store_n test_store
#ifndef NTF_DETOUR_SOURCE
#define NTF_DETOUR_SOURCE "../../src/detour.cc"
#endif
#include NTF_DETOUR_SOURCE
#undef mmap
#undef mprotect
#undef memcpy
#undef fopen
#undef sysconf
#undef __atomic_store_n

extern "C" __attribute__((noreturn)) void ntf_patch_unsafe(const char *)
{
    // The production handler reboots. A child exit proves the installer cannot return success
    // or a recoverable error and let NickelHook disarm its failsafe after a broken rollback.
    _exit(99);
}

struct Fixture {
    size_t page;
    unsigned char *memory, *fn;
    void *original;
    int relocated;

    explicit Fixture(bool guard = true) : page(sysconf(_SC_PAGESIZE)), original(0), relocated(-123)
    {
        fault = None; target_page = 0; trampoline = 0;
        rx_calls = rwx_calls = stores = 0;
        memory = (unsigned char *)test_mmap(0, page * 2, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(memory != MAP_FAILED && (uintptr_t)memory <= UINT32_MAX);
        fn = memory;
        memcpy(fn, body, sizeof body);
        // movs r0,#7; bx lr
        const unsigned char replacement[] = {0x07,0x20,0x70,0x47};
        memcpy(memory + 64, replacement, sizeof replacement);
        assert(mprotect(memory, guard ? page : page * 2, rx) == 0);
        if (guard) assert(mprotect(memory + page, page, PROT_NONE) == 0);
        target_page = memory;
        target_entry = (uint32_t *)fn;
    }

    int install() { return ntf_detour_at(fn, memory + 65, &original, &relocated); }

    void unchanged()
    {
        assert(original == 0 && relocated == -123);
        assert(permissions(memory) == rx);
        assert(memcmp(fn, body, sizeof body) == 0);
        if (trampoline && trampoline != MAP_FAILED) assert(permissions(trampoline) == -1);
    }

    ~Fixture()
    {
        fault = None; target_page = 0;
        if (original) assert(munmap((void *)((uintptr_t)original & ~uintptr_t(1)), page) == 0);
        assert(munmap(memory, page * 2) == 0);
    }
};

static void successful_install_restores_permissions_and_preserves_original()
{
    Fixture f;
#ifdef __arm__
    typedef int (*Function)();
    Function target = (Function)((uintptr_t)f.fn | 1);
    assert(target() == 42);
#endif
    assert(f.install() == 0);
    assert(f.relocated == 8);
    assert(permissions(f.fn) == rx);
    unsigned char *saved = (unsigned char *)((uintptr_t)f.original & ~uintptr_t(1));
    assert(permissions(saved) == rx);
    assert(memcmp(saved, body, 8) == 0);
    uint32_t destination, continuation;
    memcpy(&destination, f.fn + 4, 4);
    memcpy(&continuation, saved + 12, 4);
    assert(destination == ((uintptr_t)(f.memory + 64) | 1));
    assert(continuation == ((uintptr_t)(f.fn + 8) | 1));
#ifdef __arm__
    assert(target() == 7);
    assert(((Function)f.original)() == 42);
#endif
    void *original = f.original;
    assert(f.install() < 0 && f.original == original);
    void *second = 0;
    assert(ntf_detour_at(f.fn, f.memory + 65, &second, 0) < 0 && !second);
}

static void whole_instruction_at_eight_byte_boundary_is_relocated()
{
    Fixture f;
    assert(mprotect(f.memory, f.page, PROT_READ | PROT_WRITE) == 0);
    // movw r0,#42 at offset six ends at ten. Its continuation follows a padding NOP.
    const unsigned char prefix[] = {0x00,0xbf,0x00,0xbf,0x00,0xbf,0x40,0xf2,0x2a,0x00};
    memcpy(f.fn, prefix, sizeof prefix);
    f.fn[10] = 0x70; f.fn[11] = 0x47; // bx lr
    assert(mprotect(f.memory, f.page, rx) == 0);
    assert(f.install() == 0 && f.relocated == 10);
    unsigned char *saved = (unsigned char *)((uintptr_t)f.original & ~uintptr_t(1));
    assert(memcmp(saved, prefix, sizeof prefix) == 0);
    assert(saved[10] == 0 && saved[11] == 0xbf);
    uint32_t continuation;
    memcpy(&continuation, saved + 16, 4);
    assert(continuation == ((uintptr_t)(f.fn + 10) | 1));
#ifdef __arm__
    typedef int (*Function)();
    assert(((Function)f.original)() == 42);
#endif
}

static void invalid_ranges_are_rejected_before_reading()
{
    Fixture f;
    void *addresses[] = {0, (void *)4, (void *)(UINTPTR_MAX - 3), f.fn + 2,
                        f.memory + f.page, f.memory + f.page - 4};
    for (void *addr : addresses) {
        assert(ntf_detour_at(addr, f.memory + 65, &f.original, &f.relocated) < 0);
        f.unchanged();
    }
    assert(ntf_detour_at(f.fn, f.memory + f.page, &f.original, 0) < 0);
    for (int prot : {PROT_READ, PROT_READ | PROT_WRITE, rx | PROT_WRITE}) {
        assert(mprotect(f.memory, f.page, prot) == 0);
        assert(f.install() < 0);
        assert(permissions(f.memory) == prot);
    }
    assert(mprotect(f.memory, f.page, rx) == 0);
    f.unchanged();

    // The first eight bytes fit, but the last instruction needs two bytes in a PROT_NONE page.
    assert(mprotect(f.memory, f.page, PROT_READ | PROT_WRITE) == 0);
    unsigned char *edge = f.memory + f.page - 8;
    const unsigned char prefix[] = {0x00,0xbf,0x00,0xbf,0x00,0xbf,0x2d,0xe9};
    memcpy(edge, prefix, sizeof prefix);
    assert(mprotect(f.memory, f.page, rx) == 0);
    assert(ntf_detour_at(edge, f.memory + 65, &f.original, &f.relocated) < 0);
    assert(memcmp(edge, prefix, sizeof prefix) == 0);
}

static void patch_spanning_two_pages_restores_both()
{
    // Keep both pages in one mapping. A PROT_NONE guard splits VMAs under qemu-user.
    Fixture f(false);
    assert(mprotect(f.memory, f.page * 2, PROT_READ | PROT_WRITE) == 0);
    f.fn = f.memory + f.page - 4;
    target_entry = (uint32_t *)f.fn;
    memcpy(f.fn, body, sizeof body);
    assert(mprotect(f.memory, f.page * 2, rx) == 0);
    assert(f.install() == 0);
    assert(permissions(f.memory) == rx && permissions(f.memory + f.page) == rx);
#ifdef __arm__
    typedef int (*Function)();
    assert(((Function)f.original)() == 42);
#endif
}

static void changed_target_is_not_overwritten_during_preparation()
{
    Fixture f;
    fault = TargetChanged;
    assert(f.install() < 0);
    assert(!f.original && f.relocated == -123);
    assert(f.fn[0] == (body[0] ^ 1));
    assert(memcmp(f.fn + 1, body + 1, sizeof body - 1) == 0);
    assert(permissions(f.memory) == rx && permissions(trampoline) == -1);
}

static void pc_relative_prologues_are_rejected()
{
    const unsigned char prefixes[][4] = {
        {0x00,0x48,0,0}, // ldr r0,[pc]
        {0x00,0xe0,0,0}, // b
        {0xdf,0xf8,0x00,0x00}, // ldr.w r0,[pc]
        {0x00,0xf0,0x00,0xf8}  // bl
    };
    for (const auto &prefix : prefixes) {
        Fixture f;
        assert(mprotect(f.memory, f.page, PROT_READ | PROT_WRITE) == 0);
        memcpy(f.fn, prefix, sizeof prefix);
        assert(mprotect(f.memory, f.page, rx) == 0);
        assert(f.install() < 0);
        assert(!f.original && f.relocated == -123);
        assert(memcmp(f.fn, prefix, sizeof prefix) == 0 && permissions(f.fn) == rx);
    }
}

static void recoverable_failures_leave_no_patch_or_writable_code()
{
    const Fault failures[] = {NoMaps, BadPage, NoMemory, TrampolineProtect, TrampolineCorruption,
                             TargetProtect, PartialTargetProtect, TargetCorruption, TargetRestore};
    for (Fault failure : failures) {
        Fixture f;
        fault = failure;
        assert(f.install() < 0);
        f.unchanged();
    }
}

static void unverified_rollback_cannot_return_to_startup()
{
    const Fault failures[] = {RollbackProtect, RollbackCorruption, RollbackRestore};
    for (Fault failure : failures) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            Fixture f;
            fault = failure;
            f.install();
            _exit(1);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 99);
    }
}

int main()
{
    successful_install_restores_permissions_and_preserves_original();
    whole_instruction_at_eight_byte_boundary_is_relocated();
    invalid_ranges_are_rejected_before_reading();
    patch_spanning_two_pages_restores_both();
    changed_target_is_not_overwritten_during_preparation();
    pc_relative_prologues_are_rejected();
    recoverable_failures_leave_no_patch_or_writable_code();
    unverified_rollback_cannot_return_to_startup();
    puts("detour installation tests passed");
}
