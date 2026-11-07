#include "eg2.h"

#if EG2_HAVE_SME
#include <errno.h>
#include <sys/sysctl.h>

extern int eg_sme_width(void);

/* Plain-C ABI: preserve d8-d15 and reject an already-owned streaming/ZA context. */
__asm__(
    ".text\n.arch armv9-a+sme\n.p2align 2\n.globl _eg_sme_width\n_eg_sme_width:\n"
    "mrs x8, SVCR\ncbnz x8, 9f\nmrs x8, TPIDR2_EL0\ncbnz x8, 9f\n"
    "stp d8, d9, [sp, #-64]!\nstp d10, d11, [sp, #16]\n"
    "stp d12, d13, [sp, #32]\nstp d14, d15, [sp, #48]\n"
    "smstart sm\ncntw x0\nsmstop sm\n"
    "ldp d14, d15, [sp, #48]\nldp d12, d13, [sp, #32]\n"
    "ldp d10, d11, [sp, #16]\nldp d8, d9, [sp], #64\nret\n"
    "9:\nmov w0, #-1\nret\n"
    ".p2align 2\n.globl _eg_sme_block\n_eg_sme_block:\n"
    "cbz x0, 9f\ncbz x1, 9f\ncbz x2, 9f\ncbz w3, 9f\n"
    "mrs x8, SVCR\ncbnz x8, 9f\nmrs x8, TPIDR2_EL0\ncbnz x8, 9f\n"
    "stp d8, d9, [sp, #-64]!\nstp d10, d11, [sp, #16]\n"
    "stp d12, d13, [sp, #32]\nstp d14, d15, [sp, #48]\n"
    "smstart\ncntw x8\ncmp x8, #16\nb.ne 8f\nptrue p0.s\n"
    "mov w12, #0\nmov x9, x2\n"
    "1:\nld1w {za0h.s[w12, 0]}, p0/z, [x9]\n"
    "add x9, x9, #64\nadd w12, w12, #1\ncmp w12, #16\nb.ne 1b\n"
    "2:\nld1w {z0.s}, p0/z, [x0]\nld1w {z1.s}, p0/z, [x1]\n"
    "fmopa za0.s, p0/m, p0/m, z0.s, z1.s\n"
    "add x0, x0, #64\nadd x1, x1, #64\nsubs w3, w3, #1\nb.ne 2b\n"
    "mov w12, #0\n"
    "3:\nst1w {za0h.s[w12, 0]}, p0, [x2]\n"
    "add x2, x2, #64\nadd w12, w12, #1\ncmp w12, #16\nb.ne 3b\n"
    "mov w0, #0\nb 7f\n8:\nmov w0, #-1\n"
    "7:\nsmstop\nldp d14, d15, [sp, #48]\nldp d12, d13, [sp, #32]\n"
    "ldp d10, d11, [sp, #16]\nldp d8, d9, [sp], #64\nret\n"
    "9:\nmov w0, #-1\nret\n");

int eg_sme_available(void) {
    const char *names[] = {"hw.optional.arm.FEAT_SME", "hw.optional.arm.SME_F32F32"};
    for (unsigned int i = 0; i < 2; i++) {
        int enabled = 0;
        size_t size = sizeof(enabled);
        if (sysctlbyname(names[i], &enabled, &size, 0, 0)) {
            if (errno == ENOENT) return 0;
            return eg_error("cannot query optional CPU feature: ", names[i]);
        }
        if (size != sizeof(enabled)) return eg_error("invalid CPU feature size: ", names[i]);
        if (!enabled) return 0;
    }
    int width = eg_sme_width();
    if (width < 0) return eg_error("model initialization requires a private non-streaming context", 0);
    return width == 16;
}
#else
int eg_sme_available(void) {
    return 0;
}
#endif
