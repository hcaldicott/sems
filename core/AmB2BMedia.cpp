#include "AmB2BMedia.h"
#include "AmAudio.h"
#include "AmB2BSession.h"
#include "AmRtpReceiver.h"
#include "AmUtils.h"
#include "sip/msg_logger.h"
#include "amci/codecs.h"

#include <string.h>
#include <strings.h>
#include <algorithm>
#include <stdexcept>

using namespace std;

#define TRACE             DBG
#define UNDEFINED_PAYLOAD (-1)

/** class for computing payloads for relay the simpliest way - allow relaying of
 * all payloads supported by remote party */
static B2BMediaStatistics b2b_stats;

static const string zero_ip("0.0.0.0");

static void replaceRtcpAttr(SdpMedia &m, const string &relay_address, int rtcp_port)
{
    for (auto &a : m.attributes) {
        try {
            if (a.attribute == "rtcp") {
                RtcpAddress addr(a.value);
                addr.setPort(rtcp_port);
                if (addr.hasAddress())
                    addr.setAddress(relay_address);
                a.value = addr.print();
            }
        } catch (const exception &e) {
            DBG("can't replace RTCP address: %s", e.what());
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////

void B2BMediaStatistics::incCodecWriteUsage(const string &codec_name)
{
    if (codec_name.empty())
        return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_write_usage.find(codec_name);

    if (i != codec_write_usage.end())
        i->second++;
    else
        codec_write_usage[codec_name] = 1;
}

void B2BMediaStatistics::decCodecWriteUsage(const string &codec_name)
{
    if (codec_name.empty())
        return;

    AmLock                     lock(mutex);
    map<string, int>::iterator i = codec_write_usage.find(codec_name);
    if (i != codec_write_usage.end()) {
        if (i->second > 0)
            i->second--;
    }
}

void B2BMediaStatistics::incCodecReadUsage(const string &codec_name)
{
    if (codec_name.empty())
        return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_read_usage.find(codec_name);

    if (i != codec_read_usage.end())
        i->second++;
    else
        codec_read_usage[codec_name] = 1;
}

void B2BMediaStatistics::decCodecReadUsage(const string &codec_name)
{
    if (codec_name.empty())
        return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_read_usage.find(codec_name);
    if (i != codec_read_usage.end()) {
        if (i->second > 0)
            i->second--;
    }
}

B2BMediaStatistics *B2BMediaStatistics::instance()
{
    return &b2b_stats;
}

void B2BMediaStatistics::reportCodecWriteUsage(string &dst)
{
    if (codec_write_usage.empty()) {
        dst = "pcma=0"; // to be not empty
        return;
    }

    bool first = true;
    dst.clear();
    AmLock lock(mutex);
    for (map<string, int>::iterator i = codec_write_usage.begin(); i != codec_write_usage.end(); ++i) {
        if (first)
            first = false;
        else
            dst += ",";
        dst += i->first;
        dst += "=";
        dst += int2str(i->second);
    }
}

void B2BMediaStatistics::reportCodecReadUsage(string &dst)
{
    if (codec_read_usage.empty()) {
        dst = "pcma=0"; // to be not empty
        return;
    }

    bool first = true;
    dst.clear();
    AmLock lock(mutex);
    for (map<string, int>::iterator i = codec_read_usage.begin(); i != codec_read_usage.end(); ++i) {
        if (first)
            first = false;
        else
            dst += ",";
        dst += i->first;
        dst += "=";
        dst += int2str(i->second);
    }
}

void B2BMediaStatistics::getReport(const AmArg &, AmArg &ret)
{
    AmArg write_usage;
    AmArg read_usage;

    { // locked area
        AmLock lock(mutex);

        for (map<string, int>::iterator i = codec_write_usage.begin(); i != codec_write_usage.end(); ++i) {
            AmArg avp;
            avp["codec"] = i->first;
            avp["count"] = i->second;
            write_usage.push(avp);
        }

        for (map<string, int>::iterator i = codec_read_usage.begin(); i != codec_read_usage.end(); ++i) {
            AmArg avp;
            avp["codec"] = i->first;
            avp["count"] = i->second;
            read_usage.push(avp);
        }
    }

    ret["write"] = write_usage;
    ret["read"]  = read_usage;
}

//////////////////////////////////////////////////////////////////////////////////
StreamData::StreamData(AmB2BSession *_leg, int _media_idx, State initial, MediaType _type, TransProt _transport)
    : leg(nullptr) // set via setLeg below
    , media_idx(_media_idx)
    , state(initial)
    , type(_type)
    , transport(_transport)
    , initialized(false)
    , dtmf_detector(nullptr)
    , dtmf_queue(nullptr)
    , outgoing_payload(UNDEFINED_PAYLOAD)
    , incoming_payload(UNDEFINED_PAYLOAD)
{
    setLeg(_leg, initial == ActiveAudio);
}

StreamData::~StreamData()
{
    /* prevent stream leak
     * on streams.clear() in AmB2BMedia::clearAudio(bool a_leg)
     * or AmB2BMedia destruction without explicit clearing */
    if (auto *s = getStream())
        s->stopReceiving();
    clear();
}

AmRtpAudio *StreamData::getStream() const
{
    if (!leg || media_idx < 0)
        return nullptr;
    return leg->RTPStream(static_cast<unsigned>(media_idx));
}

void StreamData::initialize(bool audio)
{
    CLASS_DBG("StreamData::initialize()");

    in                            = nullptr;
    out                           = nullptr;
    dtmf_detector                 = nullptr;
    dtmf_queue                    = nullptr;
    enable_dtmf_transcoding       = false;
    force_symmetric_rtp           = false;
    enable_inbound_dtmf_filtering = false;
    enable_dtmf_rtp_filtering     = false;
    enable_dtmf_rtp_detection     = false;
    relay_map.clear();
    relay_mask.clear();
    relay_enabled = false;
    relay_port    = 0;
    relay_paused  = false;
    relay_address.clear();
    muted            = false;
    outgoing_payload = UNDEFINED_PAYLOAD;
    incoming_payload = UNDEFINED_PAYLOAD;
    outgoing_payload_name.clear();
    incoming_payload_name.clear();
    lowfi_payloads.clear();

    if (!audio) {
        initialized = true;
        return;
    }

    // no live stream yet (Empty pair, or a not-yet-attached leg in distributed B2B):
    // configuration will be re-applied by the next initialize() once the leg has the stream.
    auto *s = getStream();
    if (!s)
        return;

    s->setRtpRelayTransparentSeqno(leg->getRtpRelayTransparentSeqno());
    s->setRtpRelayTransparentSSRC(leg->getRtpRelayTransparentSSRC());
    s->setRtpRelayFilterRtpDtmf(leg->getEnableDtmfRtpFiltering());
    s->setRtpForceRelayDtmf(leg->getEnableDtmfForceRelay());
    s->setRtpForceRelayCN(leg->getEnableCNForceRelay());
    s->setRtpTimeout(leg->getRtpTimeout());
    s->setRtpPing(leg->getRtpPing());
    s->setRtpRelayTimestampAligning(leg->getRtpRelayTimestampAligning());

    TransProt trsp = leg->getMediaTransport();
    if (TP_NONE != trsp)
        s->setTransport(trsp);

    if (leg->getEnableDtmfRtpDetection())
        s->force_receive_dtmf = true;

    s->getEndpoint()->setLocalIP();

    force_symmetric_rtp           = leg->getRtpRelayForceSymmetricRtp();
    enable_dtmf_transcoding       = leg->getEnableDtmfTranscoding();
    enable_inbound_dtmf_filtering = leg->getEnableInboundDtmfFiltering();

    leg->getLowFiPLs(lowfi_payloads);
}

bool StreamData::initStream(PlayoutType playout_type, AmSdp &local_sdp, AmSdp &remote_sdp)
{
    resetStats();

    auto *stream = getStream();
    if (!stream) {
        initialized = false;
        return false;
    }

    // The owner AmSession may be concurrently encoding/decoding this stream on a
    // media-processor thread under its audio_mut; stream->init() below frees and
    // recreates the codec, so it must not run inside encode()/decode().
    int res;
    {
        AmAudioLockGuard audio_guard(leg);
        res = stream->init(local_sdp, remote_sdp, sdp_offer_owner, force_symmetric_rtp);
    }

    if (res == 0) {
        stream->setPlayoutType(playout_type);
        initialized = true;
        // do not unmute if muted because of 0.0.0.0 remote IP (the mute flag is set during init)
        // if (!stream->muted()) stream->setOnHold(muted);
    } else {
        initialized = false;
        DBG("stream initialization failed");
        // there still can be payloads to be relayed (if all possible payloads are
        // to be relayed this needs not to be an error)
    }

    /* prioritize stream disabled sending over StreamData::muted */
    stream->setMute(muted);

    // NOTE: commented out because of incorrect overriding of the stream state negotiated by SDP
    // this change breaks setReceiving(bool receiving_a, bool receiving_b) behavior
    // stream->setReceiving(receiving);

    return initialized;
}

void StreamData::clear()
{
    resetStats();
    in = nullptr;
    clearDtmfSink();

    if (auto *s = getStream()) {
        s->disableRtpRelay();
        s->setRelayStream(nullptr);
    }
    initialized = false;
}

void StreamData::clearDtmfSink()
{
    if (dtmf_detector) {
        delete dtmf_detector;
        dtmf_detector = nullptr;
    }
    if (dtmf_queue) {
        delete dtmf_queue;
        dtmf_queue = nullptr;
    }
}

void StreamData::resetStats()
{
    if (outgoing_payload != UNDEFINED_PAYLOAD) {
        b2b_stats.decCodecWriteUsage(outgoing_payload_name);
        outgoing_payload = UNDEFINED_PAYLOAD;
        outgoing_payload_name.clear();
    }
    if (incoming_payload != UNDEFINED_PAYLOAD) {
        b2b_stats.decCodecReadUsage(incoming_payload_name);
        incoming_payload = UNDEFINED_PAYLOAD;
        incoming_payload_name.clear();
    }
}

void StreamData::debug()
{
    if (auto *s = getStream())
        s->debug();
}

void StreamData::getInfo(AmArg &ret)
{
    ret["muted"]            = muted;
    ret["outgoing_payload"] = outgoing_payload_name;
    ret["incoming_payload"] = incoming_payload_name;

    if (auto *s = getStream()) {
        AmArg &a = ret["stream"];
        s->getInfo(a);
    }
}

void StreamData::setLeg(AmB2BSession *l, bool audio)
{
    // detach: unwind relay wiring on the old leg's stream before we forget it.
    if (leg && leg != l)
        clear();

    leg = l;

    // materialise the leg's slot at our media_idx if not present yet.
    if (l && media_idx >= 0 && !l->hasRtpStream(static_cast<unsigned>(media_idx))) {
        switch (state) {
        case ActiveAudio:
        case ActiveRelay: l->addRtpStream(); break;
        case Empty:       l->addEmptyRtpSlot(type, transport); break;
        case Inactive:    break; // was Active, slot already exists — nothing to add
        }
    }

    // (re)apply per-leg B2B config; getStream() is now valid if we just added a live slot.
    initialize(audio);
}

void StreamData::transition(State desired)
{
    if (state == desired)
        return;
    switch (desired) {
    case ActiveAudio:
    case ActiveRelay:
        if (state == Empty && leg)
            leg->activateRtpSlot(static_cast<unsigned>(media_idx));
        setLeg(leg, desired == ActiveAudio);
        break;
    case Inactive:
        if (auto *s = getStream())
            s->stopReceiving();
        clear();
        break;
    case Empty: break; // unreachable — Empty is only assigned by ctor
    }
    state = desired;
}

void StreamData::setRelayStream(AmRtpAudio *other)
{
    auto *stream = getStream();
    if (!stream)
        return;

    if (relay_address.empty()) {
        DBG("not setting relay for empty relay address");
        stream->disableRtpRelay();
        return;
    }

    if (!relay_enabled || !other) {
        // nothing to relay or other stream not set
        stream->disableRtpRelay();
        return;
    }

    if (other->getFrameTime() != stream->getFrameTime()) {
        DBG("not setting relay for streams with different frame sizes");
        stream->disableRtpRelay();
        return;
    }

    if (stream->isRecordEnabled() || other->isRecordEnabled()) {
        DBG("disable relay because of enabled recording");
        stream->disableRtpRelay();
        return;
    }

    stream->setRelayStream(other);
    stream->setForceBuffering(other->isRecordEnabled());
    stream->setRelayPayloads(relay_mask);
    stream->setRelayPayloadMap(relay_map);
    if (!relay_paused)
        stream->enableRtpRelay();
    stream->setRAddr(relay_address, static_cast<unsigned short>(relay_port));
}

void StreamData::setRelayPayloads(const SdpMedia &m, RelayController *ctrl)
{
    ctrl->computeRelayMask(m, relay_enabled, relay_mask, relay_map);
}

void StreamData::setRelayDestination(const string &connection_address, int port)
{
    relay_address = connection_address;
    relay_port    = port;
}

void StreamData::setRelayPaused(bool paused)
{
    auto *stream = getStream();
    if (paused == relay_paused) {
        DBG("relay already paused for stream [%p], ignoring", static_cast<void *>(stream));
        return;
    }

    relay_paused = paused;
    DBG("relay %spaused, stream [%p]", relay_paused ? "" : "not ", static_cast<void *>(stream));

    if (nullptr != stream) {
        if (relay_paused)
            stream->disableRtpRelay();
        else
            stream->enableRtpRelay();
    }
}

void StreamData::mute(bool set_mute)
{
    auto *stream = getStream();
    DBG("mute(%s) - RTP stream [%p]", set_mute ? "true" : "false", static_cast<void *>(stream));

    if (stream) {
        stream->setMute(set_mute);
        if (muted != set_mute)
            stream->clearRTPTimeout();
    }
    muted = set_mute;
}

void StreamData::setDtmfSink(AmDtmfSink *dtmf_sink)
{
    // TODO: optimize: clear & create the dtmf_detector only if the dtmf_sink changed
    clearDtmfSink();

    auto *stream = getStream();
    if (dtmf_sink && stream) {
        dtmf_detector = new AmDtmfDetector(dtmf_sink);
        dtmf_queue    = new AmDtmfEventQueue(dtmf_detector);
        dtmf_detector->setInbandDetector(AmConfig.default_dtmf_detector, stream->getSampleRate());

        if (!enable_dtmf_transcoding && lowfi_payloads.size()) {
            string selected_payload_name = stream->getPayloadName(stream->getPayloadType());
            for (vector<SdpPayload>::iterator it = lowfi_payloads.begin(); it != lowfi_payloads.end(); ++it) {
                DBG("checking %s/%i PL type against %s/%i", selected_payload_name.c_str(), stream->getPayloadType(),
                    it->encoding_name.c_str(), it->payload_type);
                if (selected_payload_name == it->encoding_name) {
                    enable_dtmf_transcoding = true;
                    break;
                }
            }
        }
    }
}

void StreamData::sendDtmf(int event, unsigned int duration_ms, int volume)
{
    DBG("StreamData::sendDtmf(event = %d, duration = %u, volume = %d)", event, duration_ms, volume);
    if (auto *s = getStream())
        s->sendDtmf(event, duration_ms, volume);
}

void StreamData::updateSendStats()
{
    if (!initialized) {
        resetStats();
        return;
    }

    auto *stream = getStream();
    if (!stream)
        return;
    int payload = stream->getPayloadType();
    if (payload != outgoing_payload) {
        // payload used to send has changed

        // decrement usage of previous payload if set
        if (outgoing_payload != UNDEFINED_PAYLOAD)
            b2b_stats.decCodecWriteUsage(outgoing_payload_name);

        if (payload != UNDEFINED_PAYLOAD) {
            // remember payload name (in lowercase to simulate case insensitivity)
            outgoing_payload_name = stream->getPayloadName(payload);
            transform(outgoing_payload_name.begin(), outgoing_payload_name.end(), outgoing_payload_name.begin(),
                      ::tolower);
            b2b_stats.incCodecWriteUsage(outgoing_payload_name);
        } else
            outgoing_payload_name.clear();
        outgoing_payload = payload;
    }
}

void StreamData::updateRecvStats(AmRtpStream *s)
{
    if (!initialized) {
        resetStats();
        return;
    }

    auto *stream = getStream();
    if (!stream)
        return;
    int payload = s->getLastPayload();
    if (payload != incoming_payload) {
        // payload used to send has changed

        // decrement usage of previous payload if set
        if (incoming_payload != UNDEFINED_PAYLOAD)
            b2b_stats.decCodecReadUsage(incoming_payload_name);

        if (payload != UNDEFINED_PAYLOAD) {
            // remember payload name (in lowercase to simulate case insensitivity)
            incoming_payload_name = stream->getPayloadName(payload);
            transform(incoming_payload_name.begin(), incoming_payload_name.end(), incoming_payload_name.begin(),
                      ::tolower);
            b2b_stats.incCodecReadUsage(incoming_payload_name);
        } else
            incoming_payload_name.clear();
        incoming_payload = payload;
    }
}

int StreamData::writeStream(unsigned long long ts, unsigned char *buffer, StreamData &src)
{
    AmRtpAudio *stream        = getStream();
    AmRtpAudio *src_stream    = src.getStream();
    bool        dtmf_detected = false;
    if (!initialized) {
        if (!in || !out)
            return 0;
        // non-stream mode
        if (!src.isInitialized())
            return 0; // other leg MUST be initialized with stream

        if (src_stream->checkInterval(ts)) {
            int sample_rate = src_stream->getSampleRate();
            if (0 == sample_rate) [[unlikely]] {
                return 0;
            }

            int got = src_stream->get(ts, buffer, sample_rate, src_stream->getFrameSize());
            // CLASS_DBG("src_stream->get(%llu,%d)",ts,got);
            if (got < 0) {
                DBG("src_stream->get: %d", got);
                return -1;
            }
            if (got > 0) {
                updateRecvStats(src_stream);
                // CLASS_DBG("out->put(%llu,%d)",ts,got);
                out->applyPendingStereoRecorders(leg);

                return out->put(ts, buffer, sample_rate, static_cast<unsigned int>(got));
            }
        }
        return 0;
    }

    if (stream->getOnHold())
        return 0; // ignore hold streams?

    unsigned int f_size = stream->getFrameSize();
    if (stream->sendIntReached(ts)) {
        // A leg is ready to send data
        int sample_rate = stream->getSampleRate();
        if (0 == sample_rate) [[unlikely]] {
            return 0;
        }
        int got = 0;
        if (in) {
            // process src_stream even if custom input enabled
            if (src.isInitialized()) {
                if (src_stream->checkInterval(ts) || stream->getFrameTime() <= src_stream->getFrameTime()) {
                    int tmp_got = src_stream->get(ts, buffer, sample_rate, f_size);
                    // DBG("[%p] stream %p got %d from stream input %p",this,stream,got,src_stream);
                    if (tmp_got > 0) {
                        if (src_stream->isLastSamplesRelayed()) {
                            stream->record(ts, buffer, sample_rate, static_cast<unsigned int>(tmp_got));
                        } else {
                            updateRecvStats(src_stream);
                            if (dtmf_queue && enable_dtmf_transcoding) {
                                dtmf_queue->putDtmfAudio(dtmf_detected, buffer, tmp_got, ts);
                            }
                        }
                    }
                }
            }
            got = in->get(ts, buffer, sample_rate, f_size);
            // DBG("[%p] stream %p got %d from non-stream input %p",this,stream,got,in);
            if (got < 0)
                return 0;
        } else {
            if (!src.isInitialized()) {
                // non-stream mode
                AmAudio *src_in = src.getInput();
                if (!src_in)
                    return 0;
                got = src_in->get(ts, buffer, sample_rate, f_size);
                // CLASS_DBG("src_in->get(%llu,%d)",ts,got);
            } else {
                if (src_stream->checkInterval(ts) || stream->getFrameTime() <= src_stream->getFrameTime()) {
                    got = src_stream->get(ts, buffer, sample_rate, f_size);
                    // DBG("[%p] stream %p got %d from stream %p",this,stream,got,src_stream);
                    if (got > 0) {
                        src_stream->feedInbandDetector(buffer, static_cast<unsigned int>(got), ts);
                        if (src_stream->isLastSamplesRelayed()) {
                            stream->record(ts, buffer, sample_rate, static_cast<unsigned int>(got));
                            return 0;
                        } else {
                            updateRecvStats(src_stream);
                            if (dtmf_queue && enable_dtmf_transcoding) {
                                dtmf_queue->putDtmfAudio(dtmf_detected, buffer, got, ts);
                                if (enable_inbound_dtmf_filtering && dtmf_detected) {
                                    DBG("cut inbound dtmf from %p", static_cast<void *>(stream));
                                    memset(buffer, 0, static_cast<unsigned int>(got));
                                    // got = src_stream->conceal_loss(PCM16_B2S(got),buffer);
                                }
                            }
                        }
                    }
                }
            }

            if (got < 0) {
                if (!src.isInitialized()) {
                    DBG("src_in->get: %d", got);
                } else {
                    DBG("src_stream->get: %d", got);
                }
                return -1;
            }
        }

        stream->processRtcpTimers(ts, stream->scaleSystemTS(ts));

        if (got > 0) {
            updateSendStats();
            // CLASS_DBG("stream->put(%llu,%d)",ts,got);
            if (src_stream && src_stream->isRecvSamplesTimeout()) {
                stream->ignoreRecording();
            }

            stream->applyPendingStereoRecorders(leg);

            auto ret = stream->put(ts, buffer, sample_rate, static_cast<unsigned int>(got));
            if (ret < 0) {
                DBG("stream->put: %d", ret);
            }
            return ret;
        } else {
            // to process stuff like dtmf queues even on no data received for stream
            stream->put_on_idle(ts);
        }
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////
AmB2BMedia::AmB2BMedia(AmB2BSession *_a, AmB2BSession *_b)
    : a(_a)
    , b(_b)
    , callgroup(AmSession::getNewId())
    , have_a_leg_local_sdp(false)
    , have_a_leg_remote_sdp(false)
    , have_b_leg_local_sdp(false)
    , have_b_leg_remote_sdp(false)
    , ref_cnt(0)
    , // everybody who wants to use must add one reference itselves
    playout_type(ADAPTIVE_PLAYOUT)
    , a_leg_muted(false)
    , b_leg_muted(false)
    , relay_paused(false)
    , logger(nullptr)
    , sklfile(nullptr)
    , asensor(nullptr)
    , bsensor(nullptr)
    , ignore_relay_streams(false)
{
    DBG("AmB2BMedia[%p](%p,%p)", static_cast<void *>(this), static_cast<void *>(_a), static_cast<void *>(_b));
}

AmB2BMedia::~AmB2BMedia()
{
    DBG("~AmB2BMedia[%p]()", static_cast<void *>(this));
    if (streams.size()) {
        ERROR("~AmB2BMedia[%p] streams size: %zu", static_cast<void *>(this), streams.size());
    }
    if (logger)
        dec_ref(logger);
    if (sklfile)
        dec_ref(sklfile);
    if (asensor)
        dec_ref(asensor);
    if (bsensor)
        dec_ref(bsensor);
}

void AmB2BMedia::addToMediaProcessor()
{
    // AmMediaProcessor's reference
    // will be released by onMediaProcessingTerminated() or onMediaSessionExists()
    addReference();
    if (!AmMediaProcessor::instance()->addSession(this, callgroup))
        releaseReference();
}

void AmB2BMedia::addToMediaProcessorUnsafe()
{
    // AmMediaProcessor's reference
    // will be released by onMediaProcessingTerminated() or onMediaSessionExists()
    ref_cnt++;

    if (!AmMediaProcessor::instance()->addSession(this, callgroup))
        ref_cnt--;
}

void AmB2BMedia::addReference()
{
    mutex.lock();
    ref_cnt++;
    mutex.unlock();
}

bool AmB2BMedia::releaseReference()
{
    mutex.lock();
    int r = --ref_cnt;
    mutex.unlock();
    if (r == 0) {
        DBG("last reference to AmB2BMedia [%p] cleared, destroying", static_cast<void *>(this));
        delete this;
    }
    return (r == 0);
}

void AmB2BMedia::changeSession(bool a_leg, AmB2BSession *new_session)
{
    AmLock lock(mutex);
    changeSessionUnsafe(a_leg, new_session);
}

void AmB2BMedia::changeSessionUnsafe(bool a_leg, AmB2BSession *new_session)
{
    TRACE("changing %s leg session to %p\n", a_leg ? "A" : "B", static_cast<void *>(new_session));

    if (a_leg) {
        if (a)
            a->onSessionChange(new_session);
        a = new_session;
    } else {
        if (b)
            b->onSessionChange(new_session);
        b = new_session;
    }

    bool needs_processing = a && b && a->getRtpRelayMode() == AmB2BSession::RTP_Transcoding;

    // update all streams
    for (auto &pair : streams) {
        if (pair.audio()) {
            // stop processing first to avoid unexpected results
            pair.a.stopStreamProcessing();
            pair.b.stopStreamProcessing();

            // replace leg
            if (a_leg)
                pair.a.setLeg(new_session, true);
            else
                pair.b.setLeg(new_session, true);

            // cross-leg wiring only — codec re-init is SDP-driven (updateStreams path).
            syncPairWiring(pair);

            if (pair.requiresProcessing())
                needs_processing = true;

            // reset logger (needed if a stream changes)
            pair.setLogger(logger);
            pair.setSklLogger(sklfile);
            pair.setASensor(asensor);
            pair.setBSensor(bsensor);

            // return back for processing if needed
            pair.a.resumeStreamProcessing();
            pair.b.resumeStreamProcessing();
        } else {
            if (a_leg)
                pair.a.setLeg(new_session, false);
            else
                pair.b.setLeg(new_session, false);
        }
    }

    if (needs_processing) {
        addToMediaProcessorUnsafe();
    } else {
        AmMediaProcessor::instance()->removeSession(this);
    }

    TRACE("session changed\n");
}

int AmB2BMedia::writeStreams(unsigned long long ts, unsigned char *buffer)
{
    int    res = 0;
    AmLock lock(mutex);
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        if (pair.a.writeStream(ts, buffer, pair.b) < 0) {
            res = -1;
            break;
        }
        if (pair.b.writeStream(ts, buffer, pair.a) < 0) {
            res = -1;
            break;
        }
    }
    return res;
}

void AmB2BMedia::ping(unsigned long long ts)
{
    AmLock lock(mutex);
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        if (pair.a.getStream())
            pair.a.getStream()->ping(ts);
        if (pair.b.getStream())
            pair.b.getStream()->ping(ts);
    }
}

void AmB2BMedia::processDtmfEvents()
{
    AmLock lock(mutex);
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        pair.a.processDtmfEvents();
        pair.b.processDtmfEvents();
    }

    if (a)
        a->processDtmfEvents();
    if (b)
        b->processDtmfEvents();
}

void AmB2BMedia::sendDtmf(bool a_leg, int event, unsigned int duration_ms, int volume)
{
    AmLock lock(mutex);
    if (!streams.size())
        return;

    // send the DTMFs using the first available stream
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        if (a_leg)
            pair.a.sendDtmf(event, duration_ms, volume);
        else
            pair.b.sendDtmf(event, duration_ms, volume);
        break;
    }
}

void AmB2BMedia::clearAudio()
{
    if (a)
        a->postEvent(new B2BEvent(B2BClearMedia));
    if (b)
        b->postEvent(new B2BEvent(B2BClearMedia));
}

void AmB2BMedia::clearAudio(bool a_leg)
{
    TRACE("[%p] clear %s leg audio\n", static_cast<void *>(this), a_leg ? "A" : "B");

    AmLock lock(mutex);

    for (auto &pair : streams) {
        // remove streams from AmRtpReceiver first! (always both?)
        pair.a.stopStreamProcessing();
        pair.b.stopStreamProcessing();
        if (!pair.audio())
            continue;
        if (a_leg) {
            pair.a.clear();
            pair.b.setRelayStream(nullptr);
        } else {
            pair.b.clear();
            pair.a.setRelayStream(nullptr);
        }
    }

    // forget sessions to avoid using them once clearAudio is called
    changeSessionUnsafe(a_leg, nullptr);

    if (a_leg) {
        have_a_leg_local_sdp  = false;
        have_a_leg_remote_sdp = false;
    } else {
        have_b_leg_local_sdp  = false;
        have_b_leg_remote_sdp = false;
    }

    if (!a && !b) {
        streams.clear();
    }
}

void AmB2BMedia::clearRTPTimeout()
{
    AmLock lock(mutex);
    for (auto &pair : streams) {
        pair.a.clearRTPTimeout();
        pair.b.clearRTPTimeout();
    }
}

bool AmB2BMedia::canRelay(const SdpMedia &m)
{
    return (m.transport == TP_RTPAVP) || (m.transport == TP_RTPSAVP) || (m.transport == TP_UDPTLSRTPSAVP) ||
           (m.transport == TP_UDP) || (m.transport == TP_UDPTL);
}

void AmB2BMedia::createStreams(const AmSdp &sdp)
{
    int idx = 0;
    for (auto m = sdp.media.begin(); m != sdp.media.end(); ++m, ++idx) {
        if (static_cast<size_t>(idx) < streams.size())
            continue;

        StreamData::State initial;
        if (m->port == 0 && !m->use_bundle) // bundle-only member may carry port=0
            initial = StreamData::Empty;
        else if (m->type == MT_AUDIO)
            initial = StreamData::ActiveAudio;
        else if (!ignore_relay_streams && canRelay(*m))
            initial = StreamData::ActiveRelay;
        else
            initial = StreamData::Empty;

        // pair ctor → StreamData ctor → setLeg → materialises the leg slot and initialize()
        auto &p = streams.emplace_back(a, b, idx, initial, static_cast<MediaType>(m->type), m->transport);
        DBG("[%p] createStreams() created StreamPair for m=%d, state=%d", static_cast<void *>(this), idx,
            static_cast<int>(initial));

        if (p.audio()) {
            p.a.mute(a_leg_muted);
            p.b.mute(b_leg_muted);
        }
        p.setLogger(logger);
        p.setSklLogger(sklfile);
        p.setASensor(asensor);
        p.setBSensor(bsensor);
    }
}

void AmB2BMedia::replaceConnectionAddress(AmSdp &parser_sdp, bool a_leg, AddressType addr_type)
{
    AmLock lock(mutex);

    string        public_address;
    SdpConnection orig_conn = parser_sdp.conn; // needed for the 'quick workaround' for non-audio media

    createStreams(parser_sdp);

    string replaced_ports;

    // streams and parser_sdp.media are 1:1 by index;
    // kind of the slot is taken from the current m-line,
    // not from the pair's cached state (may lag on reinvite).
    auto it      = parser_sdp.media.begin();
    auto pair_it = streams.begin();
    for (unsigned int idx = 0; it != parser_sdp.media.end() && pair_it != streams.end(); ++it, ++pair_it, ++idx) {
        // FIXME: only UDP streams are handled for now
        if (it->type == MT_AUDIO) {
            public_address.clear();
            try {
                auto stream = a_leg ? pair_it->a.getStream() : pair_it->b.getStream();
                if (stream) {
                    stream->replaceAudioMediaParameters(*it, idx, addr_type);
                    public_address = stream->getEndpoint()->getLocalAddress();
                    if (!replaced_ports.empty())
                        replaced_ports += "/";
                    replaced_ports += int2str(it->port);
                }
            } catch (const string &s) {
                ERROR("setting port: '%s'", s.c_str());
                throw string("error setting RTP port\n");
            }

            if (!public_address.empty() && !it->conn.address.empty() && (parser_sdp.conn.address != zero_ip)) {
                it->conn.address  = public_address;
                it->conn.addrType = addr_type;
                DBG("new stream connection address: %s", it->conn.address.c_str());
            }
        } else if (canRelay(*it)) {
            if (it->port) { // if stream active
                public_address.clear();
                try {
                    auto stream = a_leg ? pair_it->a.getStream() : pair_it->b.getStream();
                    if (stream) {
                        stream->getEndpoint()->setLocalIP(addr_type);
                        public_address = stream->getEndpoint()->getLocalAddress();
                        it->port       = static_cast<unsigned int>(stream->getEndpoint()->getLocalPort());
                        replaceRtcpAttr(*it, stream->getEndpoint()->getLocalIP(),
                                        stream->getEndpoint()->getLocalRtcpPort());

                        if (!replaced_ports.empty())
                            replaced_ports += "/";
                        replaced_ports += int2str(it->port);
                    }
                } catch (const string &s) {
                    ERROR("setting port: '%s'", s.c_str());
                    throw string("error setting RTP port\n");
                }

                if (!public_address.empty() && !it->conn.address.empty() && (parser_sdp.conn.address != zero_ip)) {
                    it->conn.address  = public_address;
                    it->conn.addrType = addr_type;
                    DBG("new stream connection address: %s", it->conn.address.c_str());
                }
            }
        } else {
            // non-audio, non-canRelay m= (Empty/Inactive pair): propagate remote's
            // connection address unchanged.
            if (it->conn.address.empty())
                it->conn = orig_conn;
        }
    }

    // place relay_address in connection address
    if (!parser_sdp.conn.address.empty() && (parser_sdp.conn.address != zero_ip)) {
        parser_sdp.conn.address = public_address;
        DBG("new connection address: %s", parser_sdp.conn.address.c_str());
    }

    DBG("replaced connection address in SDP with %s:%s", public_address.c_str(), replaced_ports.c_str());
}

void AmB2BMedia::initPairStream(StreamPair &pair)
{
    if (!pair.active())
        return;

    try {
        if (have_a_leg_local_sdp && have_a_leg_remote_sdp)
            pair.a.initStream(playout_type, a_leg_local_sdp, a_leg_remote_sdp);
        if (have_b_leg_local_sdp && have_b_leg_remote_sdp)
            pair.b.initStream(playout_type, b_leg_local_sdp, b_leg_remote_sdp);
    } catch (const string &err) {
        ERROR("initPairStream failed: %s", err.c_str());
    }
}

void AmB2BMedia::syncPairWiring(StreamPair &pair)
{
    if (!pair.audio())
        return;

    pair.a.setDtmfSink(b);
    pair.b.setDtmfSink(a);

    // relay: skip if the other leg has an external input override (mixer)
    if (pair.b.getInput())
        pair.a.setRelayStream(nullptr);
    else
        pair.a.setRelayStream(pair.b.getStream());

    if (pair.a.getInput())
        pair.b.setRelayStream(nullptr);
    else
        pair.b.setRelayStream(pair.a.getStream());

    // stereo recorders track the current leg's tag; refresh on wiring change
    if (auto *sa = pair.a.getStream())
        sa->updateStereoRecorders();
    if (auto *sb = pair.b.getStream())
        sb->updateStereoRecorders();
}

void AmB2BMedia::updateAudioStreams()
{
    // SDP was updated
    TRACE("handling SDP change, A leg: %c%c, B leg: %c%c\n", have_a_leg_local_sdp ? 'X' : '-',
          have_a_leg_remote_sdp ? 'X' : '-', have_b_leg_local_sdp ? 'X' : '-', have_b_leg_remote_sdp ? 'X' : '-');

    // if we have all necessary information we can initialize streams and start
    // their processing
    if (streams.empty())
        return; // no streams

    bool have_a = have_a_leg_local_sdp && have_a_leg_remote_sdp;
    bool have_b = have_b_leg_local_sdp && have_b_leg_remote_sdp;

    if (!((have_a || have_b)))
        return;

    bool needs_processing = a && b && have_a && have_b && a->getRtpRelayMode() == AmB2BSession::RTP_Transcoding;

    // initialize streams to be able to relay & transcode (or use local audio)
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        pair.a.stopStreamProcessing();
        pair.b.stopStreamProcessing();

        initPairStream(pair);
        syncPairWiring(pair);

        if (pair.requiresProcessing())
            needs_processing = true;

        pair.a.resumeStreamProcessing();
        pair.b.resumeStreamProcessing();
    }

    // start media processing (only if transcoding or regular audio processing
    // required)
    // Note: once we send local SDP to the other party we have to expect RTP but
    // we need to be fully initialised (both legs) before we can correctly handle
    // the media, right?
    if (needs_processing) {
        addToMediaProcessorUnsafe();
    } else {
        AmMediaProcessor::instance()->removeSession(this);
    }
}

void AmB2BMedia::updateRelayStream(AmRtpStream *stream, AmB2BSession *session, const string &connection_address,
                                   const SdpMedia &m, AmRtpStream *relay_to)
{
    static const PayloadMask true_mask(true);

    if (!stream) {
        return;
    }

    stream->stopReceiving();
    if (m.port) {
        stream->setRelayStream(relay_to);
        if (relay_to)
            relay_to->setRelayStream(stream);
        stream->setRelayPayloads(true_mask);
        if (!relay_paused)
            stream->enableRtpRelay();
        stream->setRAddr(connection_address, static_cast<unsigned short>(m.port));
        if ((m.transport != TP_RTPAVP) && !m.is_simple_srtp() && !m.is_dtls_srtp())
            stream->setRawRelay(true);
        if (session) {
            // propagate session settings
            stream->getEndpoint()->setPassiveMode(session->getRtpRelayForceSymmetricRtp());
            stream->setRtpRelayTransparentSeqno(session->getRtpRelayTransparentSeqno());
            stream->setRtpRelayTransparentSSRC(session->getRtpRelayTransparentSSRC());
        }
        stream->setLogger(logger);
        stream->getEndpoint()->setSklfile(sklfile);
        stream->resumeReceiving();
    } else {
        DBG("disabled stream");
    }
}

void AmB2BMedia::createUpdateStreams(bool a_leg, const AmSdp &local_sdp, const AmSdp &remote_sdp, RelayController *ctrl,
                                     bool sdp_offer_owner)
{
    TRACE("%s (%c): create/updating streams with local & remote SDP\n",
          a_leg ? (a ? a->getLocalTag().c_str() : NULL) : (b ? b->getLocalTag().c_str() : NULL), a_leg ? 'A' : 'B');

    AmLock lock(mutex);

    if (a_leg) {
        a_leg_local_sdp       = local_sdp;
        a_leg_remote_sdp      = remote_sdp;
        have_a_leg_local_sdp  = true;
        have_a_leg_remote_sdp = true;
    } else {
        b_leg_local_sdp       = local_sdp;
        b_leg_remote_sdp      = remote_sdp;
        have_b_leg_local_sdp  = true;
        have_b_leg_remote_sdp = true;
    }

    createStreams(local_sdp);
    updateStreamsUnsafe(a_leg, ctrl, sdp_offer_owner);
}

void AmB2BMedia::updateStreams(bool a_leg, RelayController *ctrl, bool sdp_offer_owner)
{
    AmLock l(mutex);
    updateStreamsUnsafe(a_leg, ctrl, sdp_offer_owner);
}

void AmB2BMedia::applyStateTransitions()
{
    // Both legs must have completed negotiation. Otherwise pair state stays put.
    if (!(have_a_leg_local_sdp && have_a_leg_remote_sdp && have_b_leg_local_sdp && have_b_leg_remote_sdp))
        return;

    auto sdp_managed = [this](const AmSdp &sdp, int idx) -> bool {
        if (static_cast<size_t>(idx) >= sdp.media.size())
            return false;
        const auto &m = sdp.media[idx];
        if (m.port == 0 && !m.use_bundle) // bundle-only member may carry port=0
            return false;
        if (m.type == MT_AUDIO)
            return true;
        return !ignore_relay_streams && canRelay(m);
    };

    int idx = 0;
    for (auto &p : streams) {
        bool managed = sdp_managed(a_leg_local_sdp, idx) && sdp_managed(a_leg_remote_sdp, idx) &&
                       sdp_managed(b_leg_local_sdp, idx) && sdp_managed(b_leg_remote_sdp, idx);

        StreamData::State desired;
        if (managed) {
            // both legs agree on kind; audio in local means audio-managed pair.
            desired = (a_leg_local_sdp.media[idx].type == MT_AUDIO) ? StreamData::ActiveAudio : StreamData::ActiveRelay;
        } else {
            desired = p.empty() ? StreamData::Empty : StreamData::Inactive;
        }

        p.a.transition(desired);
        p.b.transition(desired);
        ++idx;
    }
}

void AmB2BMedia::updateStreamsUnsafe(bool a_leg, RelayController *ctrl, bool sdp_offer_owner)
{
    applyStateTransitions();

    const AmSdp &remote_sdp = a_leg ? a_leg_remote_sdp : b_leg_remote_sdp;

    // compute relay mask for every stream
    // Warning: do not apply the new mask unless the offer answer succeeds?
    // we can safely apply the changes once we have local & remote SDP (i.e. the
    // negotiation is finished) otherwise we might handle the RTP in a wrong way

    for (auto &p : streams)
        (a_leg ? p.a : p.b).setSdpOfferOwner(sdp_offer_owner);

    // streams and remote_sdp.media are 1:1 by index; kind of the slot is taken
    // from the current m-line, not from the pair's cached state (may lag on reinvite).
    auto pair_it = streams.begin();
    int  idx     = 0;
    for (auto m = remote_sdp.media.begin(); m != remote_sdp.media.end() && pair_it != streams.end();
         ++m, ++pair_it, ++idx)
    {
        const string &connection_address = (m->conn.address.empty() ? remote_sdp.conn.address : m->conn.address);
        if (m->type == MT_AUDIO) {
            DBG("updateStreams() processing audio stream %d", idx);
            DBG("[%p] updateStreams() update AudioStreamPair %p/%p", static_cast<void *>(this),
                static_cast<void *>(pair_it->a.getStream()), static_cast<void *>(pair_it->b.getStream()));

            // initialize relay mask in the other(!) leg and relay destination for stream in current leg
            TRACE("relay payloads in direction %s\n", a_leg ? "B -> A" : "A -> B");

            if (a_leg) {
                pair_it->b.setRelayPayloads(*m, ctrl);
                pair_it->a.setRelayDestination(connection_address, static_cast<int>(m->port));
            } else {
                pair_it->a.setRelayPayloads(*m, ctrl);
                pair_it->b.setRelayDestination(connection_address, static_cast<int>(m->port));
            }
        } else {
            DBG("updateStreams() processing non-audio stream %d", idx);
            if (ignore_relay_streams)
                continue;
            if (!canRelay(*m))
                continue;

            pair_it->a.stopStreamProcessing();
            pair_it->b.stopStreamProcessing();

            if (a_leg) {
                DBG("[%p] updating A-leg relay_stream %d. %p", static_cast<void *>(this), idx,
                    static_cast<void *>(pair_it->a.getStream()));
                updateRelayStream(pair_it->a.getStream(), a, connection_address, *m, pair_it->b.getStream());
            } else {
                DBG("[%p] updating B-leg relay_stream %d. %p", static_cast<void *>(this), idx,
                    static_cast<void *>(pair_it->b.getStream()));
                updateRelayStream(pair_it->b.getStream(), b, connection_address, *m, pair_it->a.getStream());
            }

            initPairStream(*pair_it);

            pair_it->a.resumeStreamProcessing();
            pair_it->b.resumeStreamProcessing();
        }
    } // iterate remote_sdp.media

    updateAudioStreams();
    TRACE("streams updated with SDP");
}

void AmB2BMedia::stop(bool a_leg)
{
    TRACE("stop %s leg\n", a_leg ? "A" : "B");
    clearAudio(a_leg);
    // remove from processor only if both A and B leg stopped
    if ((!a) && (!b)) {
        AmMediaProcessor::instance()->removeSession(this);
    }
}

void AmB2BMedia::onMediaSessionExists()
{
    AmMediaSession::onMediaSessionExists();
    // release reference held by AmMediaProcessor
    // aquired by addToMediaProcessor() or addToMediaProcessorUnsafe()
    releaseReference();
}

void AmB2BMedia::onMediaProcessingTerminated()
{
    AmMediaSession::onMediaProcessingTerminated();

    // release reference held by AmMediaProcessor
    // aquired by addToMediaProcessor() or addToMediaProcessorUnsafe()
    releaseReference();
}

bool AmB2BMedia::replaceOffer(AmSdp &sdp, bool a_leg)
{
    TRACE("replacing offer with a local one\n");
    AmLock lock(mutex);

    createStreams(sdp);
    try {
        auto pair = streams.begin();
        for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m, ++pair) {
            if (m->type == MT_AUDIO && pair != streams.end()) {
                if (!pair->audio())
                    continue;

                // generate our local offer
                TRACE("... making audio stream offer\n");
                if (a_leg)
                    pair->a.getSdpOffer(*m);
                else
                    pair->b.getSdpOffer(*m);
            } else {
                TRACE("... making non-audio/uninitialised stream inactive\n");
                m->send = false;
                m->recv = false;
            }
        }
    } catch (...) {
        TRACE("hold SDP offer creation failed\n");
        return true;
    }

    TRACE("hold SDP offer generated\n");
    return true;
}

bool AmB2BMedia::haveLocalSdp(bool a_leg)
{
    if (a_leg)
        return have_a_leg_local_sdp;
    else
        return have_b_leg_local_sdp;
}

bool AmB2BMedia::haveRemoteSdp(bool a_leg)
{
    if (a_leg)
        return have_a_leg_remote_sdp;
    else
        return have_b_leg_remote_sdp;
}

const AmSdp &AmB2BMedia::getLocalSdp(bool a_leg)
{
    if (a_leg)
        return a_leg_local_sdp;
    else
        return b_leg_local_sdp;
}

const AmSdp &AmB2BMedia::getRemoteSdp(bool a_leg)
{
    if (a_leg)
        return a_leg_remote_sdp;
    else
        return b_leg_remote_sdp;
}

void AmB2BMedia::setMuteFlag(bool a_leg, bool set)
{
    AmLock lock(mutex);
    if (a_leg)
        a_leg_muted = set;
    else
        b_leg_muted = set;
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;
        if (a_leg)
            pair.a.mute(set);
        else
            pair.b.mute(set);
    }
}

void AmB2BMedia::setRtpTimeout(bool a_leg, unsigned int timeout)
{
    AmLock lock(mutex);
    for (auto &pair : streams)
        pair.setRtpTimeout(a_leg, timeout);
}

void AmB2BMedia::setRtpTimeout(unsigned int timeout)
{
    AmLock lock(mutex);
    for (auto &pair : streams)
        pair.setRtpTimeout(timeout);
}

void AmB2BMedia::setMonitorRtpTimeout(bool enable)
{
    AmLock lock(mutex);
    for (auto &pair : streams)
        pair.setMonitorRtpTimeout(enable);
}

void AmB2BMedia::setFirstStreamInput(bool a_leg, AmAudio *in)
{
    AmLock lock(mutex);

    bool found = false;
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;

        found = true;
        if (a_leg)
            pair.a.setInput(in);
        else
            pair.b.setInput(in);
        // in changed → relay wiring in the other leg needs to reflect it
        syncPairWiring(pair);

        break;
    }

    if (!found && in) {
        ERROR("BUG: can't set %s leg's first stream input, no streams", a_leg ? "A" : "B");
    }
}

void AmB2BMedia::setFirstStreamOutput(bool a_leg, AmAudio *out)
{
    AmLock lock(mutex);

    bool found = false;
    for (auto &pair : streams) {
        if (!pair.audio())
            continue;

        found = true;
        if (a_leg)
            pair.a.setOutput(out);
        else
            pair.b.setOutput(out);
        // out changed → relay wiring in the other leg needs to reflect it
        syncPairWiring(pair);

        break;
    }

    if (!found && out) {
        ERROR("BUG: can't set %s leg's first stream output, no streams", a_leg ? "A" : "B");
    }
}

void AmB2BMedia::createHoldAnswer(bool a_leg, const AmSdp &offer, AmSdp &answer, bool use_zero_con)
{
    // because of possible RTP relaying our payloads need not to match the remote
    // party's payloads (i.e. we might need not understand the remote party's
    // codecs)
    // As a quick hack we may use just copy of the original SDP with all streams
    // deactivated to avoid sending RTP to us (twinkle requires at least one
    // non-disabled stream in the response so we can not set all ports to 0 to
    // signalize that we don't want to receive anything)

    AmLock lock(mutex);

    answer = offer;
    answer.media.clear();

    if (use_zero_con) {
        answer.conn.address = zero_ip;
    } else {
        if (a_leg) {
            if (a)
                answer.conn.address = a->RTPStream()->getEndpoint()->getLocalAddress();
        } else {
            if (b)
                answer.conn.address = b->RTPStream()->getEndpoint()->getLocalAddress();
        }
        if (answer.conn.address.empty())
            answer.conn.address = zero_ip; // we need something there
    }

    auto                             pair = streams.begin();
    vector<SdpMedia>::const_iterator m;
    for (m = offer.media.begin(); m != offer.media.end(); ++m) {
        answer.media.push_back(SdpMedia());
        SdpMedia &media = answer.media.back();
        media.type      = m->type;

        if (media.type != MT_AUDIO) { // copy whole media line except port
            media      = *m;
            media.port = 0;
            continue;
        }

        if (m->port == 0) { // copy whole inactive media line
            media = *m;
            while (!pair->audio() || pair != streams.end())
                ++pair;
            continue;
        }

        if (pair == streams.end()) {
            ERROR("audio streams less then media lines in sdp");
            return;
        }

        if (a_leg)
            pair->a.getSdpAnswer(*m, media);
        else
            pair->b.getSdpAnswer(*m, media);

        media.send = false; // should be already because the stream should be on hold
        media.recv = false; // what we would do with received data?

        if (media.payloads.empty()) {
            // we have to add something there
            if (!m->payloads.empty())
                media.payloads.push_back(m->payloads[0]);
        }
        break;
    }
}

void AmB2BMedia::setRtpLogger(msg_logger *_logger)
{
    DBG("AmB2BMedia::setRtpLogger");

    AmLock lock(mutex);

    if (logger)
        dec_ref(logger);
    logger = _logger;
    if (logger)
        inc_ref(logger);

    // walk through all the streams and use logger for them
    for (auto &pair : streams)
        pair.setLogger(logger);
}

void AmB2BMedia::setSklLogger(SSLKeyLogger *logger)
{
    DBG("AmB2BMedia::setSklLogger");

    AmLock lock(mutex);

    if (sklfile)
        dec_ref(sklfile);
    if (logger)
        inc_ref(logger);
    sklfile = logger;

    // walk through all the streams and use logger for them
    for (auto &pair : streams)
        pair.setSklLogger(logger);
}

void AmB2BMedia::setRtpASensor(msg_sensor *_sensor)
{
    DBG("AmB2BMedia: change B sensors to %p", static_cast<void *>(_sensor));

    AmLock lock(mutex);
    if (asensor)
        dec_ref(asensor);
    asensor = _sensor;
    if (asensor)
        inc_ref(asensor);

    // walk through all the streams and apply sensor for them
    for (auto &pair : streams)
        pair.setASensor(asensor);
}

void AmB2BMedia::setRtpBSensor(msg_sensor *_sensor)
{
    DBG("AmB2BMedia: change B sensors to %p", static_cast<void *>(_sensor));

    AmLock lock(mutex);
    if (bsensor)
        dec_ref(bsensor);
    bsensor = _sensor;
    if (bsensor)
        inc_ref(bsensor);

    // walk through all the streams and apply sensor for them
    for (auto &pair : streams)
        pair.setBSensor(bsensor);
}

void AmB2BMedia::setRelayDTMFReceiving(bool enabled)
{
    AmLock lock(mutex);

    DBG("streams.size() = %zd", streams.size());
    for (auto &pair : streams) {
        DBG("force_receive_dtmf %sabled for [%p]", enabled ? "en" : "dis", static_cast<void *>(&pair.a));
        DBG("force_receive_dtmf %sabled for [%p]", enabled ? "en" : "dis", static_cast<void *>(&pair.b));
        pair.a.getStream()->force_receive_dtmf = enabled;
        pair.b.getStream()->force_receive_dtmf = enabled;
    }
}

/** set receving of RTP/relay streams (not receiving=drop incoming packets) */
void AmB2BMedia::setReceiving(bool receiving_a, bool receiving_b)
{
    AmLock lock(mutex); // TODO: is this necessary?

    DBG("streams.size() = %zd", streams.size());

    for (auto &pair : streams) {
        if (!pair.audio())
            DBG("setReceiving(%s) A relay stream [%p]", receiving_a ? "true" : "false",
                static_cast<void *>(pair.a.getStream()));
        else
            DBG("setReceiving(%s) A audio stream [%p]", receiving_a ? "true" : "false",
                static_cast<void *>(pair.a.getStream()));

        pair.a.setReceiving(receiving_a);

        if (!pair.audio())
            DBG("setReceiving(%s) B relay stream [%p]", receiving_b ? "true" : "false",
                static_cast<void *>(pair.a.getStream()));
        else
            DBG("setReceiving(%s) B audio stream [%p]", receiving_b ? "true" : "false",
                static_cast<void *>(pair.a.getStream()));

        pair.b.setReceiving(receiving_b);
    }
}

void AmB2BMedia::setIgnoreRelayStreams(bool ignore)
{
    ignore_relay_streams = ignore;
    DBG("relay streams ignore %sabled", ignore ? "en" : "dis");
}

void AmB2BMedia::pauseRelay()
{
    AmLock lock(mutex);

    DBG("streams.size() = %zd", streams.size());
    relay_paused = true;

    for (auto &pair : streams) {
        if (pair.audio()) {
            pair.a.setRelayPaused(true);
            pair.b.setRelayPaused(true);
        } else {
            pair.a.getStream()->setRawRelay(false);
            pair.b.getStream()->setRawRelay(false);
        }
    }
}

void AmB2BMedia::restartRelay()
{
    AmLock lock(mutex);

    DBG("streams.size() = %zd", streams.size());

    relay_paused = false;

    for (auto &pair : streams) {
        if (pair.audio()) {
            pair.a.setRelayPaused(false);
            pair.b.setRelayPaused(false);
        } else {
            pair.a.getStream()->setRawRelay(true);
            pair.b.getStream()->setRawRelay(true);
        }
    }
}

// print debug info
void AmB2BMedia::debug()
{
    AmLock lock(mutex);
    // walk through all the streams
    DBG("B2B media session %p ('%s' <-> '%s'):", static_cast<void *>(this), a ? a->getLocalTag().c_str() : "?",
        b ? b->getLocalTag().c_str() : "?");

    DBG("\tOA status: %c%c / %c%c", have_a_leg_local_sdp ? 'X' : '-', have_a_leg_remote_sdp ? 'X' : '-',
        have_b_leg_local_sdp ? 'X' : '-', have_b_leg_remote_sdp ? 'X' : '-');

    for (auto &pair : streams) {
        if (pair.audio())
            DBG(" - audio stream (A):");
        else
            DBG(" - relay stream (A):");
        pair.a.debug();
        if (pair.audio())
            DBG(" - audio stream (B):");
        else
            DBG(" - relay stream (B):");
        pair.b.debug();
    }
}

void AmB2BMedia::getInfo(AmArg &ret)
{
    ret["a_tag"] = a ? a->getLocalTag() : "nullptr";
    ret["b_tag"] = b ? b->getLocalTag() : "nullptr";

    AmArg &arg_audio         = ret["audio_streams"];
    AmArg &arg_relay_streams = ret["relay_streams"];
    arg_audio.assertArray();
    arg_relay_streams.assertArray();

    AmLock lock(mutex);
    for (auto &pair : streams) {
        AmArg *arg;
        if (pair.audio())
            arg = &arg_audio;
        else
            arg = &arg_relay_streams;

        arg->push(AmArg());
        AmArg &u = arg_audio.back();
        pair.a.getInfo(u["a"]);
        pair.b.getInfo(u["b"]);
        if (pair.audio()) {
            u["media_idx"] = pair.media_idx;
        }
    }

#define add_sdp_info(var)                                                                                              \
    if (have_##var) {                                                                                                  \
        AmArg &a = ret[#var];                                                                                          \
        var.getInfo(a);                                                                                                \
    } else {                                                                                                           \
        ret[#var] = "empty";                                                                                           \
    }

    add_sdp_info(a_leg_local_sdp);
    add_sdp_info(a_leg_remote_sdp);
    add_sdp_info(b_leg_local_sdp);
    add_sdp_info(b_leg_remote_sdp);

#undef add_sdp_info
}
