/*
 * Assembly glue between Linux-aarch64 (AAPCS64) callers and macOS code.
 */

    .text
    .p2align 2

/*
 * TLSDESC resolver. Called by ELF code as `blr` with x0 = &descriptor;
 * descriptor[1] holds an emutls control variable. Must return, in x0, a value
 * that the caller adds to TPIDR_EL0 -- the loader rewrote those reads to 0,
 * so we return the absolute address. Every other register (and NZCV) must be
 * preserved.
 */
    .globl _elf_tlsdesc_dynamic
_elf_tlsdesc_dynamic:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #(20 * 8 + 24 * 16)
    stp     x1, x2, [sp, #0]
    stp     x3, x4, [sp, #16]
    stp     x5, x6, [sp, #32]
    stp     x7, x8, [sp, #48]
    stp     x9, x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x16, [sp, #112]
    str     x17, [sp, #128]
    mrs     x9, nzcv
    str     x9, [sp, #136]
    stp     q0, q1, [sp, #160]
    stp     q2, q3, [sp, #192]
    stp     q4, q5, [sp, #224]
    stp     q6, q7, [sp, #256]
    stp     q16, q17, [sp, #288]
    stp     q18, q19, [sp, #320]
    stp     q20, q21, [sp, #352]
    stp     q22, q23, [sp, #384]
    stp     q24, q25, [sp, #416]
    stp     q26, q27, [sp, #448]
    stp     q28, q29, [sp, #480]
    stp     q30, q31, [sp, #512]
    ldr     x0, [x0, #8]
    adrp    x9, _elf_tls_get_address_fn@PAGE
    ldr     x9, [x9, _elf_tls_get_address_fn@PAGEOFF]
    blr     x9
    ldp     q0, q1, [sp, #160]
    ldp     q2, q3, [sp, #192]
    ldp     q4, q5, [sp, #224]
    ldp     q6, q7, [sp, #256]
    ldp     q16, q17, [sp, #288]
    ldp     q18, q19, [sp, #320]
    ldp     q20, q21, [sp, #352]
    ldp     q22, q23, [sp, #384]
    ldp     q24, q25, [sp, #416]
    ldp     q26, q27, [sp, #448]
    ldp     q28, q29, [sp, #480]
    ldp     q30, q31, [sp, #512]
    ldr     x9, [sp, #136]
    msr     nzcv, x9
    ldp     x1, x2, [sp, #0]
    ldp     x3, x4, [sp, #16]
    ldp     x5, x6, [sp, #32]
    ldp     x7, x8, [sp, #48]
    ldp     x9, x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x16, [sp, #112]
    ldr     x17, [sp, #128]
    mov     sp, x29
    ldp     x29, x30, [sp], #16
    ret

/*
 * Import tracing. Thunks arrive with x16 = real target, x17 = name. Preserve
 * all argument registers (x0-x8, q0-q7) across the log call, then tail-branch
 * so stack arguments and the return address are untouched.
 */
    .globl _elf_trace_common
_elf_trace_common:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #208
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x16, [sp, #64]
    stp     q0, q1, [sp, #80]
    stp     q2, q3, [sp, #112]
    stp     q4, q5, [sp, #144]
    stp     q6, q7, [sp, #176]
    mov     x0, x17
    bl      _elf_trace_log
    ldp     q0, q1, [sp, #80]
    ldp     q2, q3, [sp, #112]
    ldp     q4, q5, [sp, #144]
    ldp     q6, q7, [sp, #176]
    ldp     x8, x16, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    mov     sp, x29
    ldp     x29, x30, [sp], #16
    br      x16

/*
 * Variadic entry points. A Linux caller passes variadic arguments like named
 * ones (x0-x7 / q0-q7, then 8-byte stack slots); Apple's ABI puts them all on
 * the stack. Each stub spills the registers and builds a glibc-layout va_list:
 *
 *   struct { void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs; }
 *
 * then calls IMPL with the NGP named integer arguments untouched and a
 * pointer to that va_list as argument number NGP.
 *
 * Frame (240 bytes): [0] fp/lr, [16] x0-x7, [80] q0-q7, [208] va_list.
 */
.macro VARIADIC name, impl, ngp
    .globl \name
\name:
    mov     x9, sp
    sub     sp, sp, #240
    stp     x29, x30, [sp]
    mov     x29, sp
    stp     x0, x1, [sp, #16]
    stp     x2, x3, [sp, #32]
    stp     x4, x5, [sp, #48]
    stp     x6, x7, [sp, #64]
    stp     q0, q1, [sp, #80]
    stp     q2, q3, [sp, #112]
    stp     q4, q5, [sp, #144]
    stp     q6, q7, [sp, #176]
    str     x9, [sp, #208]
    add     x10, sp, #80
    str     x10, [sp, #216]
    add     x10, sp, #208
    str     x10, [sp, #224]
    mov     w10, #(-(8 - \ngp) * 8)
    str     w10, [sp, #232]
    mov     w10, #-128
    str     w10, [sp, #236]
    add     x\ngp, sp, #208
    bl      \impl
    ldp     x29, x30, [sp]
    add     sp, sp, #240
    ret
.endm

    VARIADIC _lx_printf,          _lx_vprintf_impl,          1
    VARIADIC _lx_fprintf,         _lx_vfprintf_impl,         2
    VARIADIC _lx_snprintf,        _lx_vsnprintf_impl,        3
    VARIADIC _lx_sprintf,         _lx_vsprintf_impl,         2
    VARIADIC _lx_sscanf,          _lx_vsscanf_impl,          2
    VARIADIC _lx_fscanf,          _lx_vfscanf_impl,          2
    VARIADIC _lx___printf_chk,    _lx___vprintf_chk_impl,    2
    VARIADIC _lx___fprintf_chk,   _lx___vfprintf_chk_impl,   3
    VARIADIC _lx___sprintf_chk,   _lx___vsprintf_chk_impl,   4
    VARIADIC _lx___snprintf_chk,  _lx___vsnprintf_chk_impl,  5
