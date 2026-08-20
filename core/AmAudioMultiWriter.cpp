#include "AmAudioMultiWriter.h"
#include "log.h"

AmAudioMultiWriter::AmAudioMultiWriter(AmAudio *a, AmAudio *b)
    : a(a)
    , b(b)
{
}

AmAudioMultiWriter::~AmAudioMultiWriter() {}

int AmAudioMultiWriter::get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate,
                            unsigned int nb_samples)
{
    ERROR("reading not supported");
    return -1;
}

int AmAudioMultiWriter::put(unsigned long long system_ts, unsigned char *buffer, int input_sample_rate,
                            unsigned int size)
{
    int ret = 0;

    if (a) {
        ret = a->put(system_ts, buffer, input_sample_rate, size);
    }

    if (b) {
        int b_ret = b->put(system_ts, buffer, input_sample_rate, size);
        if (ret >= 0 && b_ret < 0)
            ret = b_ret;
    }

    return ret;
}
