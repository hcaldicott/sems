/*
 * Copyright (C) 2002-2003 Fhg Fokus
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
 * For a license to use the SEMS software under conditions
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

#include "AmAdvancedAudio.h"

#include <set>
using std::set;

#include "math.h"

/* AudioBridge */
AmAudioBridge::AmAudioBridge(unsigned int sample_rate)
    : AmAudio(new AmAudioFormat(CODEC_PCM16, sample_rate))
{
    sarr.clear_all();
}

AmAudioBridge::~AmAudioBridge() {}

int AmAudioBridge::write(unsigned int user_ts, unsigned int size)
{
    sarr.write(user_ts, (short *)((unsigned char *)samples), size >> 1);
    return size;
}

int AmAudioBridge::read(unsigned int user_ts, unsigned int size)
{
    sarr.read(user_ts, (short *)((unsigned char *)samples), size >> 1);
    return size;
}

/* AudioDelay */
AmAudioDelay::AmAudioDelay(float delay_sec, unsigned int sample_rate)
    : AmAudio(new AmAudioFormat(CODEC_PCM16, sample_rate))
{
    sarr.clear_all();
    delay = delay_sec;
}

AmAudioDelay::~AmAudioDelay() {}

int AmAudioDelay::write(unsigned int user_ts, unsigned int size)
{
    sarr.write(user_ts, (short *)((unsigned char *)samples), size >> 1);
    return size;
}

int AmAudioDelay::read(unsigned int user_ts, unsigned int size)
{
    sarr.read((unsigned int)(user_ts - delay * (float)getSampleRate()), (short *)((unsigned char *)samples), size >> 1);
    return size;
}

AmAudioFrontlist::AmAudioFrontlist(AmEventQueue *q)
    : AmPlaylist(q)
    , back_audio(NULL)
{
}

AmAudioFrontlist::AmAudioFrontlist(AmEventQueue *q, AmAudio *back_audio)
    : AmPlaylist(q)
    , back_audio(back_audio)
{
}

AmAudioFrontlist::~AmAudioFrontlist()
{
    //   ba_mut.lock();
    //   if (back_audio)
    //     back_audio->close();
    //   ba_mut.unlock();
}

void AmAudioFrontlist::setBackAudio(AmAudio *new_ba)
{
    ba_mut.lock();
    back_audio = new_ba;
    ba_mut.unlock();
}

int AmAudioFrontlist::put(unsigned long long system_ts, unsigned char *buffer, int input_sample_rate, unsigned int size)
{

    // stay consistent with Playlist - if empty return size
    int res = size;
    ba_mut.lock();

    if (isEmpty()) {
        if (back_audio)
            res = back_audio->put(system_ts, buffer, input_sample_rate, size);
    } else {
        res = AmPlaylist::put(system_ts, buffer, input_sample_rate, size);
    }

    ba_mut.unlock();
    return res;
}

int AmAudioFrontlist::get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate,
                          unsigned int nb_samples)
{

    // stay consistent with Playlist - if empty return size
    int res = nb_samples;

    ba_mut.lock();
    if (isEmpty() && back_audio) {
        res = back_audio->get(system_ts, buffer, output_sample_rate, nb_samples);
    } else {
        res = AmPlaylist::get(system_ts, buffer, output_sample_rate, nb_samples);
    }
    ba_mut.unlock();
    return res;
}


int AmNullAudio::put(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate, unsigned int size)
{
    // need to stop at some point?

    if (stereo_record_enabled) {
        stereo_recorders.put(system_ts, buffer, size, output_sample_rate);
    }

    if (write_msec < 0)
        return size;

    if (!write_end_ts_i) {
        write_end_ts_i = true;
        write_end_ts   = system_ts + (write_msec * WALLCLOCK_RATE) / 1000;
    }

    if (!sys_ts_less()(system_ts, write_end_ts)) {
        DBG("%dms of silence ended (write)", write_msec);
        return -1;
    }

    return size;
}

int AmNullAudio::get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate,
                     unsigned int nb_samples)
{
    int size = (int)(nb_samples << 1);

    // need to stop at some point?
    if (read_msec < 0) {
        memset((unsigned char *)buffer, 0, size);
        return size;
    }

    if (!read_end_ts_i) {
        read_end_ts_i = true;
        read_end_ts   = system_ts + (read_msec * WALLCLOCK_RATE) / 1000;
    }

    if (!sys_ts_less()(system_ts, read_end_ts)) {
        DBG("%dms of silence ended (read)", read_msec);
        return -1;
    }

    memset((unsigned char *)buffer, 0, size);
    return size;
}

void AmNullAudio::setReadLength(int n_msec)
{
    read_msec     = n_msec;
    read_end_ts_i = false;
}

void AmNullAudio::setWriteLength(int n_msec)
{
    write_msec     = n_msec;
    write_end_ts_i = false;
}
