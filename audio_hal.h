#ifndef __AUDIO_HAL_H__
#define __AUDIO_HAL_H__

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>
//#include <audio_route/audio_route.h>

#include <pthread.h>

// Additionnal latency introduced by audio DSP and hardware in ms
#define AUDIO_HW_OUT_LATENCY_MS 0
// Default audio output sample rate
#define AUDIO_HW_OUT_SAMPLERATE 44100
// Default audio output channel mask
#define AUDIO_HW_OUT_CHANNELS (AUDIO_CHANNEL_OUT_STEREO)
// Default audio output sample format
#define AUDIO_HW_OUT_FORMAT (AUDIO_FORMAT_PCM)//(AudioSystem::PCM_16_BIT)
// Kernel pcm out buffer size in frames at 44.1kHz
#define AUDIO_HW_OUT_PERIOD_SZ 2048     // <== 1024
#define AUDIO_HW_OUT_PERIOD_CNT 4
// Default audio output buffer size in bytes
#define AUDIO_HW_OUT_PERIOD_BYTES (AUDIO_HW_OUT_PERIOD_SZ * 2 * sizeof(int16_t))

// Default audio input sample rate
#define AUDIO_HW_IN_SAMPLERATE 44100
// Default audio input channel mask
#define AUDIO_HW_IN_CHANNELS (AUDIO_CHANNEL_IN_MONO)//(AudioSystem::CHANNEL_IN_MONO)
// Default audio input sample format
#define AUDIO_HW_IN_FORMAT (AUDIO_FORMAT_PCM)//(AudioSystem::PCM_16_BIT)
// Kernel pcm in buffer size in frames at 44.1kHz (before resampling)
#define AUDIO_HW_IN_PERIOD_SZ 2048      // <== 1024
#define AUDIO_HW_IN_PERIOD_CNT 2
// Default audio input buffer size in bytes (8kHz mono)
#define AUDIO_HW_IN_PERIOD_BYTES ((AUDIO_HW_IN_PERIOD_SZ*sizeof(int16_t))/8)

#define PCM_CARD 0
#define PCM_CARD_SPDIF 1
#define PCM_TOTAL 2

#define PCM_DEVICE 0
#define PCM_DEVICE_DEEP 1
#define PCM_DEVICE_VOICE 2
#define PCM_DEVICE_SCO 3

#define MIXER_CARD 0

#define NULL 0

/* duration in ms of volume ramp applied when starting capture to remove plop */
#define CAPTURE_START_RAMP_MS 100


/* maximum number of channel mask configurations supported. Currently the primary
 * output only supports 1 (stereo) and the multi channel HDMI output 2 (5.1 and 7.1) */
#define MAX_SUPPORTED_CHANNEL_MASKS 2


enum output_type {
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_HDMI,          // HDMI multi channel
    OUTPUT_TOTAL
};


struct tiny4412_audio_device;

struct tiny4412_stream_out {
    struct audio_stream_out stream;
    struct tiny4412_audio_device *dev;
    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    bool standby; /* true if all PCMs are inactive */
    bool muted;
    audio_devices_t device;
    audio_channel_mask_t channel_mask;
    unsigned int pcm_card_type;
    unsigned int pcm_device;
    unsigned int out_type;
    unsigned int written;
    struct pcm_config config;
    struct pcm *pcm[PCM_TOTAL];
};

struct tiny4412_stream_in {
    struct audio_stream_in stream;
    struct tiny4412_audio_device *dev;
    audio_devices_t device;
    struct pcm_config config;
    pthread_mutex_t lock; /* see note below on mutex acquisition order */
};


struct tiny4412_audio_device {
    struct audio_hw_device device;
    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    audio_devices_t out_device; /* "or" of stream_out.device for all active output streams */
    audio_devices_t in_device;
    struct tiny4412_stream_out *outputs[OUTPUT_TOTAL];
};





#endif
