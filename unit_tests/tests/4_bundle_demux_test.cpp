#include <gtest/gtest.h>

#include "AmRtpStream.h"
#include "media/AmMediaEndpoint.h"
#include "AmRtpPacket.h"
#include "AmSdp.h" // MID_RTP_HDREXT_DEFAULT_ID

// session-less mock exposing the BUNDLE demux keys (mid / ext id / learned remote ssrc)
class BundleStreamMock : public AmRtpStream {
  public:
    BundleStreamMock(const string &mid, int ext_id)
        : AmRtpStream(nullptr, 0, 0)
    {
        bundle_mid        = mid;
        bundle_mid_ext_id = ext_id;
    }
    void learnRemoteSsrc(uint32_t s)
    {
        r_ssrc   = s;
        r_ssrc_i = true;
    }
};

// build a parsed RTP packet carrying a MID one-byte header extension (RFC 8285) + ssrc
static void makeRtp(AmRtpPacket &p, uint32_t ssrc, const string &mid, int ext_id)
{
    p.payload   = 0;
    p.marker    = false;
    p.sequence  = 1;
    p.timestamp = 0;
    p.ssrc      = ssrc;
    if (!mid.empty())
        p.addHeaderExtension(static_cast<uint8_t>(ext_id), reinterpret_cast<const unsigned char *>(mid.data()),
                             static_cast<uint8_t>(mid.size()));
    unsigned char data[160] = { 0 };
    p.compile(data, sizeof(data));
}

TEST(BundleDemux, RouteByMid)
{
    BundleStreamMock a("0", MID_RTP_HDREXT_DEFAULT_ID), b("1", MID_RTP_HDREXT_DEFAULT_ID);
    AmMediaEndpoint  ep(&a, nullptr, 0); // streams = {a} (primary)
    ep.addMember(&b);                    // streams = {a, b}

    AmRtpPacket pa, pb;
    makeRtp(pa, 0x1111, "0", MID_RTP_HDREXT_DEFAULT_ID);
    makeRtp(pb, 0x2222, "1", MID_RTP_HDREXT_DEFAULT_ID);

    EXPECT_EQ(ep.getStream(&pa), &a);
    EXPECT_EQ(ep.getStream(&pb), &b);
}

TEST(BundleDemux, RouteBySsrcWhenNoMid)
{
    BundleStreamMock a("0", MID_RTP_HDREXT_DEFAULT_ID), b("1", MID_RTP_HDREXT_DEFAULT_ID);
    AmMediaEndpoint  ep(&a, nullptr, 0);
    ep.addMember(&b);
    a.learnRemoteSsrc(0x1111); // learned from earlier MID-tagged packets
    b.learnRemoteSsrc(0x2222);

    AmRtpPacket p;
    makeRtp(p, 0x2222, "", 0);
    EXPECT_EQ(ep.getStream(&p), &b);
    makeRtp(p, 0x1111, "", 0);
    EXPECT_EQ(ep.getStream(&p), &a);
}
