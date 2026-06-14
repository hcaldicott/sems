/*
 * In-flight media reconfiguration (e.g. audio<->T.38): commit on a successful
 * answer, rollback on a rejected one. Holds the staged stream->endpoint wiring
 * and owns the objects built for it until commit hands them over. The live
 * binding is left untouched until commit, so rollback just drops the staged
 * objects and restores the previous media. Owned by the session.
 */
#pragma once

#include "AmSdp.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <utility>

class AmSession;
class AmRtpStream;
class AmRtpAudio;
class AmMediaEndpoint;

class AmMediaTransaction {
    AmSession *session;

    AmSdp prev_local_sdp;

    // same-stream reconfig (new port): a pending endpoint swap applied on commit
    std::map<AmRtpStream *, AmMediaEndpoint *>  bindings;
    std::list<std::unique_ptr<AmMediaEndpoint>> new_endpoints;

    // new m= line: streams staged here (built into the offer, owned until commit),
    // and streams disabled for this reconfig (re-enabled on rollback)
    std::list<std::unique_ptr<AmRtpAudio>> new_streams;
    std::list<AmRtpStream *>               disabled_streams;

    // endpoint hand-offs (donor -> recipient): reversed on rollback (recipient returns it to the donor)
    std::list<std::pair<AmRtpStream *, AmRtpAudio *>> handoffs;

  public:
    AmMediaTransaction(AmSession *sess, const AmSdp &local_sdp);
    ~AmMediaTransaction();

    AmMediaEndpoint *addEndpoint(AmMediaEndpoint *ep);
    AmRtpAudio      *addStream(AmRtpAudio *s);
    void             bind(AmRtpStream *stream, AmMediaEndpoint *endpoint);
    void             recordDisabled(AmRtpStream *s) { disabled_streams.push_back(s); }
    void recordHandoff(AmRtpStream *donor, AmRtpAudio *recipient) { handoffs.push_back({ donor, recipient }); }
    void forEachStaged(const std::function<void(AmRtpAudio *)> &fn);

    void commit();
    void rollback();
};
