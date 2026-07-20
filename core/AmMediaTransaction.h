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
  public:
    // one staged slot per new m= line; either a live stream (Active*) or a placeholder (Empty).
    // media_idx is the position the slot will occupy in AmSession::_rtp_streams on commit.
    struct StagedSlot {
        std::unique_ptr<AmRtpAudio> stream;
        MediaType                   type;
        TransProt                   transport;
        int                         media_idx;
    };

  private:
    AmSession *session;

    AmSdp prev_local_sdp;

    // same-stream reconfig (new port): a pending endpoint swap applied on commit
    std::map<AmRtpStream *, AmMediaEndpoint *>  bindings;
    std::list<std::unique_ptr<AmMediaEndpoint>> new_endpoints;

    // new m= lines: staged slots (order preserved so commit appends them in m-line order),
    // and existing streams disabled for this reconfig (re-enabled on rollback)
    std::list<StagedSlot>    staged_slots;
    std::list<AmRtpStream *> disabled_streams;

    // endpoint hand-offs (donor -> recipient): reversed on rollback (recipient returns it to the donor)
    std::list<std::pair<AmRtpStream *, AmRtpAudio *>> handoffs;

  public:
    AmMediaTransaction(AmSession *sess, const AmSdp &local_sdp);
    ~AmMediaTransaction();

    AmMediaEndpoint *addEndpoint(AmMediaEndpoint *ep);
    AmRtpAudio      *addStream(AmRtpAudio *s);
    void             addEmptySlot(MediaType type, TransProt transport, int media_idx);
    void             bind(AmRtpStream *stream, AmMediaEndpoint *endpoint);
    void             recordDisabled(AmRtpStream *s) { disabled_streams.push_back(s); }
    void        recordHandoff(AmRtpStream *donor, AmRtpAudio *recipient) { handoffs.push_back({ donor, recipient }); }
    void        forEachStaged(const std::function<void(AmRtpAudio *, MediaType, TransProt)> &fn);
    AmRtpAudio *getStream(int media_idx) const;
    bool        hasSlotAt(int media_idx) const;
    size_t      stagedSlotCount() const { return staged_slots.size(); }

    void commit();
    void rollback(bool send_reinvite = false);
};
