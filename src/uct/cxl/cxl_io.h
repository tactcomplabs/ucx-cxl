/*
 * Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
*/

#ifndef UCT_CXL_IO_H
#define UCT_CXL_IO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ucs/debug/assert.h>
#include "ucs/sys/compiler_def.h"

#if defined(__riscv)
#define uct_cxl_fence() asm volatile("fence iorw, iorw" ::: "memory")
#elif defined(__x86_64__)
#define uct_cxl_fence() asm volatile("mfence" ::: "memory")
#else
#include <stdatomic.h>
#define uct_cxl_fence() __atomic_thread_fence(__ATOMIC_SEQ_CST) /* revisit */
#endif

static UCS_F_ALWAYS_INLINE uint64_t uct_cxl_io_read64(const void *mmio)
{
    ucs_assert(((uintptr_t)mmio & 7) == 0); /* 8-byte alignment */
    return *((const volatile uint64_t *)mmio);
}

static UCS_F_ALWAYS_INLINE void uct_cxl_io_write64(void *mmio, uint64_t val)
{
    ucs_assert(((uintptr_t)mmio & 7) == 0); /* 8-byte alignment */
    *(volatile uint64_t *)mmio = val;
}

static UCS_F_ALWAYS_INLINE void
uct_cxl_io_write(void *mmio_dst, const void *src, size_t len)
{
    volatile uint8_t *d = (volatile uint8_t *)mmio_dst;
    const uint8_t *s    = (const uint8_t *)src;
    uint64_t buf;

    if (len == 0) {
        return;
    }

    /* Copy byte-by-byte until the aperture side is 8-byte aligned */
    while ((len > 0) && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        len--;
    }

    /* Write in 8 byte chunks (maximum size for a MMIO transaction) */
    for (; len >= 8; len -= 8) {
        memcpy(&buf, s, 8);
        uct_cxl_io_write64((void *)d, buf);
        d += 8;
        s += 8;
    }

    /* Clean up remaining bytes */
    while (len > 0) {
        *d++ = *s++;
        len--;
    }
}

static UCS_F_ALWAYS_INLINE void
uct_cxl_io_read(void *dst, const void *mmio_src, size_t len)
{
    const volatile uint8_t *s = (const volatile uint8_t *)mmio_src;
    uint8_t *d                = (uint8_t *)dst;
    uint64_t buf;

    if (len == 0) {
        return;
    }

    /* Copy byte-by-byte until the aperture side is 8-byte aligned */
    while ((len > 0) && ((uintptr_t)s & 7)) {
        *d++ = *s++;
        len--;
    }

    /* Read in 8 byte chunks (maximum size for a MMIO transaction) */
    for (; len >= 8; len -= 8) {
        buf = uct_cxl_io_read64((void *)s);
        memcpy(d, &buf, 8);
        d += 8;
        s += 8;
    }

    /* Clean up remaining bytes */
    while (len > 0) {
        *d++ = *s++;
        len--;
    }
}

#endif