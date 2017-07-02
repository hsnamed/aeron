/*
 * Copyright 2014-2017 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <array>
#include <cstdint>
#include <thread>
#include <exception>

#include <gtest/gtest.h>
#include <arpa/inet.h>

extern "C"
{
#include "aeron_driver_conductor.h"
#include "util/aeron_error.h"
#include "aeron_driver_sender.h"
#include "aeron_driver_receiver.h"
}

#include "concurrent/ringbuffer/ManyToOneRingBuffer.h"
#include "concurrent/broadcast/CopyBroadcastReceiver.h"
#include "command/PublicationBuffersReadyFlyweight.h"
#include "command/ImageBuffersReadyFlyweight.h"
#include "command/CorrelatedMessageFlyweight.h"
#include "command/PublicationMessageFlyweight.h"
#include "command/SubscriptionMessageFlyweight.h"
#include "command/RemoveMessageFlyweight.h"
#include "command/ImageMessageFlyweight.h"

using namespace aeron::concurrent::broadcast;
using namespace aeron::concurrent::ringbuffer;
using namespace aeron::concurrent;
using namespace aeron;

#define CHANNEL_1 "aeron:udp?endpoint=localhost:40001"
#define CHANNEL_2 "aeron:udp?endpoint=localhost:40002"
#define CHANNEL_3 "aeron:udp?endpoint=localhost:40003"
#define CHANNEL_4 "aeron:udp?endpoint=localhost:40004"

#define STREAM_ID_1 (101)
#define STREAM_ID_2 (102)
#define STREAM_ID_3 (103)
#define STREAM_ID_4 (104)

#define SESSION_ID (0x5E5510)
#define INITIAL_TERM_ID (0x3456)

#define TERM_LENGTH (64 * 1024)

#define SRC_IP_ADDR "127.0.0.1"
#define SRC_UDP_PORT (43657)
#define SOURCE_IDENTITY "127.0.0.1:43657"

#define CONTROL_IP_ADDR "127.0.0.1"
#define CONTROL_UDP_PORT (43657)

static int64_t ms_timestamp = 0;

static int64_t test_nano_clock()
{
    return ms_timestamp * 1000 * 1000;
}

static int64_t test_epoch_clock()
{
    return ms_timestamp;
}

static int test_malloc_map_raw_log(
    aeron_mapped_raw_log_t *log, const char *path, bool use_sparse_file, uint64_t term_length)
{
    uint64_t log_length = AERON_LOGBUFFER_COMPUTE_LOG_LENGTH(term_length);

    log->num_mapped_files = 0;
    log->mapped_files[0].length = 0;
    log->mapped_files[0].addr = malloc(log_length);

    memset(log->mapped_files[0].addr, 0, log_length);

    for (size_t i = 0; i < AERON_LOGBUFFER_PARTITION_COUNT; i++)
    {
        log->term_buffers[i].addr =
            (uint8_t *)log->mapped_files[0].addr + (i * term_length);
        log->term_buffers[i].length = term_length;
    }

    log->log_meta_data.addr =
        (uint8_t *)log->mapped_files[0].addr + (log_length - AERON_LOGBUFFER_META_DATA_LENGTH);
    log->log_meta_data.length = AERON_LOGBUFFER_META_DATA_LENGTH;

    log->term_length = term_length;
    return 0;
}

static int test_malloc_map_raw_log_close(aeron_mapped_raw_log_t *log)
{
    free(log->mapped_files[0].addr);
    return 0;
}

static uint64_t test_uint64_max_usable_fs_space(const char *path)
{
    return UINT64_MAX;
}

struct TestDriverContext
{
    TestDriverContext()
    {
        ms_timestamp = 0; /* single threaded */

        if (aeron_driver_context_init(&m_context) < 0)
        {
            throw std::runtime_error("could not init context: " + std::string(aeron_errmsg()));
        }

        m_context->threading_mode = AERON_THREADING_MODE_SHARED;
        m_context->cnc_map.length = aeron_cnc_length(m_context);
        m_cnc = std::unique_ptr<uint8_t[]>(new uint8_t[m_context->cnc_map.length]);
        m_context->cnc_map.addr = m_cnc.get();

        memset(m_context->cnc_map.addr, 0, m_context->cnc_map.length);

        aeron_driver_fill_cnc_metadata(m_context);

        m_context->term_buffer_length = TERM_LENGTH;
        m_context->ipc_term_buffer_length = TERM_LENGTH;
        m_context->term_buffer_sparse_file = 1;

        /* control time */
        m_context->nano_clock = test_nano_clock;
        m_context->epoch_clock = test_epoch_clock;

        /* control files */
        m_context->usable_fs_space_func = test_uint64_max_usable_fs_space;
        m_context->map_raw_log_func = test_malloc_map_raw_log;
        m_context->map_raw_log_close_func = test_malloc_map_raw_log_close;
    }

    virtual ~TestDriverContext()
    {
        m_context->cnc_map.addr = NULL;
        aeron_driver_context_close(m_context);
    }

    aeron_driver_context_t *m_context = NULL;
    std::unique_ptr<uint8_t[]> m_cnc;
};

struct TestDriverConductor
{
    TestDriverConductor(TestDriverContext &context)
    {
        if (aeron_driver_conductor_init(&m_conductor, context.m_context) < 0)
        {
            throw std::runtime_error("could not init context: " + std::string(aeron_errmsg()));
        }

        if (aeron_driver_sender_init(&m_sender, context.m_context, &m_conductor.system_counters, &m_conductor.error_log) < 0)
        {
            throw std::runtime_error("could not init sender: " + std::string(aeron_errmsg()));
        }

        context.m_context->sender_proxy = &m_sender.sender_proxy;

        if (aeron_driver_receiver_init(&m_receiver, context.m_context, &m_conductor.system_counters, &m_conductor.error_log) < 0)
        {
            throw std::runtime_error("could not init receiver: " + std::string(aeron_errmsg()));
        }

        context.m_context->receiver_proxy = &m_receiver.receiver_proxy;
    }

    virtual ~TestDriverConductor()
    {
        aeron_driver_conductor_on_close(&m_conductor);
    }

    aeron_driver_conductor_t m_conductor;
    aeron_driver_sender_t m_sender;
    aeron_driver_receiver_t m_receiver;
};

class DriverConductorTest : public testing::Test
{
public:

    DriverConductorTest() :
        m_command(m_command_buffer, sizeof(m_command_buffer)),
        m_conductor(m_context),
        m_to_clients_buffer(
            m_context.m_context->to_clients_buffer,
            static_cast<util::index_t>(m_context.m_context->to_clients_buffer_length)),
        m_to_clients_receiver(m_to_clients_buffer),
        m_to_clients_copy_receiver(m_to_clients_receiver),
        m_to_driver_buffer(
            m_context.m_context->to_driver_buffer,
            static_cast<util::index_t >(m_context.m_context->to_driver_buffer_length)),
        m_to_driver(m_to_driver_buffer)
    {
    }

    size_t readAllBroadcastsFromConductor(const handler_t& func)
    {
        size_t num_received = 0;

        while (m_to_clients_copy_receiver.receive(func) > 0)
        {
            num_received++;
        }

        return num_received;
    }

    int64_t nextCorrelationId()
    {
        return m_to_driver.nextCorrelationId();
    }

    inline int writeCommand(int32_t msg_type_id, util::index_t length)
    {
        return m_to_driver.write(msg_type_id, m_command, 0, length) ? 0 : -1;
    }

    int addIpcPublication(int64_t client_id, int64_t correlation_id, int32_t stream_id, bool is_exclusive)
    {
        int32_t msg_type_id = is_exclusive ? AERON_COMMAND_ADD_EXCLUSIVE_PUBLICATION : AERON_COMMAND_ADD_PUBLICATION;
        command::PublicationMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.streamId(stream_id);
        command.channel(AERON_IPC_CHANNEL);

        return writeCommand(msg_type_id, command.length());
    }

    int addNetworkPublication(
        int64_t client_id, int64_t correlation_id, const char *channel, int32_t stream_id, bool is_exclusive)
    {
        int32_t msg_type_id = is_exclusive ? AERON_COMMAND_ADD_EXCLUSIVE_PUBLICATION : AERON_COMMAND_ADD_PUBLICATION;
        command::PublicationMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.streamId(stream_id);
        command.channel(channel);

        return writeCommand(msg_type_id, command.length());
    }

    int removePublication(int64_t client_id, int64_t correlation_id, int64_t registration_id)
    {
        command::RemoveMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.registrationId(registration_id);

        return writeCommand(AERON_COMMAND_REMOVE_PUBLICATION, command.length());
    }

    int addIpcSubscription(int64_t client_id, int64_t correlation_id, int32_t stream_id, int64_t registration_id)
    {
        command::SubscriptionMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.streamId(stream_id);
        command.registrationCorrelationId(registration_id);
        command.channel(AERON_IPC_CHANNEL);

        return writeCommand(AERON_COMMAND_ADD_SUBSCRIPTION, command.length());
    }

    int addNetworkSubscription(
        int64_t client_id, int64_t correlation_id, const char *channel, int32_t stream_id, int64_t registration_id)
    {
        command::SubscriptionMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.streamId(stream_id);
        command.registrationCorrelationId(registration_id);
        command.channel(channel);

        return writeCommand(AERON_COMMAND_ADD_SUBSCRIPTION, command.length());
    }

    int removeSubscription(int64_t client_id, int64_t correlation_id, int64_t registration_id)
    {
        command::RemoveMessageFlyweight command(m_command, 0);

        command.clientId(client_id);
        command.correlationId(correlation_id);
        command.registrationId(registration_id);

        return writeCommand(AERON_COMMAND_REMOVE_SUBSCRIPTION, command.length());
    }

    int clientKeepalive(int64_t client_id)
    {
        command::CorrelatedMessageFlyweight command(m_command, 0);

        command.clientId(client_id);

        return writeCommand(AERON_COMMAND_CLIENT_KEEPALIVE, command::CORRELATED_MESSAGE_LENGTH);
    }

    int doWork()
    {
        return aeron_driver_conductor_do_work(&m_conductor.m_conductor);
    }

    void doWorkUntilTimeNs(int64_t end_ns, int64_t num_increments = 100, std::function<void()> func = [](){})
    {
        int64_t increment = (end_ns - ms_timestamp) / num_increments;

        if (increment <= 0)
        {
            throw std::runtime_error("increment must be positive");
        }

        do
        {
            ms_timestamp += increment;
            func();
            doWork();
        }
        while (ms_timestamp <= end_ns);
    }

    void fill_sockaddr_ipv4(struct sockaddr_storage *addr, const char *ip, unsigned short int port)
    {
        struct sockaddr_in *ipv4addr = (struct sockaddr_in *)addr;

        ipv4addr->sin_family = AF_INET;
        if (inet_pton(AF_INET, ip, &ipv4addr->sin_addr) != 1)
        {
            throw std::runtime_error("can't get IPv4 address");
        }
        ipv4addr->sin_port = htons(port);
    }

    void createPublicationImage(aeron_receive_channel_endpoint_t *endpoint, int64_t position)
    {
        aeron_command_create_publication_image_t cmd;
        size_t position_bits_to_shift = (size_t)aeron_number_of_trailing_zeroes(TERM_LENGTH);

        cmd.base.func = aeron_driver_conductor_on_create_publication_image;
        cmd.base.item = NULL;
        cmd.endpoint = endpoint;
        cmd.session_id = SESSION_ID;
        cmd.stream_id = STREAM_ID_1;
        cmd.term_offset = 0;
        cmd.active_term_id =
            aeron_logbuffer_compute_term_id_from_position(position, position_bits_to_shift, INITIAL_TERM_ID);
        cmd.initial_term_id = INITIAL_TERM_ID;
        cmd.mtu_length = (int32_t)m_context.m_context->mtu_length;
        cmd.term_length = TERM_LENGTH;

        fill_sockaddr_ipv4(&cmd.src_address, SRC_IP_ADDR, SRC_UDP_PORT);
        fill_sockaddr_ipv4(&cmd.control_address, CONTROL_IP_ADDR, CONTROL_UDP_PORT);

        aeron_driver_conductor_on_create_publication_image(&m_conductor.m_conductor, &cmd);
    }

protected:
    uint8_t m_command_buffer[AERON_MAX_PATH];
    AtomicBuffer m_command;
    TestDriverContext m_context;
    TestDriverConductor m_conductor;

    AtomicBuffer m_to_clients_buffer;
    BroadcastReceiver m_to_clients_receiver;
    CopyBroadcastReceiver m_to_clients_copy_receiver;

    AtomicBuffer m_to_driver_buffer;
    ManyToOneRingBuffer m_to_driver;
};

static auto null_handler = [](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
{
};

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);

    doWork();

    aeron_ipc_publication_t *publication =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id);

    ASSERT_NE(publication, (aeron_ipc_publication_t *)NULL);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.correlationId(), pub_id);
            EXPECT_GT(response.logFileName().length(), 0u);
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddAndRemoveSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    ASSERT_EQ(removePublication(client_id, remove_correlation_id, pub_id), 0);
    doWork();
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), remove_correlation_id);
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);

    doWork();

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), sub_id);
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddAndRemoveSingleIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    ASSERT_EQ(removeSubscription(client_id, remove_correlation_id, sub_id), 0);
    doWork();
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), remove_correlation_id);
    };

    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleIpcPublications)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();
    int64_t pub_id_3 = nextCorrelationId();
    int64_t pub_id_4 = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id_1, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_2, STREAM_ID_2, false), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_3, STREAM_ID_3, false), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_4, STREAM_ID_4, false), 0);
    doWork();

    aeron_ipc_publication_t *publication_1 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_1);
    aeron_ipc_publication_t *publication_2 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_2);
    aeron_ipc_publication_t *publication_3 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_3);
    aeron_ipc_publication_t *publication_4 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_4);

    ASSERT_NE(publication_1, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_2, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_3, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_4, (aeron_ipc_publication_t *)NULL);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleExclusiveIpcPublicationsWithSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();
    int64_t pub_id_3 = nextCorrelationId();
    int64_t pub_id_4 = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_2, STREAM_ID_1, true), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_3, STREAM_ID_1, true), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_4, STREAM_ID_1, true), 0);
    doWork();

    aeron_ipc_publication_t *publication_1 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_1);
    aeron_ipc_publication_t *publication_2 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_2);
    aeron_ipc_publication_t *publication_3 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_3);
    aeron_ipc_publication_t *publication_4 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_4);

    ASSERT_NE(publication_1, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_2, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_3, (aeron_ipc_publication_t *)NULL);
    ASSERT_NE(publication_4, (aeron_ipc_publication_t *)NULL);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleIpcSubscriptionsWithSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id_1 = nextCorrelationId();
    int64_t sub_id_2 = nextCorrelationId();
    int64_t sub_id_3 = nextCorrelationId();
    int64_t sub_id_4 = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id_2, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id_3, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id_4, STREAM_ID_1, -1), 0);

    doWork();

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcSubscriptionThenAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();

    aeron_ipc_publication_t *publication =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 1u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        if (0 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

            const command::CorrelatedMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), sub_id);
        }
        else if (1 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id);
            session_id = response.sessionId();

            log_file_name = response.logFileName();
        }
        else if (2 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

            const command::ImageBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.sessionId(), session_id);
            EXPECT_EQ(response.subscriberPositionCount(), 1);

            const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);

            EXPECT_EQ(position.registrationId, sub_id);

            EXPECT_EQ(log_file_name, response.logFileName());
            EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
        }

        response_number++;
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 3u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcPublicationThenAddSingleIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    doWork();

    aeron_ipc_publication_t *publication =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 1u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        if (0 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id);
            session_id = response.sessionId();

            log_file_name = response.logFileName();
        }
        else if (1 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

            const command::CorrelatedMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), sub_id);
        }
        else if (2 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

            command::ImageBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.sessionId(), session_id);
            EXPECT_EQ(response.subscriberPositionCount(), 1);

            const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);

            EXPECT_EQ(position.registrationId, sub_id);

            EXPECT_EQ(log_file_name, response.logFileName());
            EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
        }

        response_number++;
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 3u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleIpcSubscriptionWithSameStreamIdThenAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id_1 = nextCorrelationId();
    int64_t sub_id_2 = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id_2, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();

    aeron_ipc_publication_t *publication =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 2u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        if (0 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

            const command::CorrelatedMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), sub_id_1);
        }
        else if (1 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

            const command::CorrelatedMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), sub_id_2);
        }
        else if (2 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id);
            session_id = response.sessionId();

            log_file_name = response.logFileName();
        }
        else if (3 == response_number || 4 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

            const command::ImageBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.sessionId(), session_id);
            EXPECT_EQ(response.subscriberPositionCount(), 1);

            const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);

            EXPECT_TRUE(position.registrationId == sub_id_1 || position.registrationId == sub_id_2);

            EXPECT_EQ(log_file_name, response.logFileName());
            EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
        }

        response_number++;
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 5u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcSubscriptionThenAddMultipleExclusiveIpcPublicationsWithSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_2, STREAM_ID_1, true), 0);
    doWork();

    aeron_ipc_publication_t *publication_1 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_1);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication_1), 1u);
    aeron_ipc_publication_t *publication_2 =
        aeron_driver_conductor_find_ipc_publication(&m_conductor.m_conductor, pub_id_2);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication_2), 1u);

    size_t response_number = 0;
    int32_t session_id_1 = 0;
    int32_t session_id_2 = 0;
    std::string log_file_name_1;
    std::string log_file_name_2;
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        if (0 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

            const command::CorrelatedMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), sub_id);
        }
        else if (1 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_EXCLUSIVE_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id_1);
            session_id_1 = response.sessionId();

            log_file_name_1 = response.logFileName();
        }
        else if (2 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

            const command::ImageBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.subscriberPositionCount(), 1);

            const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);
            EXPECT_EQ(position.registrationId, sub_id);
            EXPECT_EQ(response.sessionId(), session_id_1);
            EXPECT_EQ(response.correlationId(), pub_id_1);
            EXPECT_EQ(log_file_name_1, response.logFileName());
            EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
        }
        else if (3 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_EXCLUSIVE_PUBLICATION_READY);

            const command::PublicationBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id_2);
            session_id_2 = response.sessionId();

            log_file_name_2 = response.logFileName();
        }
        else if (4 == response_number)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

            const command::ImageBuffersReadyFlyweight response(buffer, offset);

            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.subscriberPositionCount(), 1);

            const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);
            EXPECT_EQ(position.registrationId, sub_id);
            EXPECT_EQ(response.sessionId(), session_id_2);
            EXPECT_EQ(response.correlationId(), pub_id_2);
            EXPECT_EQ(log_file_name_2, response.logFileName());
            EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
        }

        response_number++;
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 5u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    doWorkUntilTimeNs(
        m_context.m_context->publication_linger_timeout_ns +
        (m_context.m_context->client_liveness_timeout_ns * 2));
    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldBeAbleToNotTimeoutIpcPublicationOnKeepalive)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    int64_t timeout =
        m_context.m_context->publication_linger_timeout_ns +
        (m_context.m_context->client_liveness_timeout_ns * 2);

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    doWorkUntilTimeNs(
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2));
    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldBeAbleToNotTimeoutIpcSubscriptionOnKeepalive)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    int64_t timeout =
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2);

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutIpcPublicationWithActiveIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, false), 0);
    doWork();
    ASSERT_EQ(removePublication(client_id, remove_correlation_id, pub_id), 0);
    doWork();
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);

    int64_t timeout = m_context.m_context->publication_linger_timeout_ns * 2;

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_active_ipc_subscriptions(&m_conductor.m_conductor, STREAM_ID_1), 0u);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_UNAVAILABLE_IMAGE);

            const command::ImageMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id);
            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.channel(), AERON_IPC_CHANNEL);
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleNetworkPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id, CHANNEL_1, STREAM_ID_1, false), 0);

    doWork();

    aeron_send_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    ASSERT_NE(endpoint, (aeron_send_channel_endpoint_t *)NULL);

    aeron_network_publication_t *publication =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id);

    ASSERT_NE(publication, (aeron_network_publication_t *)NULL);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

        const command::PublicationBuffersReadyFlyweight response(buffer, offset);

        EXPECT_EQ(response.streamId(), STREAM_ID_1);
        EXPECT_EQ(response.correlationId(), pub_id);
        EXPECT_GT(response.logFileName().length(), 0u);
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddAndRemoveSingleNetworkPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    ASSERT_EQ(removePublication(client_id, remove_correlation_id, pub_id), 0);
    doWork();
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), remove_correlation_id);
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleNetworkSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, -1), 0);

    doWork();

    aeron_receive_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    ASSERT_NE(endpoint, (aeron_receive_channel_endpoint_t *)NULL);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), sub_id);
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddAndRemoveSingleNetworkSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, -1), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    ASSERT_EQ(removeSubscription(client_id, remove_correlation_id, sub_id), 0);
    doWork();
    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_OPERATION_SUCCESS);

        const command::CorrelatedMessageFlyweight response(buffer, offset);

        EXPECT_EQ(response.correlationId(), remove_correlation_id);
    };

    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleNetworkPublications)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();
    int64_t pub_id_3 = nextCorrelationId();
    int64_t pub_id_4 = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id_1, CHANNEL_1, STREAM_ID_1, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_2, CHANNEL_1, STREAM_ID_2, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_3, CHANNEL_1, STREAM_ID_3, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_4, CHANNEL_1, STREAM_ID_4, false), 0);
    doWork();

    aeron_send_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    ASSERT_NE(endpoint, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 1u);

    aeron_network_publication_t *publication_1 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_1);
    aeron_network_publication_t *publication_2 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_2);
    aeron_network_publication_t *publication_3 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_3);
    aeron_network_publication_t *publication_4 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_4);

    ASSERT_NE(publication_1, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_2, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_3, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_4, (aeron_network_publication_t *)NULL);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleNetworkPublicationsDifferentChannelsSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();
    int64_t pub_id_3 = nextCorrelationId();
    int64_t pub_id_4 = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id_1, CHANNEL_1, STREAM_ID_1, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_2, CHANNEL_2, STREAM_ID_1, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_3, CHANNEL_3, STREAM_ID_1, false), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_4, CHANNEL_4, STREAM_ID_1, false), 0);
    doWork();

    aeron_send_channel_endpoint_t *endpoint_1 =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);
    aeron_send_channel_endpoint_t *endpoint_2 =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_2);
    aeron_send_channel_endpoint_t *endpoint_3 =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_3);
    aeron_send_channel_endpoint_t *endpoint_4 =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_4);

    ASSERT_NE(endpoint_1, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_2, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_3, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_4, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 4u);

    aeron_network_publication_t *publication_1 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_1);
    aeron_network_publication_t *publication_2 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_2);
    aeron_network_publication_t *publication_3 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_3);
    aeron_network_publication_t *publication_4 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_4);

    ASSERT_NE(publication_1, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_2, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_3, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_4, (aeron_network_publication_t *)NULL);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleExclusiveNetworkPublicationsWithSameChannelSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();
    int64_t pub_id_3 = nextCorrelationId();
    int64_t pub_id_4 = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id_1, CHANNEL_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_2, CHANNEL_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_3, CHANNEL_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addNetworkPublication(client_id, pub_id_4, CHANNEL_1, STREAM_ID_1, true), 0);
    doWork();

    aeron_send_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_send_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    ASSERT_NE(endpoint, (aeron_send_channel_endpoint_t *)NULL);
    ASSERT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 1u);

    aeron_network_publication_t *publication_1 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_1);
    aeron_network_publication_t *publication_2 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_2);
    aeron_network_publication_t *publication_3 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_3);
    aeron_network_publication_t *publication_4 =
        aeron_driver_conductor_find_network_publication(&m_conductor.m_conductor, pub_id_4);

    ASSERT_NE(publication_1, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_2, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_3, (aeron_network_publication_t *)NULL);
    ASSERT_NE(publication_4, (aeron_network_publication_t *)NULL);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleNetworkSubscriptionsWithSameChannelSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id_1 = nextCorrelationId();
    int64_t sub_id_2 = nextCorrelationId();
    int64_t sub_id_3 = nextCorrelationId();
    int64_t sub_id_4 = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_1, CHANNEL_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_2, CHANNEL_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_3, CHANNEL_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_4, CHANNEL_1, STREAM_ID_1, -1), 0);

    doWork();

    aeron_receive_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    ASSERT_NE(endpoint, (aeron_receive_channel_endpoint_t *)NULL);
    ASSERT_EQ(aeron_driver_conductor_num_receive_channel_endpoints(&m_conductor.m_conductor), 1u);
    ASSERT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 4u);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToAddMultipleNetworkSubscriptionsWithDifferentChannelSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id_1 = nextCorrelationId();
    int64_t sub_id_2 = nextCorrelationId();
    int64_t sub_id_3 = nextCorrelationId();
    int64_t sub_id_4 = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_1, CHANNEL_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_2, CHANNEL_2, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_3, CHANNEL_3, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addNetworkSubscription(client_id, sub_id_4, CHANNEL_4, STREAM_ID_1, -1), 0);

    doWork();

    aeron_receive_channel_endpoint_t *endpoint_1 =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);
    aeron_receive_channel_endpoint_t *endpoint_2 =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_2);
    aeron_receive_channel_endpoint_t *endpoint_3 =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_3);
    aeron_receive_channel_endpoint_t *endpoint_4 =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_4);

    ASSERT_NE(endpoint_1, (aeron_receive_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_2, (aeron_receive_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_3, (aeron_receive_channel_endpoint_t *)NULL);
    ASSERT_NE(endpoint_4, (aeron_receive_channel_endpoint_t *)NULL);
    ASSERT_EQ(aeron_driver_conductor_num_receive_channel_endpoints(&m_conductor.m_conductor), 4u);
    ASSERT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 4u);

    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutNetworkPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    doWorkUntilTimeNs(
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2));
    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldBeAbleToNotTimeoutNetworkPublicationOnKeepalive)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    int64_t timeout =
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2);

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutNetworkSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_receive_channel_endpoints(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    doWorkUntilTimeNs(
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2));
    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_receive_channel_endpoints(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldBeAbleToNotTimeoutNetworkSubscriptionOnKeepalive)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    int64_t timeout =
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2);

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 1u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutSendChannelEndpointWithClientKeepaliveAfterRemovePublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addNetworkPublication(client_id, pub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 1u);
    ASSERT_EQ(removePublication(client_id, remove_correlation_id, pub_id), 0);
    doWork();
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 2u);

    int64_t timeout =
        m_context.m_context->publication_linger_timeout_ns +
            (m_context.m_context->client_liveness_timeout_ns * 2);

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_publications(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_send_channel_endpoints(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldBeAbleToTimeoutReceiveChannelEndpointWithClientKeepaliveAfterRemoveSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 1u);
    ASSERT_EQ(removeSubscription(client_id, remove_correlation_id, sub_id), 0);
    doWork();
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 2u);

    int64_t timeout = m_context.m_context->client_liveness_timeout_ns;

    doWorkUntilTimeNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_network_subscriptions(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_receive_channel_endpoints(&m_conductor.m_conductor), 0u);
}

TEST_F(DriverConductorTest, shouldCreatePublicationImageForActiveNetworkSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();

    ASSERT_EQ(addNetworkSubscription(client_id, sub_id, CHANNEL_1, STREAM_ID_1, -1), 0);
    doWork();
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 1u);

    aeron_receive_channel_endpoint_t *endpoint =
        aeron_driver_conductor_find_receive_channel_endpoint(&m_conductor.m_conductor, CHANNEL_1);

    createPublicationImage(endpoint, 1000);

    EXPECT_EQ(aeron_driver_conductor_num_images(&m_conductor.m_conductor), 1u);

    aeron_publication_image_t *image =
        aeron_driver_conductor_find_publication_image(&m_conductor.m_conductor, endpoint, STREAM_ID_1);

    EXPECT_NE(image, (aeron_publication_image_t *)NULL);
    EXPECT_EQ(aeron_publication_image_num_subscriptions(image), 1u);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
    {
        ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

        const command::ImageBuffersReadyFlyweight response(buffer, offset);

        EXPECT_EQ(response.sessionId(), SESSION_ID);
        EXPECT_EQ(response.streamId(), STREAM_ID_1);
        EXPECT_EQ(response.correlationId(), aeron_publication_image_registration_id(image));
        EXPECT_EQ(response.subscriberPositionCount(), 1);

        const command::ImageBuffersReadyDefn::SubscriberPosition position = response.subscriberPosition(0);

        EXPECT_EQ(position.registrationId, sub_id);

        EXPECT_EQ(std::string(aeron_publication_image_log_file_name(image)), response.logFileName());
        EXPECT_EQ(SOURCE_IDENTITY, response.sourceIdentity());
    };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}