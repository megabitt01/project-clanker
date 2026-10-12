#include "audio.h"
#include "../log.h"

#include <SDL.h>

namespace rt {

static SDL_AudioDeviceID g_dev;

bool host_audio_init()
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_WARN("audio", "SDL audio unavailable: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want = {}, have = {};
    want.freq = AUDIO_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        LOG_WARN("audio", "no audio device: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    LOG_INFO("audio", "audio output %d Hz, %d channels", have.freq, have.channels);
    return true;
}

void host_audio_queue(const float *frames, int count)
{
    if (g_dev)
        SDL_QueueAudio(g_dev, frames, count * 2 * sizeof(float));
}

int host_audio_queued_frames()
{
    return g_dev ? int(SDL_GetQueuedAudioSize(g_dev) / (2 * sizeof(float))) : 0;
}

} // namespace rt
