/*
 * Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
extern "C" {
#include <uct/api/uct.h>
#include <uct/cxl/cxl_iface.h>
}

/*
 * Single-process tests of the AM FIFO (send, progress, pending). The MD's
 * local device and its aperture both map the same temp file, so the iface
 * talks to itself: tx.fifo and rx.fifo are the same slots, and remote_ctrl and
 * local_ctrl are the same control block. Every message sent is the next one
 * the iface's own progress receives, and every ack it writes is the one its
 * own send path reads. White-box access to uct_cxl_iface_t lets the tests
 * start the stream anywhere and plant half-written slots and acks.
 */
class test_cxl_fifo : public ucs::test {
protected:
    static const uint8_t AM_ID       = 5;
    static const uint8_t AM_ID_REPLY = 6;
    static const size_t FILE_SIZE    = 1024 * 1024;
    static const uint64_t DEPTH      = UCT_CXL_FIFO_DEPTH;
    /* A stream position past 2^32 + DEPTH, so that the seq and ack of the
     * current slot have both 32-bit halves nonzero */
    static const uint64_t HIGH_POS = (1ull << 32) + DEPTH + 3;

    struct message {
        uint8_t am_id;
        std::vector<uint8_t> data;
    };

    struct pending_req {
        uct_pending_req_t uct;
        test_cxl_fifo *test;
        uint64_t counter; /* message sent once the request runs */
        unsigned calls;
        unsigned purged;
    };

    struct bcopy_arg {
        uint64_t counter;
        size_t length;
    };

    test_cxl_fifo() :
        m_fd(-1),
        m_md_config(NULL),
        m_md(NULL),
        m_async(NULL),
        m_worker(NULL),
        m_iface(NULL),
        m_ep(NULL),
        m_reply_in_handler(false)
    {
    }

    virtual void init()
    {
        ucs::test::init();

        uct_component_h component = find_component();
        if (component == NULL) {
            UCS_TEST_SKIP_R("cxl component not found");
        }

        /* cleanup() only runs after a successful init() */
        try {
            open_resources(component);
        } catch (...) {
            release_resources();
            throw;
        }
    }

    virtual void cleanup()
    {
        release_resources();
        ucs::test::cleanup();
    }

    void open_resources(uct_component_h component)
    {
        char path[] = "/tmp/uct_cxl_fifo_XXXXXX";
        m_fd        = mkstemp(path);
        ASSERT_GE(m_fd, 0) << strerror(errno);
        m_path = path;
        ASSERT_EQ(0, ftruncate(m_fd, FILE_SIZE)) << strerror(errno);

        ASSERT_UCS_OK(uct_md_config_read(component, NULL, NULL, &m_md_config));
        modify_md_config("CXL_ACCEL_DEV", m_path);
        modify_md_config("CXL_LOCAL_DEV", m_path);
        modify_md_config("CXL_APERTURE_SIZE", ucs::to_string(FILE_SIZE));
        ASSERT_UCS_OK(uct_md_open(component, "cxl", m_md_config, &m_md));

        ASSERT_UCS_OK(ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK,
                                               &m_async));
        ASSERT_UCS_OK(
                uct_worker_create(m_async, UCS_THREAD_MODE_SINGLE, &m_worker));

        uct_iface_config_t *iface_config;
        ASSERT_UCS_OK(uct_md_iface_config_read(m_md, "cxl", NULL, NULL,
                                               &iface_config));
        uct_iface_params_t iface_params = {};
        iface_params.field_mask         = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                                  UCT_IFACE_PARAM_FIELD_DEVICE;
        iface_params.open_mode            = UCT_IFACE_OPEN_MODE_DEVICE;
        iface_params.mode.device.tl_name  = "cxl";
        iface_params.mode.device.dev_name = "cxl0";
        ucs_status_t status = uct_iface_open(m_md, m_worker, &iface_params,
                                             iface_config, &m_iface);
        uct_config_release(iface_config);
        ASSERT_UCS_OK(status);

        ASSERT_UCS_OK(
                uct_iface_set_am_handler(m_iface, AM_ID, am_handler, this, 0));
        ASSERT_UCS_OK(uct_iface_set_am_handler(m_iface, AM_ID_REPLY, am_handler,
                                               this, 0));

        uct_cxl_iface_addr_t addr;
        ASSERT_UCS_OK(
                uct_iface_get_address(m_iface, (uct_iface_addr_t *)&addr));
        uct_ep_params_t ep_params = {};
        ep_params.field_mask      = UCT_EP_PARAM_FIELD_IFACE |
                               UCT_EP_PARAM_FIELD_IFACE_ADDR;
        ep_params.iface      = m_iface;
        ep_params.iface_addr = (const uct_iface_addr_t *)&addr;
        ASSERT_UCS_OK(uct_ep_create(&ep_params, &m_ep));
    }

    void release_resources()
    {
        if (m_ep != NULL) {
            /* The iface and EP destructors assert the pending queue is empty */
            uct_ep_pending_purge(m_ep, purge_cb, NULL);
            uct_ep_destroy(m_ep);
        }
        if (m_iface != NULL) {
            uct_iface_close(m_iface);
        }
        if (m_worker != NULL) {
            uct_worker_destroy(m_worker);
        }
        if (m_async != NULL) {
            ucs_async_context_destroy(m_async);
        }
        if (m_md != NULL) {
            uct_md_close(m_md);
        }
        if (m_md_config != NULL) {
            uct_config_release(m_md_config);
        }
        if (m_fd >= 0) {
            close(m_fd);
            unlink(m_path.c_str());
        }

        m_ep        = NULL;
        m_iface     = NULL;
        m_worker    = NULL;
        m_async     = NULL;
        m_md        = NULL;
        m_md_config = NULL;
        m_fd        = -1;
    }

    static uct_component_h find_component()
    {
        uct_component_h *components;
        uct_component_h found = NULL;
        unsigned num_components;

        if (uct_query_components(&components, &num_components) != UCS_OK) {
            return NULL;
        }

        for (unsigned i = 0; i < num_components; ++i) {
            uct_component_attr_t attr;
            attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
            if ((uct_component_query(components[i], &attr) == UCS_OK) &&
                !strcmp(attr.name, "cxl")) {
                found = components[i];
            }
        }

        uct_release_component_list(components);
        return found;
    }

    void modify_md_config(const std::string &name, const std::string &value)
    {
        ASSERT_UCS_OK(
                uct_config_modify(m_md_config, name.c_str(), value.c_str()));
    }

    uct_cxl_iface_t *cxl_iface()
    {
        return ucs_derived_of(m_iface, uct_cxl_iface_t);
    }

    uct_cxl_fifo_elem_t *slot(uint64_t pos)
    {
        return &cxl_iface()->rx.fifo[pos % DEPTH];
    }

    uint64_t *ack(uint64_t pos)
    {
        return &cxl_iface()->local_ctrl->ack[pos % DEPTH];
    }

    /*
     * Put the stream at 'pos' as if positions [pos - DEPTH, pos) had been
     * sent, received and acked: tx.head == rx.tail == pos, every slot holds the
     * previous lap's seq, and every ack frees its slot for this lap.
     */
    void set_stream_position(uint64_t pos)
    {
        ASSERT_GE(pos, DEPTH);
        for (uint64_t k = pos - DEPTH; k < pos; ++k) {
            uct_cxl_io_write64(&slot(k)->seq, k + 1);
            uct_cxl_io_write64(ack(k), k + 1);
        }
        cxl_iface()->tx.head = pos;
        cxl_iface()->rx.tail = pos;
    }

    static uint8_t pattern(uint64_t counter, size_t i)
    {
        return (uint8_t)(counter * 13 + i * 31 + 7);
    }

    /* am_short payload is 'length' pattern bytes after a 'counter' header */
    ucs_status_t
    send_short(uint64_t counter, unsigned length = 16, uint8_t am_id = AM_ID)
    {
        std::vector<uint8_t> payload(length);

        for (size_t i = 0; i < length; ++i) {
            payload[i] = pattern(counter, i);
        }
        return uct_ep_am_short(m_ep, am_id, counter, payload.data(), length);
    }

    /* am_bcopy payload is 'length' bytes: 'counter', then pattern bytes */
    static size_t pack_cb(void *dest, void *arg)
    {
        const bcopy_arg *barg = (const bcopy_arg *)arg;
        uint8_t *p            = (uint8_t *)dest;

        memcpy(p, &barg->counter, sizeof(barg->counter));
        for (size_t i = sizeof(barg->counter); i < barg->length; ++i) {
            p[i] = pattern(barg->counter, i - sizeof(barg->counter));
        }
        return barg->length;
    }

    ssize_t send_bcopy(uint64_t counter, size_t length)
    {
        bcopy_arg arg = {counter, length};

        return uct_ep_am_bcopy(m_ep, AM_ID, pack_cb, &arg, 0);
    }

    static ucs_status_t
    am_handler(void *arg, void *data, size_t length, unsigned flags)
    {
        test_cxl_fifo *self = (test_cxl_fifo *)arg;
        const uint8_t *p    = (const uint8_t *)data;
        message msg;
        uint64_t counter;

        memcpy(&counter, data, sizeof(counter));
        /* Distinguish the two ids by the header the reply carries */
        msg.am_id = (counter & (1ull << 63)) ? AM_ID_REPLY : AM_ID;
        msg.data.assign(p, p + length);
        self->m_received.push_back(msg);

        if (self->m_reply_in_handler && (msg.am_id == AM_ID)) {
            self->m_reply_in_handler = false;
            EXPECT_UCS_OK(
                    self->send_short(counter | (1ull << 63), 8, AM_ID_REPLY));
        }
        return UCS_OK;
    }

    static ucs_status_t pending_cb(uct_pending_req_t *uct_req)
    {
        pending_req *req = ucs_container_of(uct_req, pending_req, uct);

        ++req->calls;
        return req->test->send_short(req->counter);
    }

    static void purge_cb(uct_pending_req_t *uct_req, void *arg)
    {
        pending_req *req = ucs_container_of(uct_req, pending_req, uct);

        ++req->purged;
    }

    void init_pending(pending_req *req, uint64_t counter)
    {
        req->uct.func = pending_cb;
        req->test     = this;
        req->counter  = counter;
        req->calls    = 0;
        req->purged   = 0;
    }

    unsigned progress()
    {
        return uct_iface_progress(m_iface);
    }

    /* Send DEPTH messages counter, counter + 1, ... without progress */
    void fill(uint64_t counter)
    {
        for (uint64_t i = 0; i < DEPTH; ++i) {
            ASSERT_UCS_OK(send_short(counter + i), << "i=" << i);
        }
    }

    /* m_received[idx] must be the am_short sent with 'counter' and 'length' */
    void check_short(size_t idx, uint64_t counter, unsigned length = 16)
    {
        ASSERT_LT(idx, m_received.size());
        const message &msg = m_received[idx];
        uint64_t header;

        ASSERT_EQ(sizeof(header) + length, msg.data.size()) << "idx=" << idx;
        memcpy(&header, msg.data.data(), sizeof(header));
        EXPECT_EQ(counter, header) << "idx=" << idx;
        for (size_t i = 0; i < length; ++i) {
            ASSERT_EQ(pattern(counter, i), msg.data[sizeof(header) + i])
                    << "idx=" << idx << " byte=" << i;
        }
    }

    /* Send 'count' messages one at a time, each received before the next */
    void run_one_at_a_time(uint64_t count)
    {
        for (uint64_t i = 0; i < count; ++i) {
            ASSERT_UCS_OK(send_short(i));
            ASSERT_EQ(1u, progress()) << "i=" << i;
            check_short(i, i);
        }
    }

    /* Send 'laps' full FIFOs, receiving each in a single progress call */
    void run_laps(uint64_t laps)
    {
        for (uint64_t lap = 0; lap < laps; ++lap) {
            fill(lap * DEPTH);
            ASSERT_EQ(UCS_ERR_NO_RESOURCE, send_short(0));
            ASSERT_EQ(DEPTH, progress()) << "lap=" << lap;
        }

        ASSERT_EQ(laps * DEPTH, m_received.size());
        for (uint64_t i = 0; i < laps * DEPTH; ++i) {
            check_short(i, i);
        }
    }

    /* The slots and acks of the last lap before the current position */
    void check_last_lap()
    {
        uint64_t pos = cxl_iface()->tx.head;

        ASSERT_EQ(pos, cxl_iface()->rx.tail);
        for (uint64_t k = pos - DEPTH; k < pos; ++k) {
            EXPECT_EQ(k + 1, uct_cxl_io_read64(&slot(k)->seq)) << "k=" << k;
            EXPECT_EQ(k + 1, uct_cxl_io_read64(ack(k))) << "k=" << k;
        }
    }

    int m_fd;
    std::string m_path;
    uct_md_config_t *m_md_config;
    uct_md_h m_md;
    ucs_async_context_t *m_async;
    uct_worker_h m_worker;
    uct_iface_h m_iface;
    uct_ep_h m_ep;
    std::vector<message> m_received;
    bool m_reply_in_handler;
};

/* Definitions for members gtest's assertions bind by reference */
const uint8_t test_cxl_fifo::AM_ID;
const uint8_t test_cxl_fifo::AM_ID_REPLY;
const uint64_t test_cxl_fifo::DEPTH;
const uint64_t test_cxl_fifo::HIGH_POS;

UCS_TEST_F(test_cxl_fifo, am_short) {
    const unsigned max_length = UCT_CXL_FIFO_PAYLOAD_SIZE - sizeof(uint64_t);

    ASSERT_UCS_OK(send_short(1, 0));
    ASSERT_UCS_OK(send_short(2, 16));
    ASSERT_UCS_OK(send_short(3, max_length));
    EXPECT_EQ(0u, m_received.size());

    EXPECT_EQ(3u, progress());
    ASSERT_EQ(3u, m_received.size());
    check_short(0, 1, 0);
    check_short(1, 2, 16);
    check_short(2, 3, max_length);
    EXPECT_EQ(0u, progress());
}

UCS_TEST_F(test_cxl_fifo, am_bcopy) {
    const size_t lengths[] = {sizeof(uint64_t), 100, UCT_CXL_FIFO_PAYLOAD_SIZE};

    for (size_t i = 0; i < ucs_static_array_size(lengths); ++i) {
        EXPECT_EQ((ssize_t)lengths[i], send_bcopy(i, lengths[i]));
    }

    EXPECT_EQ(ucs_static_array_size(lengths), progress());
    ASSERT_EQ(ucs_static_array_size(lengths), m_received.size());
    for (size_t i = 0; i < ucs_static_array_size(lengths); ++i) {
        const message &msg = m_received[i];
        uint64_t counter;

        ASSERT_EQ(lengths[i], msg.data.size());
        memcpy(&counter, msg.data.data(), sizeof(counter));
        EXPECT_EQ(i, counter);
        for (size_t j = sizeof(counter); j < lengths[i]; ++j) {
            ASSERT_EQ(pattern(i, j - sizeof(counter)), msg.data[j])
                    << "i=" << i << " byte=" << j;
        }
    }
}

UCS_TEST_F(test_cxl_fifo, wraparound_one_at_a_time) {
    run_one_at_a_time(3 * DEPTH + 7);
    EXPECT_EQ(3 * DEPTH + 7, cxl_iface()->tx.head);
    check_last_lap();
}

UCS_TEST_F(test_cxl_fifo, wraparound_laps) {
    run_laps(4);
    EXPECT_EQ(4 * DEPTH, cxl_iface()->tx.head);
    check_last_lap();
}

/* Sequence numbers and acks must compare all 64 bits across 2^32 */
UCS_TEST_F(test_cxl_fifo, wraparound_2_32) {
    const uint64_t start = (1ull << 32) - DEPTH / 2;

    set_stream_position(start);
    run_one_at_a_time(DEPTH + 7);
    EXPECT_EQ(start + DEPTH + 7, cxl_iface()->tx.head);

    m_received.clear();
    set_stream_position(start);
    run_laps(3);
    EXPECT_EQ(start + 3 * DEPTH, cxl_iface()->tx.head);
    check_last_lap();
}

UCS_TEST_F(test_cxl_fifo, full_fifo_pending) {
    pending_req req;

    init_pending(&req, DEPTH);
    /* A free slot means the caller should send instead */
    EXPECT_EQ(UCS_ERR_BUSY, uct_ep_pending_add(m_ep, &req.uct, 0));

    fill(0);
    EXPECT_EQ(UCS_ERR_NO_RESOURCE, send_short(DEPTH));
    EXPECT_EQ((ssize_t)UCS_ERR_NO_RESOURCE, send_bcopy(DEPTH, 16));
    ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &req.uct, 0));
    EXPECT_EQ(0u, req.calls);

    /* Draining the FIFO acks every slot, then the pending request sends */
    EXPECT_EQ(DEPTH, progress());
    EXPECT_EQ(DEPTH, m_received.size());
    EXPECT_EQ(1u, req.calls);
    EXPECT_EQ(DEPTH + 1, cxl_iface()->tx.head);
    EXPECT_TRUE(ucs_queue_is_empty(&cxl_iface()->tx.pending));

    EXPECT_EQ(1u, progress());
    ASSERT_EQ(DEPTH + 1, m_received.size());
    for (uint64_t i = 0; i <= DEPTH; ++i) {
        check_short(i, i);
    }
    EXPECT_EQ(1u, req.calls);
    EXPECT_EQ(0u, req.purged);
}

/* A free slot must not let a new send overtake a queued pending request */
UCS_TEST_F(test_cxl_fifo, pending_no_overtake) {
    pending_req req;

    init_pending(&req, DEPTH);
    fill(0);
    ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &req.uct, 0));

    /* Free slot 0 as the peer would, without running progress */
    uct_cxl_io_write64(ack(0), 1);
    EXPECT_TRUE(uct_cxl_iface_tx_slot_free(cxl_iface()));
    EXPECT_EQ(UCS_ERR_NO_RESOURCE, send_short(DEPTH + 1));
    EXPECT_EQ((ssize_t)UCS_ERR_NO_RESOURCE, send_bcopy(DEPTH + 1, 16));
    EXPECT_EQ(0u, req.calls);

    EXPECT_EQ(DEPTH, progress());
    EXPECT_EQ(1u, req.calls);
    ASSERT_UCS_OK(send_short(DEPTH + 1));
    EXPECT_EQ(2u, progress());
    ASSERT_EQ(DEPTH + 2, m_received.size());
    for (uint64_t i = 0; i < DEPTH + 2; ++i) {
        check_short(i, i);
    }
}

UCS_TEST_F(test_cxl_fifo, pending_purge) {
    pending_req reqs[2];

    fill(0);
    for (size_t i = 0; i < 2; ++i) {
        init_pending(&reqs[i], DEPTH + i);
        ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &reqs[i].uct, 0));
    }

    uct_ep_pending_purge(m_ep, purge_cb, NULL);
    EXPECT_TRUE(ucs_queue_is_empty(&cxl_iface()->tx.pending));
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(1u, reqs[i].purged) << "i=" << i;
        EXPECT_EQ(0u, reqs[i].calls) << "i=" << i;
    }

    /* Purged requests never run */
    EXPECT_EQ(DEPTH, progress());
    EXPECT_EQ(DEPTH, m_received.size());
    EXPECT_EQ(0u, reqs[0].calls);
    EXPECT_EQ(0u, reqs[1].calls);
}

/*
 * A slot whose contents are written but whose seq isn't (yet) tail + 1 must
 * not be consumed: nothing delivered, no ack, tail unchanged. Once the right
 * seq lands, the message goes through.
 */
UCS_TEST_F(test_cxl_fifo, half_written_seq) {
    const uint64_t pos        = HIGH_POS;
    const uint64_t expected   = pos + 1;
    const uint64_t bad_seqs[] = {
            0, /* never written */
            pos + 1 - DEPTH, /* previous lap */
            expected & 0xffffffffull, /* low half only */
            expected & ~0xffffffffull, /* high half only */
            expected + DEPTH /* next lap */
    };
    const uint32_t length = 24;
    uct_cxl_fifo_elem_t *elem;
    uint8_t payload[length];
    uint64_t ack_before;

    set_stream_position(pos);
    elem       = slot(pos);
    ack_before = uct_cxl_io_read64(ack(pos));

    for (size_t i = 0; i < length; ++i) {
        payload[i] = pattern(pos, i);
    }
    uct_cxl_io_write(elem->payload, payload, length);
    uct_cxl_io_write64(&elem->length, length | ((uint64_t)AM_ID << 32));

    for (size_t i = 0; i < ucs_static_array_size(bad_seqs); ++i) {
        uct_cxl_io_write64(&elem->seq, bad_seqs[i]);
        EXPECT_EQ(0u, progress()) << "seq=" << bad_seqs[i];
        EXPECT_EQ(0u, m_received.size()) << "seq=" << bad_seqs[i];
        EXPECT_EQ(pos, cxl_iface()->rx.tail) << "seq=" << bad_seqs[i];
        EXPECT_EQ(ack_before, uct_cxl_io_read64(ack(pos)))
                << "seq=" << bad_seqs[i];
    }

    uct_cxl_io_write64(&elem->seq, expected);
    EXPECT_EQ(1u, progress());
    ASSERT_EQ(1u, m_received.size());
    ASSERT_EQ(length, m_received[0].data.size());
    EXPECT_EQ(0, memcmp(payload, m_received[0].data.data(), length));
    EXPECT_EQ(pos + 1, cxl_iface()->rx.tail);
    EXPECT_EQ(expected, uct_cxl_io_read64(ack(pos)));
}

/* A slot whose ack isn't exactly the previous lap's seq is still in use */
UCS_TEST_F(test_cxl_fifo, half_written_ack) {
    const uint64_t pos        = HIGH_POS;
    const uint64_t expected   = pos + 1 - DEPTH;
    const uint64_t bad_acks[] = {
            0, /* never written */
            expected - DEPTH, /* two laps ago */
            expected & 0xffffffffull, /* low half only */
            expected & ~0xffffffffull /* high half only */
    };
    pending_req req;

    set_stream_position(pos);
    init_pending(&req, 0);

    for (size_t i = 0; i < ucs_static_array_size(bad_acks); ++i) {
        uct_cxl_io_write64(ack(pos), bad_acks[i]);
        EXPECT_EQ(UCS_ERR_NO_RESOURCE, send_short(0)) << "ack=" << bad_acks[i];
        EXPECT_EQ((ssize_t)UCS_ERR_NO_RESOURCE, send_bcopy(0, 16))
                << "ack=" << bad_acks[i];
        EXPECT_EQ(pos, cxl_iface()->tx.head) << "ack=" << bad_acks[i];
        EXPECT_EQ(pos + 1 - DEPTH, uct_cxl_io_read64(&slot(pos)->seq))
                << "ack=" << bad_acks[i];
    }

    ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &req.uct, 0));
    EXPECT_EQ(0u, progress());
    EXPECT_EQ(0u, req.calls);

    uct_cxl_io_write64(ack(pos), expected);
    /* Nothing to receive yet; the pending request sends into the slot */
    EXPECT_EQ(0u, progress());
    EXPECT_EQ(1u, req.calls);
    EXPECT_EQ(1u, progress());
    ASSERT_EQ(1u, m_received.size());
    check_short(0, 0);
}

/* An AM handler may send, since progress releases the slot before the call */
UCS_TEST_F(test_cxl_fifo, send_from_handler) {
    m_reply_in_handler = true;

    /* In loopback the reply lands in the next slot, so the same progress
     * call picks it up */
    ASSERT_UCS_OK(send_short(1));
    EXPECT_EQ(2u, progress());
    ASSERT_EQ(2u, m_received.size());
    EXPECT_EQ(AM_ID, m_received[0].am_id);
    check_short(0, 1);
    EXPECT_EQ(AM_ID_REPLY, m_received[1].am_id);
    EXPECT_EQ(2u, cxl_iface()->rx.tail);
    EXPECT_EQ(0u, progress());
}

/* With a full FIFO, the per-call DEPTH limit leaves the reply for the next
 * progress call */
UCS_TEST_F(test_cxl_fifo, send_from_handler_full) {
    fill(0);
    m_reply_in_handler = true;

    /* The first message's slot is acked before its handler runs */
    EXPECT_EQ(DEPTH, progress());
    EXPECT_EQ(DEPTH, m_received.size());
    EXPECT_EQ(1u, progress());
    ASSERT_EQ(DEPTH + 1, m_received.size());
    EXPECT_EQ(AM_ID_REPLY, m_received[DEPTH].am_id);
}
