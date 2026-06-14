#include <gtest/gtest.h>

#include "AmRtpStream.h"
#include "AmDtmfSender.h"

class AmStreamConnectionMock : public AmStreamConnection {
    std::function<void(AmRtpPacket *)> send_callback;

  public:
    AmStreamConnectionMock(AmMediaTransport *_transport, std::function<void(AmRtpPacket *)> send_callback)
        : AmStreamConnection(_transport, "127.0.0.1", 0, ConnectionType::RTP_CONN)
        , send_callback(send_callback)
    {
    }
    virtual void handleConnection(uint8_t *, unsigned int, struct sockaddr_storage *, struct timeval) override {}
    ssize_t      send(AmRtpPacket *packet) override
    {
        send_callback(packet);
        return 0;
    }
};

class AmMediaTransportMock : public AmMediaTransport {
  public:
    explicit AmMediaTransportMock(AmMediaEndpoint *endpoint, std::function<void(AmRtpPacket *)> send_callback)
        : AmMediaTransport(endpoint, 0, 0, RTP_TRANSPORT)
    {
        setCurRtpConn(new AmStreamConnectionMock(this, send_callback));
    }
};

// the transport now lives on the endpoint, so the mock stream supplies a mock endpoint
class AmMediaEndpointMock : public AmMediaEndpoint {
    std::unique_ptr<AmMediaTransportMock> _transport;

  public:
    AmMediaEndpointMock(AmRtpStream *stream, std::function<void(AmRtpPacket *)> send_callback)
        : AmMediaEndpoint(stream, nullptr, 0)
    {
        _transport.reset(new AmMediaTransportMock(this, send_callback));
        setCurrentTransport(_transport.get());
    }
};

class AmRtpStreamMock : public AmRtpStream {
    std::function<void(AmRtpPacket *)> send_callback;
    std::unique_ptr<AmMediaEndpoint>   owned_endpoint; // session-less: no pool to own the endpoint

  protected:
    AmMediaEndpoint *createEndpoint() const override
    {
        return new AmMediaEndpointMock(const_cast<AmRtpStreamMock *>(this), send_callback);
    }

  public:
    explicit AmRtpStreamMock(std::function<void(AmRtpPacket *)> send_callback)
        : AmRtpStream(nullptr, 0, 0)
        , send_callback(send_callback)
    {
        // there is no session here to take ownership, so seed the endpoint now and own it locally
        owned_endpoint.reset(createEndpoint());
        endpoint = owned_endpoint.get();
    }
};

TEST(DtmfSenderTest, EndEventBackwardDuration)
{
    AmDtmfSender                                      sender;
    std::optional<decltype(dtmf_payload_t::duration)> last_duration = 0;

    AmRtpStreamMock stream([&](AmRtpPacket *p) {
        auto dtmf     = reinterpret_cast<const dtmf_payload_t &>(*p->getData());
        auto duration = ntohs(dtmf.duration);
        if (last_duration) {
            GTEST_ASSERT_GE(duration, last_duration.value());
        }
        last_duration = duration;
    });

    sender.queueEvent(9 /* key */, 298 /* duration */, 20 /* volume */, 8000 /* rate */, 20 /* frame_size */);
    for (auto ts = 849056240u; ts < 849056240u + 160 * 30; ts += 160) {
        sender.sendPacket(ts, 0, &stream);
    }
}

TEST(DtmfSenderTest, ShortEvent)
{
    AmDtmfSender sender;
    bool         has_non_end_packets = false, has_end_packets = false;

    AmRtpStreamMock stream([&](AmRtpPacket *p) {
        auto dtmf = reinterpret_cast<const dtmf_payload_t &>(*p->getData());
        if (dtmf.e) {
            has_non_end_packets |= true;
        } else {
            has_end_packets |= true;
        }
    });

    sender.queueEvent(9 /* key */, 1 /* duration */, 20 /* volume */, 8000 /* rate */, 20 /* frame_size */);
    for (auto ts = 0u; ts < 160 * 30; ts += 160) {
        sender.sendPacket(ts, 0, &stream);
    }

    GTEST_ASSERT_TRUE(has_non_end_packets);
    GTEST_ASSERT_TRUE(has_end_packets);
}
