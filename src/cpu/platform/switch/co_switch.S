.text
.align 2

// AArch64 firmware (EL0/newlib) cooperative context frame (240 bytes):
// x18-x30, then v8-v15. x18 is the platform register on Linux AArch64; it is
// saved and restored here for TLS safety even though newlib does not use it.
// x0 = address holding the target frame pointer; x1 = address to receive the
// current frame pointer. Leaf-only: never calls C++.
//
// ELF variant: this file exports unmangled (non-underscored) symbols, unlike
// the Mach-O companion under src/platform/macos/, so the runtime links cleanly
// for aarch64-none-elf (NintendoSwitch).
.globl mkw_co_switch
mkw_co_switch:
    sub sp, sp, #240
    str x18, [sp, #0]
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp x29, x30, [sp, #96]
    stp q8, q9, [sp, #112]
    stp q10, q11, [sp, #144]
    stp q12, q13, [sp, #176]
    stp q14, q15, [sp, #208]
    mov x2, sp
    str x2, [x1]

    ldr x2, [x0]
    mov sp, x2
    ldr x18, [sp, #0]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp, #96]
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    add sp, sp, #240
    ret

// Creates a frame compatible with mkw_co_switch and returns its saved SP.
// x0 = one-past-end stack pointer, x1 = entry(void*), x2 = entry argument.
.globl mkw_co_init
mkw_co_init:
    bic x0, x0, #0xf
    sub x0, x0, #240
    str x18, [x0, #0]
    str x1, [x0, #16]      // x19: entry
    str x2, [x0, #24]      // x20: argument
    str xzr, [x0, #96]     // x29
    adrp x3, mkw_co_entry_trampoline
    add x3, x3, :lo12:mkw_co_entry_trampoline
    str x3, [x0, #104]     // x30
    ret

mkw_co_entry_trampoline:
    mov x0, x20
    blr x19
    brk #0