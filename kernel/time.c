/*-
 * Modified for solo5 by Ricardo Koller <kollerr.us.ibm.com>
 * Copyright (c) 2014, 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2015 Martin Lucina.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "kernel.h"

typedef unsigned long long bmk_time_t;

bmk_time_t rtc_epochoffset;

bmk_time_t pvclock_read_wall_clock(void);

/* Xen/KVM per-vcpu time ABI. */
struct pvclock_vcpu_time_info {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;
    uint32_t tsc_to_system_mul;
    int8_t tsc_shift;
    uint8_t flags;
    uint8_t pad[2];
} __attribute__((__packed__));

/* Xen/KVM wall clock ABI. */
struct pvclock_wall_clock {
    uint32_t version;
    uint32_t sec;
    uint32_t nsec;
} __attribute__((__packed__));


/*
 * pvclock structures shared with hypervisor.
 * TODO: These should be pointers (for Xen HVM support), but we can't use
 * bmk_pgalloc() here.
 */
volatile struct pvclock_vcpu_time_info pvclock_ti;
volatile struct pvclock_wall_clock pvclock_wc;

bmk_time_t pvclock_read_wall_clock(void) {
    uint32_t version;
    bmk_time_t wc_boot = 0;

    do {
        version = pvclock_wc.version;
        __asm__ ("mfence" ::: "memory");
        wc_boot = pvclock_wc.sec * NSEC_PER_SEC;
        wc_boot += pvclock_wc.nsec;
        __asm__ ("mfence" ::: "memory");
    } while ((pvclock_wc.version & 1) || (pvclock_wc.version != version));

    return wc_boot;
}


int pvclock_init(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t msr_kvm_system_time, msr_kvm_wall_clock;

    /*
     * Prefer new-style MSRs, and bail entirely if neither is indicated as
     * available by CPUID.
     */
    x86_cpuid(0x40000001, &eax, &ebx, &ecx, &edx);
    if (eax & (1 << 3)) {
        msr_kvm_system_time = 0x4b564d01;
        msr_kvm_wall_clock = 0x4b564d00;
    }
    else if (eax & (1 << 0)) {
        msr_kvm_system_time = 0x12;
        msr_kvm_wall_clock = 0x11;
    }
    else {
        return 1;
        }

        printf("Initializing the KVM Paravirtualized clock.\n");

    __asm__ __volatile("wrmsr" ::
        "c" (msr_kvm_system_time),
        "a" ((uint32_t)((uintptr_t)&pvclock_ti | 0x1)),
        "d" ((uint32_t)((uintptr_t)&pvclock_ti >> 32))
    );
    __asm__ __volatile("wrmsr" ::
        "c" (msr_kvm_wall_clock),
        "a" ((uint32_t)((uintptr_t)&pvclock_wc)),
        "d" ((uint32_t)((uintptr_t)&pvclock_wc >> 32))
    );
    /* Initialise epoch offset using wall clock time */
    rtc_epochoffset = pvclock_read_wall_clock();

    return 0;
}


static inline uint64_t mul64_32(uint64_t a, uint32_t b) {
    uint64_t prod;
#if defined(__x86_64__)
    /* For x86_64 the computation can be done using 64-bit multiply and
     * shift. */
    __asm__ (
        "mul %%rdx ; "
        "shrd $32, %%rdx, %%rax"
        : "=a" (prod)
        : "0" (a), "d" ((uint64_t)b)
    );
#elif defined(__i386__)
    /* For i386 we compute the partial products and add them up, discarding
     * the lower 32 bits of the product in the process. */
    uint32_t h = (uint32_t)(a >> 32);
    uint32_t l = (uint32_t)a;
    uint32_t t1, t2;
    __asm__ (
        "mul  %5       ; "  /* %edx:%eax = (l * b)                    */
        "mov  %4,%%eax ; "  /* %eax = h                               */
        "mov  %%edx,%4 ; "  /* t1 = ((l * b) >> 32)                   */
        "mul  %5       ; "  /* %edx:%eax = (h * b)                    */
        "xor  %5,%5    ; "  /* t2 = 0                                 */
        "add  %4,%%eax ; "  /* %eax = (h * b) + t1 (LSW)              */
        "adc  %5,%%edx ; "  /* %edx = (h * b) + t1 (MSW)              */
        : "=A" (prod), "=r" (t1), "=r" (t2)
        : "a" (l), "1" (h), "2" (b)
    );
#else
#error mul64_32 not supported for target architecture
#endif

    return prod;
}


uint64_t pvclock_monotonic(void) {
    uint32_t version;
    uint64_t delta, time_now;

    do {
        version = pvclock_ti.version;
        __asm__ ("mfence" ::: "memory");
        delta = rdtsc() - pvclock_ti.tsc_timestamp;
        if (pvclock_ti.tsc_shift < 0)
            delta >>= -pvclock_ti.tsc_shift;
        else
            delta <<= pvclock_ti.tsc_shift;
        time_now = mul64_32(delta, pvclock_ti.tsc_to_system_mul) +
            pvclock_ti.system_time;
        __asm__ ("mfence" ::: "memory");
    } while ((pvclock_ti.version & 1) || (pvclock_ti.version != version));

    return time_now;
}
