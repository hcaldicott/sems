/*
 * Copyright (C) 2008 IPTEGO GmbH
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the sems software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmAudioMixer.h"

AmAudioMixer::AmAudioMixer(int external_sample_rate)
{
    // sink_channel   = mixer.addChannel(external_sample_rate);
    // sink_connector = new AmAudioMixerConnector(mixer, sink_channel, NULL, &srcsink_mut, &sinks);
}

AmAudioMixer::~AmAudioMixer()
{
    // mixer.removeChannel(sink_channel);
    for (std::map<AmAudioMixerConnector *, unsigned int>::iterator it = sources.begin(); it != sources.end(); it++) {
        mixer.removeChannel(it->second);
        delete it->first;
    }
    // delete sink_connector;
}

AmAudio *AmAudioMixer::addSource(int external_sample_rate)
{
    // srcsink_mut.lock();
    unsigned int src_channel = mixer.addChannel(external_sample_rate);
    // the first source will process the media in the mixer channel
    AmAudioMixerConnector *conn =
        new AmAudioMixerConnector(mixer, src_channel /*, sources.empty() ? sink_connector : NULL*/);
    sources[conn] = src_channel;
    // srcsink_mut.unlock();
    return conn;
}

void AmAudioMixer::releaseSource(AmAudio *s)
{
    // srcsink_mut.lock();
    std::map<AmAudioMixerConnector *, unsigned int>::iterator it = sources.find((AmAudioMixerConnector *)s);
    if (it == sources.end()) {
        // srcsink_mut.unlock();
        ERROR("source [%p] is not part of this mixer.", s);
        return;
    }
    mixer.removeChannel(it->second);
    delete s;
    sources.erase(it);
    // srcsink_mut.unlock();
}
/*
void AmAudioMixer::addSink(AmAudio *s)
{
    srcsink_mut.lock();
    sinks.insert(s);
    srcsink_mut.unlock();
}

void AmAudioMixer::releaseSink(AmAudio *s)
{
    srcsink_mut.lock();
    sinks.erase(s);
    srcsink_mut.unlock();
}
*/

int AmAudioMixerConnector::get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate,
                               unsigned int nb_samples)
{
    // in fact GCP here only needed for the mixed channel
    // unsigned int mixer_sample_rate;
    // mixer.GetChannelPacket(channel, system_ts, buffer, nb_samples, mixer_sample_rate);


    int mixer_rate = mixer.GetCurrentSampleRate();
    if (mixer_rate <= 0 || output_sample_rate <= 0)
        return 0;

    // the requested amount of audio, expressed at the rate the mixer works at
    unsigned int size = PCM16_S2B(
        static_cast<unsigned int>((static_cast<unsigned long long>(nb_samples) * static_cast<unsigned>(mixer_rate)) /
                                  static_cast<unsigned>(output_sample_rate)));

    // CLASS_DBG("get: output_sample_rate %d, nb_samples %d, mixer_rate %d, size %u", output_sample_rate, nb_samples,
    // mixer_rate, size);

    // both the mixer's and the resampled representation are held in the caller's buffer
    if (!size || size > AUDIO_BUFFER_SIZE || PCM16_S2B(nb_samples) > AUDIO_BUFFER_SIZE) {
        ERROR("mixer channel %u: bad read size %u (%u samples @ %d Hz, mixer %d Hz)", channel, size, nb_samples,
              output_sample_rate, mixer_rate);
        return 0;
    }

    // in fact GCP here only needed for the mixed channel;
    // updates size and reports the rate the samples are actually in
    unsigned int got_sample_rate = static_cast<unsigned int>(mixer_rate);
    mixer.GetChannelPacket(channel, system_ts, buffer, size, got_sample_rate);

    if (!size)
        return 0;

    // CLASS_DBG("get, GCP: got_sample_rate %d, size %u", got_sample_rate, size);


    /*if ((audio_mut != NULL) && (sinks != NULL)) {
        audio_mut->lock();
        // write to all sinks
        for (std::set<AmAudio *>::iterator it = sinks->begin(); it != sinks->end(); it++) {
            (*it)->put(system_ts, buffer, output_sample_rate, nb_samples);
            //(*it)->put(system_ts, buffer, static_cast<int>(got_sample_rate), size);
        }
        audio_mut->unlock();
    }*/

    // resampled in place: the caller's buffer is AUDIO_BUFFER_SIZE and nb_samples fit it
    size = resampleOutput(buffer, size, static_cast<int>(got_sample_rate), output_sample_rate);

    // CLASS_DBG("get, resample: size %u", size);

    return static_cast<int>(size);
    // return nb_samples;
}

int AmAudioMixerConnector::put(unsigned long long system_ts, unsigned char *buffer, int input_sample_rate,
                               unsigned int size)
{
    // mixer.PutChannelPacket(channel, system_ts, buffer, size);

    int mixer_rate = mixer.GetCurrentSampleRate();
    if (mixer_rate <= 0 || input_sample_rate <= 0 || !size)
        return 0;

    unsigned int   put_size = size;
    unsigned char *put_buf  = buffer;

    // CLASS_DBG("put: input_sample_rate %d, mixer_rate %d, size %u", input_sample_rate, mixer_rate, size);

    // never resample the caller's buffer in place, it is reused afterwards
    unsigned char resampled[AUDIO_BUFFER_SIZE];
    if (input_sample_rate != mixer_rate) {

        // resampling is done in place, so the result has to fit as well
        unsigned long long resampled_size =
            (static_cast<unsigned long long>(size) * static_cast<unsigned>(mixer_rate)) /
            static_cast<unsigned>(input_sample_rate);

        if (size > sizeof(resampled) || resampled_size > sizeof(resampled)) {
            ERROR("mixer channel %u: write of %u bytes (%llu resampled) exceeds the resampling buffer", channel, size,
                  resampled_size);
            return -1;
        }
        memcpy(resampled, buffer, size);
        put_buf  = resampled;
        put_size = resampleInput(resampled, size, input_sample_rate, mixer_rate);
        if (!put_size)
            return static_cast<int>(size);

        CLASS_DBG("put, resample: put_size %u, size %u", put_size, size);
    }

    if (put_size > AUDIO_BUFFER_SIZE) {
        ERROR("mixer channel %u: write of %u bytes exceeds the mixer buffer", channel, put_size);
        return -1;
    }

    mixer.PutChannelPacket(channel, system_ts, put_buf, put_size);

    /*if (mix_channel != NULL) {
        // we are processing the media of the mixed channel as well
        ShortSample mix_buffer[SIZE_MIX_BUFFER];
        mix_channel->get(system_ts, (unsigned char *)mix_buffer, input_sample_rate, size);
        // mix_channel->get(system_ts, (unsigned char *)mix_buffer, mixer_rate, PCM16_B2S(put_size));
    }*/

    return static_cast<int>(size);
    // return size;
}
