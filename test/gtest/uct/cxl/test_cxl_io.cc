/*
 * Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <cstring>
#include <vector>
extern "C" {
#include <uct/cxl/cxl_io.h>
}

/*
 * The CXL aperture only accepts <=8 byte accesses, so the helpers must copy
 * with naturally aligned 8-byte accesses on the aperture side and single bytes
 * at unaligned ends. Ordinary RAM can't verify the access widths (see the
 * disassembly check in docs/parallel_ucx_plan.md), but it can verify that every
 * (length, aperture offset, buffer offset) combination copies exactly the
 * requested bytes and nothing around them.
 */
static const size_t PAD       = 16; /* multiple of 8: keeps offsets honest */
static const size_t MAX_OFF   = 8;
static const size_t MAX_LEN   = 1024;
static const uint8_t SENTINEL = 0xa5;

class test_cxl_io : public ucs::test {
protected:
    typedef void (*copy_func_t)(void *dst, const void *src, size_t len);

    static void pattern_fill(uint8_t *buf, size_t size)
    {
        for (size_t i = 0; i < size; ++i) {
            buf[i] = (uint8_t)(i * 31 + 7);
        }
    }

    static void io_write(void *dst, const void *src, size_t len)
    {
        uct_cxl_io_write(dst, src, len);
    }

    static void io_read(void *dst, const void *src, size_t len)
    {
        uct_cxl_io_read(dst, src, len);
    }

    static std::vector<size_t> lengths()
    {
        std::vector<size_t> lens;

        for (size_t len = 0; len <= 40; ++len) {
            lens.push_back(len);
        }
        /* Sizes that matter to the AM FIFO and an RMA-sized transfer */
        lens.push_back(232);
        lens.push_back(240);
        lens.push_back(256);
        lens.push_back(MAX_LEN);
        return lens;
    }

    /*
     * Copy 'len' bytes from 'src + src_off' to 'dst + dst_off' for every
     * combination of offsets, then verify the destination range holds the
     * source bytes and the sentinel padding around it is intact. The source
     * buffer is checked too, so a copy that scribbles on its input is caught.
     */
    void run_sweep(copy_func_t copy)
    {
        const size_t size = PAD + MAX_OFF + MAX_LEN + PAD;
        /* 64-byte alignment so that an offset's alignment equals its value % 8 */
        alignas(64) static uint8_t src[PAD + MAX_OFF + MAX_LEN + PAD];
        alignas(64) static uint8_t dst[PAD + MAX_OFF + MAX_LEN + PAD];
        uint8_t src_ref[sizeof(src)];

        pattern_fill(src, size);
        memcpy(src_ref, src, size);

        std::vector<size_t> lens = lengths();
        for (size_t len : lens) {
            for (size_t src_off = 0; src_off < MAX_OFF; ++src_off) {
                for (size_t dst_off = 0; dst_off < MAX_OFF; ++dst_off) {
                    memset(dst, SENTINEL, size);

                    copy(dst + PAD + dst_off, src + PAD + src_off, len);

                    ASSERT_EQ(0, memcmp(dst + PAD + dst_off,
                                        src + PAD + src_off, len))
                            << "len=" << len << " src_off=" << src_off
                            << " dst_off=" << dst_off;

                    for (size_t i = 0; i < size; ++i) {
                        if ((i >= PAD + dst_off) && (i < PAD + dst_off + len)) {
                            continue;
                        }
                        ASSERT_EQ(SENTINEL, dst[i])
                                << "write outside range at byte " << i
                                << " len=" << len << " src_off=" << src_off
                                << " dst_off=" << dst_off;
                    }

                    ASSERT_EQ(0, memcmp(src, src_ref, size))
                            << "source modified len=" << len
                            << " src_off=" << src_off << " dst_off=" << dst_off;
                }
            }
        }
    }
};

/* Copy from a host buffer into the "aperture" at every alignment */
UCS_TEST_F(test_cxl_io, write) {
    run_sweep(io_write);
}

/* Copy from the "aperture" into a host buffer at every alignment */
UCS_TEST_F(test_cxl_io, read) {
    run_sweep(io_read);
}

/* Data written with uct_cxl_io_write must read back identically */
UCS_TEST_F(test_cxl_io, roundtrip) {
    alignas(64) uint8_t aperture[MAX_LEN + 2 * PAD];
    alignas(64) uint8_t in[MAX_LEN + MAX_OFF];
    alignas(64) uint8_t out[MAX_LEN + MAX_OFF];

    pattern_fill(in, sizeof(in));

    for (size_t len : lengths()) {
        for (size_t off = 0; off < MAX_OFF; ++off) {
            memset(aperture, SENTINEL, sizeof(aperture));
            memset(out, 0, sizeof(out));

            uct_cxl_io_write(aperture + PAD + off, in + off, len);
            uct_cxl_io_read(out + off, aperture + PAD + off, len);

            ASSERT_EQ(0, memcmp(in + off, out + off, len))
                    << "len=" << len << " off=" << off;
        }
    }
}

UCS_TEST_F(test_cxl_io, read64_write64) {
    alignas(64) uint8_t buf[4 * sizeof(uint64_t)];
    /* Distinct bytes, so a byte-order or width mistake is visible */
    const uint64_t val = 0x0102030405060708ull;

    for (size_t slot = 0; slot < 4; ++slot) {
        uint8_t *p = buf + slot * sizeof(uint64_t);

        memset(buf, SENTINEL, sizeof(buf));
        uct_cxl_io_write64(p, val);

        EXPECT_EQ(val, uct_cxl_io_read64(p));
        /* Same bytes a plain 8-byte store would produce */
        EXPECT_EQ(0, memcmp(p, &val, sizeof(val)));
        for (size_t i = 0; i < sizeof(buf); ++i) {
            if ((i < slot * sizeof(uint64_t)) ||
                (i >= (slot + 1) * sizeof(uint64_t))) {
                EXPECT_EQ(SENTINEL, buf[i]) << "slot=" << slot << " byte=" << i;
            }
        }
    }
}
