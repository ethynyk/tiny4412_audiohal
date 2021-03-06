/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hal"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#include <cutils/log.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include "audio_hal.h"


#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct pcm_config pcm_out_config = {
            channels : 2,
            rate : AUDIO_HW_OUT_SAMPLERATE,
            period_size : AUDIO_HW_OUT_PERIOD_SZ,
            period_count : AUDIO_HW_OUT_PERIOD_CNT,
            format : PCM_FORMAT_S16_LE,
            start_threshold : 0,
            stop_threshold : 0,
            silence_threshold : 0,
};

#if 0
struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 44100,
    .period_size = 2048,
    .period_count = 2,
    .format = PCM_FORMAT_S16_LE,
};
#endif

struct pcm_config pcm_config_in = {
        channels : 2,
        rate : AUDIO_HW_IN_SAMPLERATE,
        period_size : AUDIO_HW_IN_PERIOD_SZ,
        period_count : AUDIO_HW_IN_PERIOD_CNT,
        format : PCM_FORMAT_S16_LE,
        start_threshold : 0,
        stop_threshold : 0,
        silence_threshold : 0,
    };



void pcm_dump(const void* buffer, size_t bytes)
{
    int fd;
    char value[PROPERTY_VALUE_MAX];

    property_get("debug.audio.dumpdata",value,"0");

    if(atoi(value) != 1)
    {
        return ;
    }

    fd = open("/data/data.pcm",O_WRONLY|O_CREAT|O_APPEND,0777);
    if(fd < 0)
    {
        ALOGE("open file /data/data.pcm failed,errno is %d",errno);
        return ;
    }

    write(fd,buffer,bytes);

    return;
}

static void do_tiny4412_out_standby(struct tiny4412_stream_out *out)
{
    if(!out->standby)
    {
        if(out->pcm[out->out_type])
        {
            pcm_close(out->pcm[out->out_type]);
            out->pcm[out->out_type] = NULL;
        }
        out->standby = true;
    }

    return;
}

static void do_tiny4412_in_standby(struct tiny4412_stream_in *in)
{
    struct tiny4412_audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        in->standby = true;
    }

}


/* must be called with hw device outputs list, output stream, and hw device mutexes locked */
static int start_tiny4412_output_stream(struct tiny4412_stream_out *out)
{
    struct tiny4412_audio_device *adev = out->dev;

    out->pcm[out->out_type] = pcm_open(out->pcm_card_type, out->pcm_device,
                                  PCM_OUT , &out->config);

    if (out->pcm[out->out_type] && !pcm_is_ready(out->pcm[out->out_type])) {
        ALOGE("pcm_open(PCM_CARD) failed: %s",
              pcm_get_error(out->pcm[out->out_type]));
        pcm_close(out->pcm[out->out_type]);
        return -ENOMEM;
    }



    adev->out_device |= out->device;

    return 0;
}

static int start_tiny4412_input_stream(struct tiny4412_stream_in *in)
{
    struct tiny4412_audio_device *adev = in->dev;

    ALOGI("ethyn channel:%d,rate:%d,format:%d",in->config->channels,in->config->rate,in->config->format);

    in->pcm = pcm_open(PCM_CARD, PCM_DEVICE, PCM_IN, in->config);

    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    if (in->resampler)
        in->resampler->reset(in->resampler);


    in->frames_in = 0;

    return 0;
}

static size_t get_tiny4412_input_buffer_size(unsigned int sample_rate,
                                    audio_format_t format,
                                    unsigned int channel_count,
                                    bool is_low_latency)
{
    const struct pcm_config *config = &pcm_config_in;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (config->period_size * sample_rate) / config->rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * audio_bytes_per_sample(format);
}



static int get_tiny4412_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct tiny4412_stream_in *in;
    size_t i;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct tiny4412_stream_in *)((char *)buffer_provider -
                                   offsetof(struct tiny4412_stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   pcm_frames_to_bytes(in->pcm, in->config->period_size));
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        in->frames_in = in->config->period_size;

        /* Do stereo to mono conversion in place by discarding right channel */
        if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer +
            (in->config->period_size - in->frames_in) *
                audio_channel_count_from_in_mask(in->channel_mask);

    pcm_dump(in->buffer,pcm_bytes_to_frames(in->pcm,buffer->frame_count));

    return in->read_status;

}

static void release_tiny4412_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct tiny4412_stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct tiny4412_stream_in *)((char *)buffer_provider -
                                   offsetof(struct tiny4412_stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_tiny4412_frames(struct tiny4412_stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    size_t frame_size = audio_stream_in_frame_size(&in->stream);

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * frame_size),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_tiny4412_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * frame_size,
                        buf.raw,
                        buf.frame_count * frame_size);
                frames_rd = buf.frame_count;
            }
            release_tiny4412_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }

    
    
    return frames_wr;
}




static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return AUDIO_HW_OUT_SAMPLERATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return 4096;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct tiny4412_stream_out *out = (struct tiny4412_stream_out *)stream;
    struct tiny4412_audio_device *adev = out->dev;

    pthread_mutex_lock(&out->lock);
    do_tiny4412_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    struct tiny4412_stream_out *out = (struct tiny4412_stream_out *)stream;
    struct tiny4412_audio_device *adev = out->dev;

    dprintf(fd,"out:%#x\n",out);
    dprintf(fd,"output_type:%d,standby:%d,muted:%d\n",out->out_type,out->standby,out->muted);
    dprintf(fd,"pcm_card_type:%d,pcm_device:%d,written:%d\n",out->pcm_card_type,out->pcm_device,out->written);
        
   
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    return 0;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{

    int ret = 0;
    struct tiny4412_stream_out *out = (struct tiny4412_stream_out *)stream;
    struct tiny4412_audio_device *adev = out->dev;

    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        
        ret = start_tiny4412_output_stream(out);
        if (ret < 0) {
            goto final_exit;
        }
        out->standby = false;
    }


    if (out->muted)
        memset((void *)buffer, 0, bytes);

    if (out->pcm[out->out_type]) {
        ret = pcm_write(out->pcm[out->out_type], (void *)buffer, bytes);
        
    if (ret == 0)
        out->written += bytes / (out->config.channels * sizeof(short));
     }

    //ALOGD("buffer:%#x,bytes:%u,ret=%d",buffer,bytes,ret);
    
final_exit:
    
    pthread_mutex_unlock(&out->lock);
    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
               out_get_sample_rate(&stream->common));
    }
    
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    return 48000;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct tiny4412_stream_in *in = (struct tiny4412_stream_in *)stream;
    return in->channel_mask;
}


static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct tiny4412_stream_in *in = (struct tiny4412_stream_in *)stream;

    return get_tiny4412_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 audio_channel_count_from_in_mask(in_get_channels(stream)),
                                 (in->flags & AUDIO_INPUT_FLAG_FAST) != 0);
}


static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct tiny4412_stream_in *in = (struct tiny4412_stream_in *)stream;
    struct tiny4412_audio_device *adev = in->dev;

    pthread_mutex_lock(&in->lock);
    do_tiny4412_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    struct tiny4412_stream_in *in = (struct tiny4412_stream_in *)stream;
    struct tiny4412_audio_device *adev = in->dev;

    dprintf(fd,"in:%#x\n",in);
    dprintf(fd,"standby:%d,muted:%d,channel_count:%d\n",in->standby,in->muted,in->channel_count);
    dprintf(fd,"channel_mask:%#x,requested_rate:%d,flags:%d,frames_in:%d\n",in->channel_mask,in->requested_rate,in->flags,in->frames_in);
    dprintf(fd,"resampler:%p\n",in->resampler);
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    unsigned int i,read_frame_count,frames_rd;
    struct tiny4412_stream_in *in = (struct tiny4412_stream_in *)stream;
    struct tiny4412_audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    ALOGD("in_read frames_rq:%u,bytes:%u",frames_rq,bytes);
    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        pthread_mutex_lock(&adev->lock);
        ret = start_tiny4412_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret < 0)
            goto exit;
        in->standby = false;
    }

    ALOGD("in_read frames_rq:%u,bytes:%u",frames_rq,bytes);

    ret = read_tiny4412_frames(in, buffer, frames_rq);

    if (ret > 0)
        ret = 0;

#if 0
    frames_rd = 0;
    while(frames_rd < frames_rq)
    {
         in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   pcm_frames_to_bytes(in->pcm, in->config->period_size));
        if (in->read_status != 0) {
            ALOGE("in_read() pcm_read error %d", in->read_status);
            return in->read_status;
        }

        in->frames_in += in->config->period_size;
        read_frame_count = in->config->period_size;
        frames_rd +=  in->config->period_size;
        /* Do stereo to mono conversion in place by discarding right channel */
        if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
        {
            for (i = 1; i < read_frame_count; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }
#endif
   
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_in_frame_size(stream) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
     struct tiny4412_audio_device *adev = (struct tiny4412_audio_device *)dev;
    struct tiny4412_stream_out *out;
    int ret;

    out = (struct tiny4412_stream_out *)calloc(1, sizeof(struct tiny4412_stream_out));
    if (!out)
        return -ENOMEM;

    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;
    out->device = devices;

    if (flags & AUDIO_OUTPUT_FLAG_DIRECT &&
                   devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        
    } else {
        out->config = pcm_out_config;
        out->pcm_device = PCM_DEVICE;
        out->pcm_card_type = PCM_CARD;
        out->out_type = OUTPUT_LOW_LATENCY;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    pthread_mutex_lock(&adev->lock);
    if (adev->outputs[out->out_type]) {
        pthread_mutex_unlock(&adev->lock);
        ret = -EBUSY;
        goto err_open;
    }
    adev->outputs[out->out_type] = out;
    pthread_mutex_unlock(&adev->lock);

    *stream_out = &out->stream;

    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct tiny4412_stream_out *out = (struct tiny4412_stream_out *)stream;
    struct tiny4412_audio_device *adev = out->dev;

    if(out->pcm[out->out_type])
    {
        pcm_close(out->pcm[out->out_type]);
        out->pcm[out->out_type] = NULL;
    }
        
    
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{

    ALOGD("<%s,%d> kvpairs:%s",__FUNCTION__,__LINE__,kvpairs);

    return -ENOSYS;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return NULL;
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    return 2048;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    struct tiny4412_audio_device *adev = (struct tiny4412_audio_device *)dev;
    struct tiny4412_stream_in *in;
    int ret;

    in = (struct stub_stream_in *)calloc(1, sizeof(struct tiny4412_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    
    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->input_source = AUDIO_SOURCE_DEFAULT;
    /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
    in->device = devices & ~AUDIO_DEVICE_BIT_IN;
    in->io_handle = handle;
    in->channel_mask = config->channel_mask;
    in->flags = flags;
    struct pcm_config *pcm_config = &pcm_config_in;
    in->config = pcm_config;

    in->buffer = malloc(pcm_config->period_size * pcm_config->channels
                                               * audio_stream_in_frame_size(&in->stream));

    in->channel_count = audio_channel_count_from_in_mask(in->channel_mask);

    if (!in->buffer) {
        ret = -ENOMEM;
        goto err_open;
    }
    in->resampler = NULL;
    
    if (in->requested_rate != pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_tiny4412_next_buffer;
        in->buf_provider.release_buffer = release_tiny4412_buffer;

        ret = create_resampler(pcm_config->rate,
                               in->requested_rate,
                               audio_channel_count_from_in_mask(in->channel_mask),
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err_resampler;
        }
    }

    *stream_in = &in->stream;
    adev->mic_input = in;
    return 0;
err_resampler:
    free(in->buffer);

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *in)
{
    struct tiny4412_stream_in *streamin = (struct tiny4412_stream_in *)in;
    struct tiny4412_audio_device *adev = streamin->dev;

    if(streamin->pcm)
    {
        pcm_close(streamin->pcm);
        free(streamin->buffer);
        streamin->pcm = NULL;
    }

    if (streamin->resampler) {
        release_resampler(streamin->resampler);
        streamin->resampler = NULL;
    }
    
    free(streamin);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    int i;
    struct tiny4412_audio_device *adev = (struct tiny4412_audio_device *)device;
    
    dprintf(fd,"audio hal dump info:\n");
    dprintf(fd,"out_device:%#x,in_device:%#x\n",adev->out_device,adev->in_device);
    for(i = 0; i < OUTPUT_TOTAL ; i++)
    {
        if(adev->outputs[i])
        {
            out_dump(adev->outputs[i],fd);
        }
    }

    in_dump(adev->mic_input,fd);
    
    return 0;
}

static int adev_close(hw_device_t *device)
{
    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct tiny4412_audio_device *adev;
    int ret;

 //   if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
 //       return -EINVAL;

    adev = calloc(1, sizeof(struct tiny4412_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->device.common.module = (struct hw_module_t *) module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_master_mute = adev_set_master_mute;
    adev->device.get_master_mute = adev_get_master_mute;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    adev->mic_mute = false;

    *device = &adev->device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Default audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};

