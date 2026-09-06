#ifndef NTF_DETOUR_H
#define NTF_DETOUR_H

#ifdef __cplusplus
extern "C" {
#endif

// Startup only: no thread may execute the target while its eight-byte entry is replaced.
// The caller owns a null original slot. Success publishes a Thumb trampoline there and,
// optionally, the relocated byte count. Failure leaves both outputs unchanged and returns
// a negative code. If the original bytes or permissions cannot be recovered, never return.
// The instruction guard handles the known prologues, not arbitrary Thumb code.
int ntf_detour_at(void *addr, void *replacement, void **original, int *relocated_out);

// Shared with the byte-patch installer. Reboot while NickelHook's boot failsafe is armed.
void ntf_patch_unsafe(const char *reason) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
