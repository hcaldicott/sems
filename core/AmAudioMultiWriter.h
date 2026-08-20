#ifndef _AmAudioMultiWriter_h_
#define _AmAudioMultiWriter_h_

#include "AmAudio.h"

/**
 * \brief \ref AmAudio which duplicates the written audio to two sinks
 */
class AmAudioMultiWriter : public AmAudio {
    AmAudio *a;
    AmAudio *b;

  protected:
    // not used
    int read(unsigned int user_ts, unsigned int size) { return -1; }
    int write(unsigned int user_ts, unsigned int size) { return -1; }

    // override AmAudio
    int get(unsigned long long system_ts, unsigned char *buffer, int output_sample_rate, unsigned int nb_samples);

    int put(unsigned long long system_ts, unsigned char *buffer, int input_sample_rate, unsigned int size);

  public:
    AmAudioMultiWriter(AmAudio *a, AmAudio *b);
    ~AmAudioMultiWriter();

    AmAudio *getA() const { return a; }
    AmAudio *getB() const { return b; }
};

#endif // _AmAudioMultiWriter_h_
