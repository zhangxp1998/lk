/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <lk/asm.h>

// void linux_boot_jump(paddr_t kernel_entry, paddr_t fdt, paddr_t stack_top)
//
// Enter an ARM64 Linux Image using the Linux boot protocol:
//   x0 = FDT physical address
//   x1-x3 = 0
//   SP points at identity-mapped memory
//   MMU and caches disabled before branching to the physical kernel entry.
FUNCTION(linux_boot_jump)
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2

    adr     x9, .Lmmu_off
    ldr     x10, =KERNEL_BASE
    ldr     x11, =MEMBASE
    sub     x9, x9, x10
    add     x9, x9, x11

    // Continue from an identity-mapped trampoline before disabling the MMU.
    // Once SCTLR_EL1.M is cleared, the following instruction fetches use
    // physical addresses.
    br      x9

.Lmmu_off:
    mov     sp, x21
    dsb     sy
    mrs     x12, sctlr_el1
    bic     x12, x12, #(1 << 0)   // M: MMU
    bic     x12, x12, #(1 << 2)   // C: data/unified cache
    bic     x12, x12, #(1 << 12)  // I: instruction cache
    msr     sctlr_el1, x12
    isb

    mov     x0, x20
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x19
END_FUNCTION(linux_boot_jump)
