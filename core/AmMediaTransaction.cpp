#include "AmMediaTransaction.h"
#include "AmSession.h"
#include "AmRtpStream.h"
#include "AmRtpAudio.h"
#include "AmMediaEndpoint.h"

AmMediaTransaction::AmMediaTransaction(AmSession *sess, const AmSdp &local_sdp)
    : session(sess)
    , prev_local_sdp(local_sdp)
{
}

AmMediaTransaction::~AmMediaTransaction() = default;

AmMediaEndpoint *AmMediaTransaction::addEndpoint(AmMediaEndpoint *ep)
{
    new_endpoints.emplace_back(ep);
    return ep;
}

AmRtpAudio *AmMediaTransaction::addStream(AmRtpAudio *s)
{
    new_streams.emplace_back(s);
    return s;
}

void AmMediaTransaction::bind(AmRtpStream *stream, AmMediaEndpoint *endpoint)
{
    bindings[stream] = endpoint;
    stream->setPendingEndpoint(endpoint); // SDP is built from the staged endpoint until commit
}

void AmMediaTransaction::forEachStaged(const std::function<void(AmRtpAudio *)> &fn)
{
    for (auto &s : new_streams)
        fn(s.get());
}

void AmMediaTransaction::commit()
{
    // same-stream endpoint swap: re-point each stream to its staged endpoint (the old one stays in the
    // session pool until the session ends - in a bundle the shared endpoint needs guarding, TODO(C1))
    for (auto &b : bindings)
        b.first->setEndpoint(b.second);
    for (auto &ep : new_endpoints)
        session->addMediaEndpoint(ep.release()); // staged endpoints handed to the session pool
    new_endpoints.clear();

    for (auto &h : handoffs) {
        h.second->setEndpoint(h.first->getEndpoint());
        h.first->releaseEndpoint();
    }
    handoffs.clear();

    for (auto &s : new_streams)
        session->addRtpStream(s.release());
    new_streams.clear();

    disabled_streams.clear();
}

void AmMediaTransaction::rollback()
{
    // the live binding was never touched - just drop the staged objects
    for (auto &b : bindings)
        b.first->clearPendingEndpoint();
    bindings.clear();
    new_endpoints.clear(); // free staged endpoints

    // the endpoint was never moved (only borrowed for the offer): drop the recipient's pending view;
    // the donor keeps its endpoint and its RX (it was never stopped)
    for (auto &h : handoffs)
        h.second->clearPendingEndpoint();
    handoffs.clear();

    new_streams.clear();

    for (auto *s : disabled_streams)
        s->setDisabled(false);
    disabled_streams.clear();

    // recovery original transport
    for (size_t i = 0; i < prev_local_sdp.media.size(); i++)
        if (AmRtpStream *s = session->RTPStream((unsigned)i))
            s->setTransport(prev_local_sdp.media[i].transport);

    session->restoreMedia(prev_local_sdp);
}
