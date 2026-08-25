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

#ifndef _AmAudioMixer_h_
#define _AmAudioMixer_h_

#include "AmMultiPartyMixer.h"
#include "AmThread.h"

#include <map>
#include <set>

class AmAudioMixerConnector;

/**
 * \brief N-way audio mixer exposed as a set of \ref AmAudio devices
 *
 * AmAudioMixer wraps an \ref AmMultiPartyMixer and hands out one
 * \ref AmAudioMixerConnector per participant via addSource(). Every connector
 * is a plain AmAudio device that can be plugged wherever an AmAudio is
 * expected (session input/output, playlist, audio queue, ...).
 *
 * Everything written to a connector is fed into that connector's mixer
 * channel; everything read from it is the mix of all the *other* channels,
 * i.e. a participant never hears itself back.
 *
 * The connectors resample between the rate of the AmAudio side and the rate
 * the mixer currently runs at (the highest rate of all registered channels),
 * so channels with different sample rates may be mixed together.
 *
 * The mixer owns its connectors: they are created by addChannel(), destroyed
 * by releaseChannel(), and any remaining ones are destroyed together with the
 * mixer. addChannel()/releaseChannel() may be called concurrently from
 * different threads (the mixing itself is protected by AmMultiPartyMixer).
 * Releasing a connector that is still wired somewhere is the caller's problem.
 */
class AmAudioMixer {
    AmMultiPartyMixer                               mixer;
    AmMutex                                         channels_mut;
    std::map<AmAudioMixerConnector *, unsigned int> channels;

  public:
    AmAudioMixer(int external_sample_rate);
    ~AmAudioMixer();

    AmAudio *addChannel(int external_sample_rate);
    void     releaseChannel(AmAudio *s);
};

class AmAudioMixerConnector : public AmAudio {
    AmMultiPartyMixer &mixer;
    unsigned int       channel;

  protected:
    int get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate, unsigned int nb_samples);
    int put(unsigned long long system_ts, unsigned char *buffer, int input_sample_rate, unsigned int size);

    // dummies for AmAudio's pure virtual methods
    int read(unsigned int user_ts, unsigned int size) { return -1; }
    int write(unsigned int user_ts, unsigned int size) { return -1; }

  public:
    AmAudioMixerConnector(AmMultiPartyMixer &mixer, unsigned int channel)
        : mixer(mixer)
        , channel(channel)
    {
    }
    ~AmAudioMixerConnector() {}
};

#endif
