#pragma once
#include <cstdint>

namespace rt {

constexpr int AUDIO_RATE = 48000;

// Opens the SDL audio device (48 kHz stereo float). Returns false if no
// device is available; the mixer then still runs in real time, silently.
bool host_audio_init();
// Queues interleaved stereo float frames for playback.
void host_audio_queue(const float *frames, int count);
// Frames currently queued but not yet played.
int host_audio_queued_frames();

} // namespace rt
