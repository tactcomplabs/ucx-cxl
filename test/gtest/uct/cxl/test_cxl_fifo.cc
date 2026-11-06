/*
 * Copyright (c) Tactical Computing Laboratories, LLC. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
extern "C" {
#include <uct/api/uct.h>
#include <uct/cxl/cxl_iface.h>
}

/*
 * Single-process tests of the AM FIFO (send, progress, pending) and of memdev
 * slots. Every MD's local device and its aperture map the same temp file, so
 * the file plays both guests' memdevs. The primary iface's EP talks to itself:
 * its tx ring and its rx ring from its own slot are the same elements, and the
 * ack row it writes is the one its own send path reads. Every message sent is
 * the next one the iface's own progress receives. White-box access to
 * uct_cxl_iface_t lets the tests start the stream anywhere and plant
 * half-written elements and acks.
 *
 * Further MDs opened on the same file take the other slots, which lets tests
 * send from several slots into one.
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

    /* An MD and, optionally, an iface on it and an EP from that iface */
    struct entity {
        uct_md_h md;
        uct_iface_h iface;
        uct_ep_h ep;
    };

    typedef std::map<std::string, std::string> config_map;

    test_cxl_fifo() :
        m_fd(-1),
        m_component(NULL),
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

        m_component = find_component();
        if (m_component == NULL) {
            UCS_TEST_SKIP_R("cxl component not found");
        }

        /* cleanup() only runs after a successful init() */
        try {
            open_resources();
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

    void open_resources()
    {
        char path[] = "/tmp/uct_cxl_fifo_XXXXXX";
        m_fd        = mkstemp(path);
        ASSERT_GE(m_fd, 0) << strerror(errno);
        m_path = path;
        ASSERT_EQ(0, ftruncate(m_fd, FILE_SIZE)) << strerror(errno);

        ASSERT_UCS_OK(open_md(config_map(), &m_md));

        ASSERT_UCS_OK(ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK,
                                               &m_async));
        ASSERT_UCS_OK(
                uct_worker_create(m_async, UCS_THREAD_MODE_SINGLE, &m_worker));

        m_iface = open_iface(m_md);
        ASSERT_UCS_OK(
                uct_iface_set_am_handler(m_iface, AM_ID, am_handler, this, 0));
        ASSERT_UCS_OK(uct_iface_set_am_handler(m_iface, AM_ID_REPLY, am_handler,
                                               this, 0));
        m_ep = create_ep(m_iface, m_iface);
    }

    static void destroy_ep(uct_ep_h ep)
    {
        /* The iface and EP destructors assert the pending queue is empty */
        uct_ep_pending_purge(ep, purge_cb, NULL);
        uct_ep_destroy(ep);
    }

    void release_entity(entity &e)
    {
        if (e.ep != NULL) {
            destroy_ep(e.ep);
        }
        if (e.iface != NULL) {
            uct_iface_close(e.iface);
        }
        if (e.md != NULL) {
            uct_md_close(e.md);
        }
        e.ep    = NULL;
        e.iface = NULL;
        e.md    = NULL;
    }

    void release_resources()
    {
        for (size_t i = m_entities.size(); i > 0; --i) {
            release_entity(m_entities[i - 1]);
        }
        m_entities.clear();

        if (m_ep != NULL) {
            destroy_ep(m_ep);
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
        if (m_fd >= 0) {
            close(m_fd);
            unlink(m_path.c_str());
        }

        m_ep     = NULL;
        m_iface  = NULL;
        m_worker = NULL;
        m_async  = NULL;
        m_md     = NULL;
        m_fd     = -1;
    }

    /* Open an MD on the temp file, with 'extra' MD config on top */
    ucs_status_t open_md(const config_map &extra, uct_md_h *md_p)
    {
        uct_md_config_t *md_config;
        config_map config = extra;
        ucs_status_t status;

        config.insert(std::make_pair("CXL_ACCEL_DEV", m_path));
        config.insert(std::make_pair("CXL_LOCAL_DEV", m_path));
        config.insert(
                std::make_pair("CXL_APERTURE_SIZE", ucs::to_string(FILE_SIZE)));

        status = uct_md_config_read(m_component, NULL, NULL, &md_config);
        if (status != UCS_OK) {
            return status;
        }

        for (auto it = config.begin(); it != config.end(); ++it) {
            status = uct_config_modify(md_config, it->first.c_str(),
                                       it->second.c_str());
            if (status != UCS_OK) {
                uct_config_release(md_config);
                return status;
            }
        }

        status = uct_md_open(m_component, "cxl", md_config, md_p);
        uct_config_release(md_config);
        return status;
    }

    uct_iface_h open_iface(uct_md_h md)
    {
        uct_iface_config_t *iface_config;
        uct_iface_h iface = NULL;

        EXPECT_UCS_OK(
                uct_md_iface_config_read(md, "cxl", NULL, NULL, &iface_config));
        uct_iface_params_t iface_params   = {};
        iface_params.field_mask           = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                                            UCT_IFACE_PARAM_FIELD_DEVICE;
        iface_params.open_mode            = UCT_IFACE_OPEN_MODE_DEVICE;
        iface_params.mode.device.tl_name  = "cxl";
        iface_params.mode.device.dev_name = "cxl0";
        ucs_status_t status = uct_iface_open(md, m_worker, &iface_params,
                                             iface_config, &iface);
        uct_config_release(iface_config);
        EXPECT_UCS_OK(status);
        return iface;
    }

    struct addresses {
        uct_cxl_device_addr_t dev;
        uct_cxl_iface_addr_t iface;
    };

    static addresses get_addresses(uct_iface_h iface)
    {
        addresses addrs;

        EXPECT_UCS_OK(
                uct_iface_get_device_address(iface,
                                             (uct_device_addr_t *)&addrs.dev));
        EXPECT_UCS_OK(
                uct_iface_get_address(iface, (uct_iface_addr_t *)&addrs.iface));
        return addrs;
    }

    /* EP on 'from' that sends to 'to' */
    static uct_ep_h create_ep(uct_iface_h from, uct_iface_h to)
    {
        addresses addrs = get_addresses(to);
        uct_ep_h ep     = NULL;

        uct_ep_params_t ep_params = {};
        ep_params.field_mask      = UCT_EP_PARAM_FIELD_IFACE |
                                    UCT_EP_PARAM_FIELD_DEV_ADDR |
                                    UCT_EP_PARAM_FIELD_IFACE_ADDR;
        ep_params.iface           = from;
        ep_params.dev_addr        = (const uct_device_addr_t *)&addrs.dev;
        ep_params.iface_addr      = (const uct_iface_addr_t *)&addrs.iface;
        EXPECT_UCS_OK(uct_ep_create(&ep_params, &ep));
        return ep;
    }

    /* Open another MD on the file, with an iface and an EP to the primary
     * iface if 'with_iface' */
    entity &add_entity(const config_map &extra, bool with_iface)
    {
        entity e = {NULL, NULL, NULL};

        EXPECT_UCS_OK(open_md(extra, &e.md));
        if ((e.md != NULL) && with_iface) {
            e.iface = open_iface(e.md);
            if (e.iface != NULL) {
                e.ep = create_ep(e.iface, m_iface);
            }
        }

        m_entities.push_back(e);
        return m_entities.back();
    }

    static unsigned md_slot(uct_md_h md)
    {
        return ucs_derived_of(md, uct_cxl_md_t)->slot;
    }

    unsigned my_slot()
    {
        return md_slot(m_md);
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

    uct_cxl_iface_t *cxl_iface()
    {
        return ucs_derived_of(m_iface, uct_cxl_iface_t);
    }

    uint64_t &tx_head()
    {
        return cxl_iface()->tx[my_slot()].head;
    }

    uint64_t &rx_tail()
    {
        return cxl_iface()->rx[my_slot()].tail;
    }

    ucs_queue_head_t *tx_pending()
    {
        return &cxl_iface()->tx[my_slot()].pending;
    }

    /* Element of the loopback ring that stream position 'pos' uses */
    uct_cxl_fifo_elem_t *slot(uint64_t pos)
    {
        return &cxl_iface()->rx[my_slot()].fifo[pos % DEPTH];
    }

    uint64_t *ack(uint64_t pos)
    {
        return &cxl_iface()->local_ctrl->ack[my_slot()][pos % DEPTH];
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
        tx_head() = pos;
        rx_tail() = pos;
    }

    static uint8_t pattern(uint64_t counter, size_t i)
    {
        return (uint8_t)(counter * 13 + i * 31 + 7);
    }

    /* am_short payload is 'length' pattern bytes after a 'counter' header */
    ucs_status_t send_short(uint64_t counter, unsigned length = 16,
                            uint8_t am_id = AM_ID, uct_ep_h ep = NULL)
    {
        std::vector<uint8_t> payload(length);

        for (size_t i = 0; i < length; ++i) {
            payload[i] = pattern(counter, i);
        }
        return uct_ep_am_short((ep == NULL) ? m_ep : ep, am_id, counter,
                               payload.data(), length);
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
        uint64_t pos = tx_head();

        ASSERT_EQ(pos, rx_tail());
        for (uint64_t k = pos - DEPTH; k < pos; ++k) {
            EXPECT_EQ(k + 1, uct_cxl_io_read64(&slot(k)->seq)) << "k=" << k;
            EXPECT_EQ(k + 1, uct_cxl_io_read64(ack(k))) << "k=" << k;
        }
    }

    int m_fd;
    std::string m_path;
    uct_component_h m_component;
    uct_md_h m_md;
    ucs_async_context_t *m_async;
    uct_worker_h m_worker;
    uct_iface_h m_iface;
    uct_ep_h m_ep;
    std::vector<message> m_received;
    bool m_reply_in_handler;
    std::deque<entity> m_entities; /* add_entity() returns references */
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
    EXPECT_EQ(3 * DEPTH + 7, tx_head());
    check_last_lap();
}

UCS_TEST_F(test_cxl_fifo, wraparound_laps) {
    run_laps(4);
    EXPECT_EQ(4 * DEPTH, tx_head());
    check_last_lap();
}

/* Sequence numbers and acks must compare all 64 bits across 2^32 */
UCS_TEST_F(test_cxl_fifo, wraparound_2_32) {
    const uint64_t start = (1ull << 32) - DEPTH / 2;

    set_stream_position(start);
    run_one_at_a_time(DEPTH + 7);
    EXPECT_EQ(start + DEPTH + 7, tx_head());

    m_received.clear();
    set_stream_position(start);
    run_laps(3);
    EXPECT_EQ(start + 3 * DEPTH, tx_head());
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
    EXPECT_EQ(DEPTH + 1, tx_head());
    EXPECT_TRUE(ucs_queue_is_empty(tx_pending()));

    EXPECT_EQ(1u, progress());
    ASSERT_EQ(DEPTH + 1, m_received.size());
    for (uint64_t i = 0; i <= DEPTH; ++i) {
        check_short(i, i);
    }
    EXPECT_EQ(1u, req.calls);
    EXPECT_EQ(0u, req.purged);
}

/* Flush can't complete while sends wait for a free element; the caller
 * queues it behind them like a send */
UCS_TEST_F(test_cxl_fifo, flush_full_fifo) {
    pending_req req;

    EXPECT_UCS_OK(uct_ep_flush(m_ep, 0, NULL));
    EXPECT_UCS_OK(uct_iface_flush(m_iface, 0, NULL));

    fill(0);
    EXPECT_EQ(UCS_ERR_NO_RESOURCE, uct_ep_flush(m_ep, 0, NULL));

    /* With the ring drained but a request still pending */
    init_pending(&req, DEPTH);
    ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &req.uct, 0));
    uct_cxl_io_write64(ack(0), 1); /* free element 0 without progress */
    EXPECT_EQ(UCS_ERR_NO_RESOURCE, uct_ep_flush(m_ep, 0, NULL));

    /* The iface isn't flushed either: UCP completes a worker flush when all
     * ifaces are */
    EXPECT_EQ(UCS_INPROGRESS, uct_iface_flush(m_iface, 0, NULL));

    /* Progress acks the ring and sends the pending request */
    EXPECT_EQ(DEPTH, progress());
    EXPECT_EQ(1u, req.calls);
    EXPECT_UCS_OK(uct_ep_flush(m_ep, 0, NULL));
    EXPECT_UCS_OK(uct_iface_flush(m_iface, 0, NULL));
}

/* A free slot must not let a new send overtake a queued pending request */
UCS_TEST_F(test_cxl_fifo, pending_no_overtake) {
    pending_req req;

    init_pending(&req, DEPTH);
    fill(0);
    ASSERT_UCS_OK(uct_ep_pending_add(m_ep, &req.uct, 0));

    /* Free slot 0 as the peer would, without running progress */
    uct_cxl_io_write64(ack(0), 1);
    EXPECT_TRUE(uct_cxl_iface_tx_slot_free(cxl_iface(), my_slot()));
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
    EXPECT_TRUE(ucs_queue_is_empty(tx_pending()));
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
        EXPECT_EQ(pos, rx_tail()) << "seq=" << bad_seqs[i];
        EXPECT_EQ(ack_before, uct_cxl_io_read64(ack(pos)))
                << "seq=" << bad_seqs[i];
    }

    uct_cxl_io_write64(&elem->seq, expected);
    EXPECT_EQ(1u, progress());
    ASSERT_EQ(1u, m_received.size());
    ASSERT_EQ(length, m_received[0].data.size());
    EXPECT_EQ(0, memcmp(payload, m_received[0].data.data(), length));
    EXPECT_EQ(pos + 1, rx_tail());
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
        EXPECT_EQ(pos, tx_head()) << "ack=" << bad_acks[i];
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
    EXPECT_EQ(2u, rx_tail());
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

/* MDs on one memdev take the free slots in order, and a slot is released
 * when its MD closes */
UCS_TEST_F(test_cxl_fifo, slot_auto_lock) {
    const unsigned nslots = 4; /* the default */
    std::vector<uct_md_h> mds;
    uct_md_h md;

    EXPECT_EQ(0u, my_slot());
    for (unsigned slot = 1; slot < nslots; ++slot) {
        ASSERT_UCS_OK(open_md(config_map(), &md));
        EXPECT_EQ(slot, md_slot(md));
        mds.push_back(md);
    }

    {
        scoped_log_handler wrap_err(wrap_errors_logger);
        EXPECT_EQ(UCS_ERR_BUSY, open_md(config_map(), &md));
    }

    /* Closing the MD in slot 2 frees exactly that slot */
    uct_md_close(mds[1]);
    ASSERT_UCS_OK(open_md(config_map(), &md));
    EXPECT_EQ(2u, md_slot(md));
    mds[1] = md;

    for (size_t i = 0; i < mds.size(); ++i) {
        uct_md_close(mds[i]);
    }
}

UCS_TEST_F(test_cxl_fifo, slot_forced) {
    config_map config;
    uct_md_h md, md2;

    config["CXL_SLOT"] = "3";
    ASSERT_UCS_OK(open_md(config, &md));
    EXPECT_EQ(3u, md_slot(md));

    {
        scoped_log_handler wrap_err(wrap_errors_logger);
        EXPECT_EQ(UCS_ERR_BUSY, open_md(config, &md2));
        config["CXL_SLOT"] = "4"; /* out of range for 4 slots */
        EXPECT_EQ(UCS_ERR_INVALID_PARAM, open_md(config, &md2));
    }

    uct_md_close(md);
}

/*
 * Two slots send into the primary iface's slot concurrently. Each sender has
 * its own ring, so each stream stays in order, and one sender's full ring
 * doesn't block the other.
 */
UCS_TEST_F(test_cxl_fifo, two_senders) {
    const uint64_t tag[] = {1ull << 48, 2ull << 48};
    std::vector<uint64_t> next(2, 0);
    entity *senders[2];

    for (unsigned i = 0; i < 2; ++i) {
        senders[i] = &add_entity(config_map(), true);
        ASSERT_TRUE(senders[i]->ep != NULL);
        EXPECT_EQ(i + 1, md_slot(senders[i]->md));
    }

    /* Interleave sends until the first sender's ring is full */
    for (uint64_t k = 0; k < DEPTH; ++k) {
        ASSERT_UCS_OK(send_short(tag[0] + k, 16, AM_ID, senders[0]->ep));
        if (k < DEPTH / 2) {
            ASSERT_UCS_OK(send_short(tag[1] + k, 16, AM_ID, senders[1]->ep));
        }
    }
    EXPECT_EQ(UCS_ERR_NO_RESOURCE,
              send_short(tag[0] + DEPTH, 16, AM_ID, senders[0]->ep));
    ASSERT_UCS_OK(send_short(tag[1] + DEPTH / 2, 16, AM_ID, senders[1]->ep));

    /* Progress takes at most DEPTH elements per call, across all rings */
    while (progress() > 0) {
    }
    ASSERT_EQ(DEPTH + DEPTH / 2 + 1, m_received.size());

    for (size_t i = 0; i < m_received.size(); ++i) {
        uint64_t header;
        unsigned sender;

        memcpy(&header, m_received[i].data.data(), sizeof(header));
        sender = (header >> 48) - 1;
        ASSERT_LT(sender, 2u) << "i=" << i;
        check_short(i, tag[sender] + next[sender]);
        ++next[sender];
    }
    EXPECT_EQ(DEPTH, next[0]);
    EXPECT_EQ(DEPTH / 2 + 1, next[1]);

    /* The acks freed the first sender's ring */
    EXPECT_UCS_OK(send_short(tag[0] + DEPTH, 16, AM_ID, senders[0]->ep));
    EXPECT_EQ(1u, progress());
}

/* Same-node peers are unreachable; a peer on another node is reachable once
 * its control block matches its address */
UCS_TEST_F(test_cxl_fifo, reachability) {
    config_map config;
    char info[256];

    config["CXL_NODE_ID"] = "12345";
    entity &other         = add_entity(config, true);
    ASSERT_TRUE(other.iface != NULL);

    addresses self_addrs  = get_addresses(m_iface);
    addresses other_addrs = get_addresses(other.iface);
    EXPECT_EQ(12345u, other_addrs.dev);
    EXPECT_NE(self_addrs.dev, other_addrs.dev);

    uct_iface_is_reachable_params_t params = {};
    params.field_mask         = UCT_IFACE_IS_REACHABLE_FIELD_DEVICE_ADDR |
                                UCT_IFACE_IS_REACHABLE_FIELD_IFACE_ADDR |
                                UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING |
                                UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING_LENGTH;
    params.info_string        = info;
    params.info_string_length = sizeof(info);

    /* Same node, even the primary itself */
    params.device_addr = (const uct_device_addr_t *)&self_addrs.dev;
    params.iface_addr  = (const uct_iface_addr_t *)&self_addrs.iface;
    EXPECT_FALSE(uct_iface_is_reachable_v2(m_iface, &params));

    params.device_addr = (const uct_device_addr_t *)&other_addrs.dev;
    params.iface_addr  = (const uct_iface_addr_t *)&other_addrs.iface;
    EXPECT_TRUE(uct_iface_is_reachable_v2(m_iface, &params)) << info;

    /* Never intra-node */
    params.field_mask |= UCT_IFACE_IS_REACHABLE_FIELD_SCOPE;
    params.scope = UCT_IFACE_REACHABILITY_SCOPE_DEVICE;
    EXPECT_FALSE(uct_iface_is_reachable_v2(m_iface, &params));
    params.field_mask &= ~UCT_IFACE_IS_REACHABLE_FIELD_SCOPE;

    /* Stale iface id, e.g. the process in that slot restarted */
    uct_cxl_iface_addr_t stale = other_addrs.iface;
    ++stale.id;
    params.iface_addr = (const uct_iface_addr_t *)&stale;
    EXPECT_FALSE(uct_iface_is_reachable_v2(m_iface, &params));

    /* Slot out of range */
    stale      = other_addrs.iface;
    stale.slot = UCT_CXL_MAX_SLOTS;
    EXPECT_FALSE(uct_iface_is_reachable_v2(m_iface, &params));

    /* Once the iface closes, its slot no longer looks initialized */
    params.iface_addr = (const uct_iface_addr_t *)&other_addrs.iface;
    release_entity(other);
    EXPECT_FALSE(uct_iface_is_reachable_v2(m_iface, &params));
}

/* mem_alloc hands out the MD's own slot, and RMA can reach exactly the RMA
 * region of a slot */
UCS_TEST_F(test_cxl_fifo, rma_slot_bounds) {
    entity &other     = add_entity(config_map(), false);
    uct_cxl_md_t *cmd = ucs_derived_of(other.md, uct_cxl_md_t);
    const size_t size = 4096;
    uct_md_mkey_pack_params_t pack_params = {};
    uct_allocated_memory_t mem;
    uint8_t packed[sizeof(uct_cxl_packed_rkey_t)];
    uct_rkey_bundle_t rkey;
    uint64_t offset, value = 0x1122334455667788ull;

    ASSERT_TRUE(other.md != NULL);
    ASSERT_EQ(1u, md_slot(other.md));

    uct_mem_alloc_params_t params = {};
    params.field_mask             = UCT_MEM_ALLOC_PARAM_FIELD_FLAGS |
                                    UCT_MEM_ALLOC_PARAM_FIELD_MDS |
                                    UCT_MEM_ALLOC_PARAM_FIELD_MEM_TYPE |
                                    UCT_MEM_ALLOC_PARAM_FIELD_NAME;
    params.flags                  = UCT_MD_MEM_ACCESS_ALL;
    params.mds.mds                = &other.md;
    params.mds.count              = 1;
    params.mem_type               = UCS_MEMORY_TYPE_HOST;
    params.name                   = "test_cxl_rma";
    uct_alloc_method_t method     = UCT_ALLOC_METHOD_MD;
    ASSERT_UCS_OK(uct_mem_alloc(size, &method, 1, &params, &mem));

    offset = UCS_PTR_BYTE_DIFF(cmd->local_base, mem.address);
    EXPECT_EQ(cmd->slot_size + UCT_CXL_RMA_OFFSET, offset);

    ASSERT_UCS_OK(uct_md_mkey_pack_v2(other.md, mem.memh, mem.address, size,
                                      &pack_params, packed));
    ASSERT_UCS_OK(uct_rkey_unpack(m_component, packed, &rkey));

    /* The primary EP writes into slot 1's RMA region */
    ASSERT_UCS_OK(uct_ep_put_short(m_ep, &value, sizeof(value),
                                   (uintptr_t)mem.address, rkey.rkey));
    EXPECT_EQ(value, *(volatile uint64_t *)mem.address);

    /* Past the end of the segment */
    {
        scoped_log_handler wrap_err(wrap_errors_logger);
        EXPECT_EQ(UCS_ERR_INVALID_PARAM,
                  uct_ep_put_short(m_ep, &value, sizeof(value),
                                   (uintptr_t)mem.address + size - 4,
                                   rkey.rkey));
    }

    /* A forged rkey reaching into the next slot's control block */
    uct_cxl_packed_rkey_t forged = {cmd->slot_size * 2 - 64, 0x1000, 128};
    uct_rkey_bundle_t forged_rkey;
    ASSERT_UCS_OK(uct_rkey_unpack(m_component, &forged, &forged_rkey));
    {
        scoped_log_handler wrap_err(wrap_errors_logger);
        EXPECT_EQ(UCS_ERR_INVALID_PARAM,
                  uct_ep_put_short(m_ep, &value, sizeof(value), 0x1000 + 64,
                                   forged_rkey.rkey));
    }

    uct_rkey_release(m_component, &forged_rkey);
    uct_rkey_release(m_component, &rkey);
    uct_mem_free(&mem);
}
