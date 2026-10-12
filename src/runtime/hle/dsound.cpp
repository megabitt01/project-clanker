// DirectSound replacement.
//
// Xbox DirectSound drives the MCPX audio processor (256 hardware voices plus
// DSP effects). This module replaces its public API with a software mixer:
// sound buffers and packet streams are mixed to 48 kHz stereo on a dedicated
// thread and played through SDL. Supported: 8/16-bit PCM and Xbox ADPCM,
// play/loop regions, frequency, volume, mix-bin routing (downmixed to
// stereo), submix (MIXIN) buffers, stream packet completion via events or
// callbacks, voice headroom, and DS3D positional voices (panned from their
// position, with distance rolloff). Hardware effects (I3DL2 reverb, HRTF
// filtering) are not reproduced.
#include "hook.h"
#include "../host/audio.h"
#include "../kernel/kfuncs.h"
#include "../kernel/object.h"
#include "../kernel/sync.h"
#include "../kernel/thread.h"
#include "../log.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

using namespace rt;

namespace {

#pragma pack(push, 2)
struct XWAVEFORMATEX {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
};
#pragma pack(pop)
static_assert(sizeof(XWAVEFORMATEX) == 18);

struct DSMIXBINVOLUMEPAIR {
    uint32_t dwMixBin;
    int32_t lVolume;
};
struct DSMIXBINS {
    uint32_t dwMixBinCount;
    const DSMIXBINVOLUMEPAIR *lpMixBinVolumePairs;
};
struct DSBUFFERDESC {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwBufferBytes;
    const XWAVEFORMATEX *lpwfxFormat;
    const DSMIXBINS *lpMixBins;
    uint32_t dwInputMixBin;
};
struct DSSTREAMDESC {
    uint32_t dwFlags;
    uint32_t dwMaxAttachedPackets;
    const XWAVEFORMATEX *lpwfxFormat;
    void *lpfnCallback;
    void *lpvContext;
    const DSMIXBINS *lpMixBins;
};
struct XMEDIAPACKET {
    void *pvBuffer;
    uint32_t dwMaxSize;
    uint32_t *pdwCompletedSize;
    uint32_t *pdwStatus;
    union {
        XHANDLE hCompletionEvent;
        void *pContext;
    };
    int64_t *prtTimestamp;
};
struct XMEDIAINFO {
    uint32_t dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead;
};
struct D3DVECTOR_ {
    float x, y, z;
};
struct DS3DBUFFER {
    uint32_t dwSize;
    D3DVECTOR_ vPosition, vVelocity;
    uint32_t dwInsideConeAngle, dwOutsideConeAngle;
    D3DVECTOR_ vConeOrientation;
    int32_t lConeOutsideVolume;
    float flMinDistance, flMaxDistance;
    uint32_t dwMode;
    float flDistanceFactor, flRolloffFactor, flDopplerFactor;
};
struct DS3DLISTENER {
    uint32_t dwSize;
    D3DVECTOR_ vPosition, vVelocity, vOrientFront, vOrientTop;
    float flDistanceFactor, flRolloffFactor, flDopplerFactor;
};
static_assert(sizeof(DS3DBUFFER) == 76 && sizeof(DS3DLISTENER) == 64);

constexpr int32_t DS_OK = 0;
constexpr int32_t E_FAIL_ = int32_t(0x80004005);
constexpr int32_t E_OUTOFMEMORY_ = int32_t(0x8007000E);
constexpr uint32_t XMEDIAPACKET_STATUS_SUCCESS = 0;
constexpr uint32_t XMEDIAPACKET_STATUS_PENDING = 0x8000000A;
constexpr uint32_t XMEDIAPACKET_STATUS_FLUSHED = 0x80004004;
constexpr uint32_t DSBCAPS_MIXIN = 0x00002000;
constexpr uint32_t DSBPLAY_LOOPING = 1, DSBPLAY_FROMSTART = 2, DSBPLAY_SYNCHPLAYBACK = 4;
constexpr uint32_t DSBSTATUS_PLAYING = 1, DSBSTATUS_PAUSED = 2, DSBSTATUS_LOOPING = 4;
constexpr uint16_t WAVE_FORMAT_PCM_ = 1, WAVE_FORMAT_XBOX_ADPCM = 0x69;
constexpr uint32_t MIXBIN_FRONT_LEFT = 0, MIXBIN_FRONT_RIGHT = 1;
constexpr uint32_t DS3DMODE_NORMAL = 0, DS3DMODE_HEADRELATIVE = 1, DS3DMODE_DISABLE = 2;
constexpr int ADPCM_BLOCK_SAMPLES = 64;

struct Voice;

struct StreamPacket {
    XMEDIAPACKET pkt;
    uint64_t end_frame;   // FIFO frame index at which the packet has played
};

struct Voice {
    const void *vtable = nullptr; // streams: XMediaObject vtable (must be first)
    uint32_t magic = 'VOIC';
    bool is_stream = false;
    int refs = 1;

    // format
    uint16_t tag = WAVE_FORMAT_PCM_, channels = 1, bits = 16, block_align = 2;
    uint32_t rate = 48000, freq = 0;
    int32_t volume = 0;
    uint32_t headroom = 0;        // attenuation in mB applied before the mix bins
    DSMIXBINVOLUMEPAIR bins[8] = {};
    uint32_t nbins = 0;
    Voice *output = nullptr;      // submix destination
    bool has3d = false;           // positional voice (SetAllParameters received)
    DS3DBUFFER p3d = {};
    bool mixin = false;
    std::vector<float> mixin_acc; // stereo frames accumulated this chunk

    // buffer state
    const uint8_t *data = nullptr;
    uint32_t bytes = 0;
    uint32_t play_start = 0, play_len = 0, loop_start = 0, loop_len = 0;
    double pos = 0;               // in frames
    bool playing = false, looping = false, paused = false, synch_pending = false;
    int cached_block = -1;
    int16_t cache[2][ADPCM_BLOCK_SAMPLES + 1];

    // stream state
    std::deque<float> fifo;       // decoded interleaved frames (always 2 channels)
    uint64_t fifo_base = 0;       // absolute index of fifo.front() frame
    double fifo_pos = 0;          // absolute frame position being played
    std::deque<StreamPacket> packets;
    uint32_t max_packets = 8;
    void *callback = nullptr;
    void *context = nullptr;
    bool stream_paused = false;
};

std::recursive_mutex g_lock;
DS3DLISTENER g_listener = {64, {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1, 1, 1};
std::vector<Voice *> g_voices;
uint32_t g_dsound_obj[16] = {'DSND'}; // the IDirectSound singleton (contents unused)
int g_dsound_refs;

float db_gain(int32_t hundredths)
{
    if (hundredths <= -10000)
        return 0.0f;
    if (hundredths >= 0)
        return 1.0f;
    return powf(10.0f, float(hundredths) / 2000.0f);
}

uint32_t frames_from_bytes(const Voice *v, uint32_t b)
{
    if (v->tag == WAVE_FORMAT_XBOX_ADPCM)
        return (b / (36u * v->channels)) * ADPCM_BLOCK_SAMPLES;
    return v->block_align ? b / v->block_align : 0;
}

uint32_t bytes_from_frames(const Voice *v, uint32_t f)
{
    if (v->tag == WAVE_FORMAT_XBOX_ADPCM)
        return (f / ADPCM_BLOCK_SAMPLES) * 36u * v->channels;
    return f * v->block_align;
}

void set_format(Voice *v, const XWAVEFORMATEX *w)
{
    if (!w)
        return;
    v->tag = w->wFormatTag;
    v->channels = w->nChannels ? w->nChannels : 1;
    v->rate = w->nSamplesPerSec ? w->nSamplesPerSec : 48000;
    v->bits = w->wBitsPerSample;
    v->block_align = w->nBlockAlign ? w->nBlockAlign : uint16_t(v->channels * v->bits / 8);
    v->cached_block = -1;
    if (v->tag != WAVE_FORMAT_PCM_ && v->tag != WAVE_FORMAT_XBOX_ADPCM)
        LOG_WARN("dsound", "unsupported wave format tag 0x%X", v->tag);
}

void set_mixbins(Voice *v, const DSMIXBINS *mb)
{
    v->nbins = 0;
    if (mb && mb->lpMixBinVolumePairs) {
        for (uint32_t i = 0; i < mb->dwMixBinCount && v->nbins < 8; i++)
            v->bins[v->nbins++] = mb->lpMixBinVolumePairs[i];
    }
    if (!v->nbins) {
        v->bins[0] = {MIXBIN_FRONT_LEFT, 0};
        v->bins[1] = {MIXBIN_FRONT_RIGHT, 0};
        v->nbins = 2;
    }
}

void set_mixbin_volumes(Voice *v, const DSMIXBINS *mb)
{
    if (!mb || !mb->lpMixBinVolumePairs)
        return;
    for (uint32_t i = 0; i < mb->dwMixBinCount; i++)
        for (uint32_t k = 0; k < v->nbins; k++)
            if (v->bins[k].dwMixBin == mb->lpMixBinVolumePairs[i].dwMixBin)
                v->bins[k].lVolume = mb->lpMixBinVolumePairs[i].lVolume;
}

// ---- Xbox ADPCM -------------------------------------------------------------------
const int k_ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
const int k_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Decodes one 36*channels-byte block into out[ch][0..64].
void adpcm_decode_block(const uint8_t *in, int channels, int16_t out[2][ADPCM_BLOCK_SAMPLES + 1])
{
    int pred[2], idx[2];
    for (int c = 0; c < channels && c < 2; c++) {
        pred[c] = int16_t(in[c * 4] | (in[c * 4 + 1] << 8));
        idx[c] = in[c * 4 + 2];
        if (idx[c] > 88) idx[c] = 88;
        out[c][0] = int16_t(pred[c]);
    }
    const uint8_t *p = in + 4 * channels;
    for (int chunk = 0; chunk < 8; chunk++) {
        for (int c = 0; c < channels; c++) {
            for (int b = 0; b < 4; b++) {
                uint8_t byte = *p++;
                if (c >= 2)
                    continue;
                for (int n = 0; n < 2; n++) {
                    int nib = n ? (byte >> 4) : (byte & 0xF);
                    int step = k_ima_steps[idx[c]];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    if (nib & 8) diff = -diff;
                    pred[c] += diff;
                    if (pred[c] > 32767) pred[c] = 32767;
                    if (pred[c] < -32768) pred[c] = -32768;
                    idx[c] += k_ima_index[nib];
                    if (idx[c] < 0) idx[c] = 0;
                    if (idx[c] > 88) idx[c] = 88;
                    out[c][1 + chunk * 8 + b * 2 + n] = int16_t(pred[c]);
                }
            }
        }
    }
}

// Reads frame `f` of a buffer voice, channel `c`.
float buffer_sample(Voice *v, uint32_t f, int c)
{
    if (c >= v->channels)
        c = v->channels - 1;
    if (v->tag == WAVE_FORMAT_XBOX_ADPCM) {
        uint32_t block = f / ADPCM_BLOCK_SAMPLES;
        uint32_t off = block * 36u * v->channels;
        if (off + 36u * v->channels > v->bytes)
            return 0.0f;
        if (int(block) != v->cached_block) {
            adpcm_decode_block(v->data + off, v->channels, v->cache);
            v->cached_block = int(block);
        }
        return v->cache[c > 1 ? 1 : c][f % ADPCM_BLOCK_SAMPLES] / 32768.0f;
    }
    uint32_t off = f * v->block_align + c * (v->bits / 8);
    if (off + v->bits / 8 > v->bytes)
        return 0.0f;
    if (v->bits == 8)
        return (int(v->data[off]) - 128) / 128.0f;
    return int16_t(v->data[off] | (v->data[off + 1] << 8)) / 32768.0f;
}

// Decodes a stream packet into the voice FIFO (stereo frames).
void stream_decode(Voice *v, const uint8_t *src, uint32_t size)
{
    auto push = [&](float l, float r) {
        v->fifo.push_back(l);
        v->fifo.push_back(r);
    };
    if (v->tag == WAVE_FORMAT_XBOX_ADPCM) {
        uint32_t bsz = 36u * v->channels;
        int16_t out[2][ADPCM_BLOCK_SAMPLES + 1];
        for (uint32_t off = 0; off + bsz <= size; off += bsz) {
            adpcm_decode_block(src + off, v->channels, out);
            for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++)
                push(out[0][i] / 32768.0f, out[v->channels > 1 ? 1 : 0][i] / 32768.0f);
        }
        return;
    }
    uint32_t ba = v->block_align ? v->block_align : 2;
    for (uint32_t off = 0; off + ba <= size; off += ba) {
        float s[2];
        for (int c = 0; c < 2; c++) {
            int cc = c < v->channels ? c : 0;
            uint32_t o = off + cc * (v->bits / 8);
            s[c] = v->bits == 8 ? (int(src[o]) - 128) / 128.0f : int16_t(src[o] | (src[o + 1] << 8)) / 32768.0f;
        }
        push(s[0], s[1]);
    }
}

// ---- mixing --------------------------------------------------------------------------
// Per source channel, gain to the left / right outputs.
// Stereo gains of a positional voice. On the Xbox, DirectSound turns the 3D
// parameters into speaker mix-bin volumes; here the source is panned between
// the two outputs, with distance rolloff when enabled.
void gains_3d(const Voice *v, float gain, float &gl, float &gr)
{
    const DS3DBUFFER &p = v->p3d;
    float x = p.vPosition.x, y = p.vPosition.y, z = p.vPosition.z;
    if (p.dwMode == DS3DMODE_NORMAL) {
        // Into the listener's frame (left-handed: right = top x front).
        const DS3DLISTENER &l = g_listener;
        float dx = x - l.vPosition.x, dy = y - l.vPosition.y, dz = z - l.vPosition.z;
        const D3DVECTOR_ &f = l.vOrientFront, &t = l.vOrientTop;
        D3DVECTOR_ r = {t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
        x = dx * r.x + dy * r.y + dz * r.z;
        y = dx * t.x + dy * t.y + dz * t.z;
        z = dx * f.x + dy * f.y + dz * f.z;
    }
    float dist = sqrtf(x * x + y * y + z * z);
    float rolloff = p.flRolloffFactor * g_listener.flRolloffFactor;
    float mind = p.flMinDistance > 0 ? p.flMinDistance : 1.0f;
    if (rolloff > 0 && dist > mind) {
        float d = p.flMaxDistance > mind && dist > p.flMaxDistance ? p.flMaxDistance : dist;
        gain *= mind / (mind + rolloff * (d - mind));
    }
    float pan = dist > 1e-4f ? x / dist : 0.0f;               // sine of the azimuth
    pan = pan < -1 ? -1 : pan > 1 ? 1 : pan;
    if (z < 0)
        gain *= 1.0f + 0.15f * z / (dist > 1e-4f ? dist : 1); // rear sources a little softer
    // Full level on both sides for a centred source, fading out the far side.
    gl = gain * std::min(1.0f, sqrtf(1 - pan));
    gr = gain * std::min(1.0f, sqrtf(1 + pan));
}

void voice_gains(const Voice *v, int src_channels, float g[2][2])
{
    g[0][0] = g[0][1] = g[1][0] = g[1][1] = 0.0f;
    float base = db_gain(v->volume - int32_t(v->headroom));
    bool positional = v->has3d && v->p3d.dwMode != DS3DMODE_DISABLE;
    float speakers = 0.0f; // loudest speaker bin, for positional voices
    for (uint32_t k = 0; k < v->nbins; k++) {
        float gain = base * db_gain(v->bins[k].lVolume);
        int src = src_channels == 1 ? 0 : int(k < uint32_t(src_channels) ? k : src_channels - 1);
        uint32_t bin = v->bins[k].dwMixBin;
        if (bin >= 6 && bin <= 9)
            bin = bin < 8 ? bin - 6 : bin - 4; // crosstalk-cancelled ("XTLK") speakers
        if (positional) {
            if (bin <= 5 && bin != 3)
                speakers = std::max(speakers, gain);
            continue;
        }
        switch (bin) {
        case 0: g[src][0] += gain; break;                       // front left
        case 1: g[src][1] += gain; break;                       // front right
        case 2: g[src][0] += gain * 0.707f; g[src][1] += gain * 0.707f; break; // center
        case 4: g[src][0] += gain * 0.707f; break;              // back left
        case 5: g[src][1] += gain * 0.707f; break;              // back right
        default: break;                                         // LFE, I3DL2 and effect sends
        }
    }
    if (positional) {
        float gl, gr;
        gains_3d(v, speakers, gl, gr);
        g[0][0] = g[1][0] = gl;
        g[0][1] = g[1][1] = gr;
        return;
    }
    if (src_channels == 1) {
        g[1][0] = g[0][0];
        g[1][1] = g[0][1];
    }
}

void mix_into(float *dst, int frames, float l, float r, int i, const float g[2][2])
{
    dst[i * 2] += l * g[0][0] + r * g[1][0];
    dst[i * 2 + 1] += l * g[0][1] + r * g[1][1];
}

struct Completion {
    Voice *voice;
    XMEDIAPACKET pkt;
    uint32_t status;
};

void mix_buffer(Voice *v, float *dst, int frames, const float g[2][2])
{
    uint32_t total = frames_from_bytes(v, v->bytes);
    uint32_t pstart = frames_from_bytes(v, v->play_start);
    uint32_t pend = v->play_len ? pstart + frames_from_bytes(v, v->play_len) : total;
    uint32_t lstart = frames_from_bytes(v, v->loop_start);
    uint32_t lend = v->loop_len ? lstart + frames_from_bytes(v, v->loop_len) : pend;
    if (pend > total) pend = total;
    if (lend > total) lend = total;
    double step = double(v->freq ? v->freq : v->rate) / AUDIO_RATE;
    for (int i = 0; i < frames; i++) {
        uint32_t f = uint32_t(v->pos);
        uint32_t end = v->looping ? lend : pend;
        if (f >= end) {
            if (v->looping && lend > lstart) {
                v->pos -= double(lend - lstart);
                if (v->pos < lstart) v->pos = lstart;
                f = uint32_t(v->pos);
            } else {
                v->playing = false;
                return;
            }
        }
        double frac = v->pos - f;
        uint32_t f2 = f + 1 < end ? f + 1 : (v->looping ? lstart : f);
        float l = float(buffer_sample(v, f, 0) * (1 - frac) + buffer_sample(v, f2, 0) * frac);
        float r = v->channels > 1 ? float(buffer_sample(v, f, 1) * (1 - frac) + buffer_sample(v, f2, 1) * frac) : l;
        mix_into(dst, frames, l, r, i, g);
        v->pos += step;
    }
}

void mix_stream(Voice *v, float *dst, int frames, const float g[2][2], std::vector<Completion> &done)
{
    double step = double(v->freq ? v->freq : v->rate) / AUDIO_RATE;
    for (int i = 0; i < frames; i++) {
        uint64_t f = uint64_t(v->fifo_pos);
        uint64_t avail = v->fifo_base + v->fifo.size() / 2;
        if (f + 1 >= avail)
            break; // starved: wait for more packets
        size_t idx = size_t(f - v->fifo_base) * 2;
        double frac = v->fifo_pos - double(f);
        float l = float(v->fifo[idx] * (1 - frac) + v->fifo[idx + 2] * frac);
        float r = float(v->fifo[idx + 1] * (1 - frac) + v->fifo[idx + 3] * frac);
        mix_into(dst, frames, l, r, i, g);
        v->fifo_pos += step;
    }
    // Retire consumed frames and finished packets.
    uint64_t consumed = uint64_t(v->fifo_pos);
    while (v->fifo_base < consumed && !v->fifo.empty()) {
        v->fifo.pop_front();
        v->fifo.pop_front();
        v->fifo_base++;
    }
    while (!v->packets.empty() && v->packets.front().end_frame <= consumed) {
        done.push_back({v, v->packets.front().pkt, XMEDIAPACKET_STATUS_SUCCESS});
        v->packets.pop_front();
    }
    // A packet too short to decode anything completes immediately.
    if (!v->packets.empty() && v->fifo.empty() && v->packets.front().end_frame <= v->fifo_base + 1) {
        done.push_back({v, v->packets.front().pkt, XMEDIAPACKET_STATUS_SUCCESS});
        v->packets.pop_front();
    }
}

typedef void(__stdcall *XMediaCallback)(void *stream_ctx, void *packet_ctx, uint32_t status);

void complete_packet(const Completion &c)
{
    if (c.pkt.pdwCompletedSize)
        *c.pkt.pdwCompletedSize = c.status == XMEDIAPACKET_STATUS_SUCCESS ? c.pkt.dwMaxSize : 0;
    if (c.pkt.pdwStatus)
        *c.pkt.pdwStatus = c.status;
    if (c.voice->callback)
        reinterpret_cast<XMediaCallback>(c.voice->callback)(c.voice->context, c.pkt.pContext, c.status);
    else if (c.pkt.hCompletionEvent)
        kx_NtSetEvent(c.pkt.hCompletionEvent, nullptr);
}

void mix_chunk(float *out, int frames, std::vector<Completion> &done)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    memset(out, 0, frames * 2 * sizeof(float));
    for (Voice *v : g_voices)
        if (v->mixin)
            v->mixin_acc.assign(frames * 2, 0.0f);
    for (Voice *v : g_voices) {
        if (v->mixin)
            continue;
        float *dst = (v->output && v->output->mixin) ? v->output->mixin_acc.data() : out;
        float g[2][2];
        voice_gains(v, v->is_stream ? 2 : v->channels, g);
        if (v->is_stream) {
            if (!v->stream_paused)
                mix_stream(v, dst, frames, g, done);
        } else if (v->playing && !v->paused && v->data) {
            mix_buffer(v, dst, frames, g);
        }
    }
    for (Voice *v : g_voices) {
        if (!v->mixin || v->mixin_acc.empty())
            continue;
        float g[2][2];
        voice_gains(v, 2, g);
        for (int i = 0; i < frames; i++)
            mix_into(out, frames, v->mixin_acc[i * 2], v->mixin_acc[i * 2 + 1], i, g);
    }
    for (int i = 0; i < frames * 2; i++)
        out[i] = out[i] > 1.0f ? 1.0f : (out[i] < -1.0f ? -1.0f : out[i]);
}

void mixer_thread(void *)
{
    bool have_device = host_audio_init();
    constexpr int CHUNK = 480; // 10 ms
    std::vector<float> buf(CHUNK * 2);
    std::vector<Completion> done;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    uint64_t chunks = 0;
    for (;;) {
        if (have_device) {
            if (host_audio_queued_frames() > CHUNK * 4) {
                Sleep(2);
                continue;
            }
        } else {
            QueryPerformanceCounter(&now);
            double elapsed = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
            if (elapsed < double(chunks) * CHUNK / AUDIO_RATE) {
                Sleep(2);
                continue;
            }
        }
        done.clear();
        mix_chunk(buf.data(), CHUNK, done);
        chunks++;
        static float peak;
        for (float f : buf) peak = std::max(peak, std::fabs(f));
        if (chunks % 500 == 0) {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            size_t playing = 0, streams = 0, pending = 0;
            for (Voice *v : g_voices) {
                if (v->is_stream) { streams++; pending += v->packets.size(); }
                else if (v->playing) playing++;
            }
            LOG_INFO("dsound", "mixer: %llu chunks, %d frames queued, %zu voices (%zu buffers playing, %zu streams, %zu packets pending), peak %.3f",
                     chunks, host_audio_queued_frames(), g_voices.size(), playing, streams, pending, peak);
            peak = 0;
        }
        host_audio_queue(buf.data(), CHUNK);
        for (const Completion &c : done)
            complete_packet(c);
    }
}

Voice *new_voice()
{
    Voice *v = new Voice();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_voices.push_back(v);
    return v;
}

void delete_voice(Voice *v)
{
    std::vector<Completion> flushed;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        for (auto &p : v->packets)
            flushed.push_back({v, p.pkt, XMEDIAPACKET_STATUS_FLUSHED});
        v->packets.clear();
        for (size_t i = 0; i < g_voices.size(); i++)
            if (g_voices[i] == v) {
                g_voices.erase(g_voices.begin() + i);
                break;
            }
        for (Voice *o : g_voices)
            if (o->output == v)
                o->output = nullptr;
    }
    for (auto &c : flushed)
        complete_packet(c);
    v->magic = 0;
    delete v;
}

Voice *as_voice(void *p)
{
    Voice *v = static_cast<Voice *>(p);
    if (!v || v->magic != 'VOIC') {
        LOG_ERROR("dsound", "invalid DirectSound object %p", p);
        return nullptr;
    }
    return v;
}

void stream_flush(Voice *v)
{
    std::vector<Completion> flushed;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        for (auto &p : v->packets)
            flushed.push_back({v, p.pkt, XMEDIAPACKET_STATUS_FLUSHED});
        v->packets.clear();
        v->fifo.clear();
        v->fifo_base = 0;
        v->fifo_pos = 0;
    }
    for (auto &c : flushed)
        complete_packet(c);
}

} // namespace

#define LOCK std::lock_guard<std::recursive_mutex> lock(g_lock)
#define VOICE(p)                                                                                                       \
    Voice *v = as_voice(p);                                                                                            \
    if (!v)                                                                                                            \
        return E_FAIL_;

extern "C" {

// ---- XMediaObject vtable for streams (stdcall, `this` first) ----------------------
static uint32_t __stdcall st_AddRef(void *self)
{
    LOCK;
    Voice *v = as_voice(self);
    return v ? ++v->refs : 0;
}
static uint32_t __stdcall st_Release(void *self)
{
    Voice *v = as_voice(self);
    if (!v)
        return 0;
    int r;
    {
        LOCK;
        r = --v->refs;
    }
    if (r == 0)
        delete_voice(v);
    return r;
}
static int32_t __stdcall st_GetInfo(void *self, XMEDIAINFO *info)
{
    VOICE(self);
    info->dwFlags = 0x7; // fixed packet/sample size, discards nothing
    info->dwInputSize = v->tag == WAVE_FORMAT_XBOX_ADPCM ? 36u * v->channels : v->block_align;
    info->dwOutputSize = 0;
    info->dwMaxLookahead = 0;
    return DS_OK;
}
static int32_t __stdcall st_GetStatus(void *self, uint32_t *status)
{
    VOICE(self);
    LOCK;
    *status = v->packets.size() < v->max_packets ? 1u : 0u; // XMO_STATUSF_ACCEPT_INPUT_DATA
    return DS_OK;
}
static int32_t __stdcall st_Process(void *self, const XMEDIAPACKET *in, const XMEDIAPACKET *out)
{
    VOICE(self);
    if (!in)
        return E_FAIL_;
    LOCK;
    if (in->pdwStatus)
        *in->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
    if (in->pdwCompletedSize)
        *in->pdwCompletedSize = 0;
    stream_decode(v, static_cast<const uint8_t *>(in->pvBuffer), in->dwMaxSize);
    LOG_TRACE("dsound", "stream %p Process(%u bytes) -> %zu queued, paused %d", self, in->dwMaxSize, v->packets.size() + 1, v->stream_paused);
    v->packets.push_back({*in, v->fifo_base + v->fifo.size() / 2});
    return DS_OK;
}
static int32_t __stdcall st_Discontinuity(void *self) { return DS_OK; }
static int32_t __stdcall st_Flush(void *self)
{
    VOICE(self);
    stream_flush(v);
    return DS_OK;
}

static const void *g_stream_vtbl[] = {
    reinterpret_cast<void *>(&st_AddRef),   reinterpret_cast<void *>(&st_Release),
    reinterpret_cast<void *>(&st_GetInfo),  reinterpret_cast<void *>(&st_GetStatus),
    reinterpret_cast<void *>(&st_Process),  reinterpret_cast<void *>(&st_Discontinuity),
    reinterpret_cast<void *>(&st_Flush),
};

// ---- DirectSound object ---------------------------------------------------------------
static int32_t __stdcall hle_DirectSoundCreate(void *pguid, void **ppDS, void *unk)
{
    {
        LOCK;
        g_dsound_refs++;
    }
    *ppDS = g_dsound_obj;
    return DS_OK;
}

static uint32_t __stdcall hle_IDirectSound_Release(void *ds)
{
    LOCK;
    return g_dsound_refs > 0 ? --g_dsound_refs : 0;
}

static int32_t create_stream(const DSSTREAMDESC *d, void **out)
{
    Voice *v = new_voice();
    LOCK;
    v->vtable = g_stream_vtbl;
    v->is_stream = true;
    set_format(v, d->lpwfxFormat);
    set_mixbins(v, d->lpMixBins);
    v->max_packets = d->dwMaxAttachedPackets ? d->dwMaxAttachedPackets : 8;
    v->callback = d->lpfnCallback;
    v->context = d->lpvContext;
    *out = v;
    LOG_DEBUG("dsound", "stream %p: fmt %X %uch %uHz, %u packets", v, v->tag, v->channels, v->rate, v->max_packets);
    return DS_OK;
}

static int32_t __stdcall hle_DirectSoundCreateStream(const DSSTREAMDESC *d, void **ppStream)
{
    return create_stream(d, ppStream);
}

static int32_t __stdcall hle_IDirectSound_CreateSoundStream(void *ds, const DSSTREAMDESC *d, void **ppStream, void *unk)
{
    return create_stream(d, ppStream);
}

static int32_t __stdcall hle_IDirectSound_CreateSoundBuffer(void *ds, const DSBUFFERDESC *d, void **ppBuffer, void *unk)
{
    Voice *v = new_voice();
    LOCK;
    set_format(v, d->lpwfxFormat);
    set_mixbins(v, d->lpMixBins);
    v->mixin = (d->dwFlags & DSBCAPS_MIXIN) != 0;
    if (d->dwBufferBytes) {
        // Buffers created with a size own their memory.
        uint8_t *mem = static_cast<uint8_t *>(calloc(1, d->dwBufferBytes));
        v->data = mem;
        v->bytes = d->dwBufferBytes;
    }
    *ppBuffer = v;
    LOG_DEBUG("dsound", "buffer %p: flags %X bytes %u fmt %X", v, d->dwFlags, d->dwBufferBytes, v->tag);
    return DS_OK;
}

static int32_t __stdcall hle_IDirectSound_SetMixBinHeadroom(void *ds, uint32_t mask, uint32_t headroom) { return DS_OK; }
static int32_t __stdcall hle_IDirectSound_SetAllParameters(void *ds, const void *listener, uint32_t apply)
{
    if (!listener)
        return DS_OK;
    LOCK;
    g_listener = *static_cast<const DS3DLISTENER *>(listener);
    return DS_OK;
}
static int32_t __stdcall hle_IDirectSound_SetI3DL2Listener(void *ds, const void *listener, uint32_t apply) { return DS_OK; }
static int32_t __stdcall hle_IDirectSound_CommitDeferredSettings(void *ds) { return DS_OK; }
static int32_t __stdcall hle_IDirectSound_EnableHeadphones(void *ds, int enable) { return DS_OK; }

static int32_t __stdcall hle_IDirectSound_DownloadEffectsImage(void *ds, const void *image, uint32_t size,
                                                               const void *loc, void **ppDesc)
{
    // Effects run on the APU DSPs, which are not emulated. Hand back an empty
    // effect map (dwEffectCount = 0, dwTotalScratchSize = 0).
    static uint32_t desc[64];
    if (ppDesc)
        *ppDesc = desc;
    LOG_INFO("dsound", "DownloadEffectsImage(%u bytes): DSP effects not emulated", size);
    return DS_OK;
}

static int32_t __stdcall hle_IDirectSound_SynchPlayback(void *ds)
{
    LOCK;
    for (Voice *v : g_voices) {
        if (v->synch_pending) {
            v->synch_pending = false;
            v->playing = true;
        }
    }
    return DS_OK;
}

static void __stdcall hle_DirectSoundDoWork() {}
static void __stdcall hle_DirectSoundUseLightHRTF() {}

// ---- buffers -----------------------------------------------------------------------------
static uint32_t __stdcall hle_Buffer_Release(void *b)
{
    Voice *v = as_voice(b);
    if (!v)
        return 0;
    int r;
    {
        LOCK;
        r = --v->refs;
    }
    if (r == 0)
        delete_voice(v);
    return r;
}

static int32_t __stdcall hle_Buffer_SetBufferData(void *b, const void *data, uint32_t bytes)
{
    VOICE(b);
    LOCK;
    v->data = static_cast<const uint8_t *>(data);
    v->bytes = bytes;
    v->play_start = v->play_len = v->loop_start = v->loop_len = 0;
    v->pos = 0;
    v->cached_block = -1;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetPlayRegion(void *b, uint32_t start, uint32_t length)
{
    VOICE(b);
    LOCK;
    v->play_start = start;
    v->play_len = length;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetLoopRegion(void *b, uint32_t start, uint32_t length)
{
    VOICE(b);
    LOCK;
    v->loop_start = start;
    v->loop_len = length;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_Play(void *b, uint32_t r1, uint32_t r2, uint32_t flags)
{
    VOICE(b);
    LOCK;
    v->looping = (flags & DSBPLAY_LOOPING) != 0;
    if ((flags & DSBPLAY_FROMSTART) || !v->playing)
        if ((flags & DSBPLAY_FROMSTART) || frames_from_bytes(v, v->play_start) > v->pos)
            v->pos = frames_from_bytes(v, v->play_start);
    v->paused = false;
    if (flags & DSBPLAY_SYNCHPLAYBACK)
        v->synch_pending = true;
    else
        v->playing = true;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_Stop(void *b)
{
    VOICE(b);
    LOCK;
    v->playing = false;
    v->synch_pending = false;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_StopEx(void *b, int64_t rt, uint32_t flags)
{
    VOICE(b);
    LOCK;
    if (flags & 2) // DSBSTOPEX_RELEASEWAVEFORM: let the sound play out
        v->looping = false;
    else
        v->playing = false;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_Pause(void *b, uint32_t pause)
{
    VOICE(b);
    LOCK;
    if (pause == 2)
        v->synch_pending = true;
    else
        v->paused = pause == 1;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_GetStatus(void *b, uint32_t *status)
{
    VOICE(b);
    LOCK;
    uint32_t s = 0;
    if (v->playing || v->synch_pending) s |= DSBSTATUS_PLAYING;
    if (v->paused) s |= DSBSTATUS_PAUSED;
    if (v->looping && v->playing) s |= DSBSTATUS_LOOPING;
    *status = s;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_GetCurrentPosition(void *b, uint32_t *play, uint32_t *write)
{
    VOICE(b);
    LOCK;
    uint32_t p = bytes_from_frames(v, uint32_t(v->pos));
    if (play) *play = p;
    if (write) *write = p;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetCurrentPosition(void *b, uint32_t pos)
{
    VOICE(b);
    LOCK;
    v->pos = frames_from_bytes(v, pos);
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetFormat(void *b, const XWAVEFORMATEX *w)
{
    VOICE(b);
    LOCK;
    set_format(v, w);
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetFrequency(void *b, uint32_t f)
{
    VOICE(b);
    LOCK;
    v->freq = f;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetVolume(void *b, int32_t vol)
{
    VOICE(b);
    LOCK;
    v->volume = vol;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetHeadroom(void *b, uint32_t hr)
{
    VOICE(b);
    LOCK;
    v->headroom = hr;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetMixBins(void *b, const DSMIXBINS *mb)
{
    VOICE(b);
    LOCK;
    set_mixbins(v, mb);
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetMixBinVolumes(void *b, const DSMIXBINS *mb)
{
    VOICE(b);
    LOCK;
    set_mixbin_volumes(v, mb);
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetOutputBuffer(void *b, void *target)
{
    VOICE(b);
    LOCK;
    v->output = target ? as_voice(target) : nullptr;
    return DS_OK;
}

static int32_t __stdcall hle_Buffer_SetAllParameters(void *b, const void *p, uint32_t apply)
{
    VOICE(b);
    if (!p)
        return DS_OK;
    LOCK;
    v->p3d = *static_cast<const DS3DBUFFER *>(p);
    v->has3d = true;
    return DS_OK;
}
static int32_t __stdcall hle_Buffer_SetRolloffCurve(void *b, const float *curve, uint32_t n, uint32_t apply) { return DS_OK; }
static int32_t __stdcall hle_Buffer_SetI3DL2Source(void *b, const void *p, uint32_t apply) { return DS_OK; }

// ---- streams (non-virtual methods) -------------------------------------------------------------
static int32_t __stdcall hle_Stream_SetVolume(void *s, int32_t vol) { return hle_Buffer_SetVolume(s, vol); }
static int32_t __stdcall hle_Stream_SetHeadroom(void *s, uint32_t hr) { return hle_Buffer_SetHeadroom(s, hr); }
static int32_t __stdcall hle_Stream_SetMixBinVolumes(void *s, const DSMIXBINS *mb) { return hle_Buffer_SetMixBinVolumes(s, mb); }
static int32_t __stdcall hle_Stream_SetMixBins(void *s, const DSMIXBINS *mb) { return hle_Buffer_SetMixBins(s, mb); }
static int32_t __stdcall hle_Stream_SetFrequency(void *s, uint32_t f) { return hle_Buffer_SetFrequency(s, f); }
static int32_t __stdcall hle_Stream_SetAllParameters(void *s, const void *p, uint32_t apply) { return hle_Buffer_SetAllParameters(s, p, apply); }
static int32_t __stdcall hle_Stream_SetMode(void *s, uint32_t mode, uint32_t apply)
{
    VOICE(s);
    LOCK;
    v->p3d.dwMode = mode;
    return DS_OK;
}
static int32_t __stdcall hle_Stream_SetI3DL2Source(void *s, const void *p, uint32_t apply) { return DS_OK; }

static int32_t __stdcall hle_Stream_Pause(void *s, uint32_t pause)
{
    VOICE(s);
    LOCK;
    LOG_DEBUG("dsound", "stream %p Pause(%u), %zu packets queued", s, pause, v->packets.size());
    v->stream_paused = pause == 1;
    return DS_OK;
}

static int32_t __stdcall hle_Stream_FlushEx(void *s, int64_t rt, uint32_t flags)
{
    VOICE(s);
    stream_flush(v);
    return DS_OK;
}

} // extern "C"

void rt::hle_install_dsound()
{
    HOOK(0x0041E048, hle_DirectSoundCreate);
    HOOK(0x0041E08F, hle_DirectSoundCreateStream);
    HOOK(0x0041C4B5, hle_DirectSoundDoWork);
    HOOK(0x0041B14D, hle_DirectSoundUseLightHRTF);
    HOOK(0x0041B121, hle_IDirectSound_Release);
    HOOK(0x0041DE1A, hle_IDirectSound_CreateSoundBuffer);
    HOOK(0x0041DE3E, hle_IDirectSound_CreateSoundStream);
    HOOK(0x0041C309, hle_IDirectSound_SetMixBinHeadroom);
    HOOK(0x0041DE62, hle_IDirectSound_SetAllParameters);
    HOOK(0x0041DE82, hle_IDirectSound_SetI3DL2Listener);
    HOOK(0x0041DACD, hle_IDirectSound_CommitDeferredSettings);
    HOOK(0x0041C2ED, hle_IDirectSound_EnableHeadphones);
    HOOK(0x0041C2C6, hle_IDirectSound_DownloadEffectsImage);
    HOOK(0x0041C329, hle_IDirectSound_SynchPlayback);

    HOOK(0x0041B137, hle_Buffer_Release);
    HOOK(0x0041D5D6, hle_Buffer_SetBufferData);
    HOOK(0x0041CFB4, hle_Buffer_SetPlayRegion);
    HOOK(0x0041C411, hle_Buffer_SetLoopRegion);
    HOOK(0x0041C395, hle_Buffer_Play);
    HOOK(0x0041C3B9, hle_Buffer_Stop);
    HOOK(0x0041C3D1, hle_Buffer_StopEx);
    HOOK(0x0041C3F5, hle_Buffer_Pause);
    HOOK(0x0041C431, hle_Buffer_GetStatus);
    HOOK(0x0041C44D, hle_Buffer_GetCurrentPosition);
    HOOK(0x0041C46D, hle_Buffer_SetCurrentPosition);
    HOOK(0x0041D5BA, hle_Buffer_SetFormat);
    HOOK(0x0041CEFC, hle_Buffer_SetFrequency);
    HOOK(0x0041C341, hle_Buffer_SetVolume);
    HOOK(0x0041C35D, hle_Buffer_SetHeadroom);
    HOOK(0x0041CF34, hle_Buffer_SetMixBins);
    HOOK(0x0041C379, hle_Buffer_SetMixBinVolumes);
    HOOK(0x0041CF18, hle_Buffer_SetOutputBuffer);
    HOOK(0x0041CF50, hle_Buffer_SetAllParameters);
    HOOK(0x0041CF70, hle_Buffer_SetRolloffCurve);
    HOOK(0x0041CF94, hle_Buffer_SetI3DL2Source);

    HOOK(0x0041C489, hle_Stream_SetVolume);
    HOOK(0x0041C48E, hle_Stream_SetHeadroom);
    HOOK(0x0041C493, hle_Stream_SetMixBinVolumes);
    HOOK(0x0041C498, hle_Stream_Pause);
    HOOK(0x0041C49D, hle_Stream_FlushEx);
    HOOK(0x0041CFD4, hle_Stream_SetFrequency);
    HOOK(0x0041CFD9, hle_Stream_SetMixBins);
    HOOK(0x0041CFDE, hle_Stream_SetAllParameters);
    HOOK(0x0041CFE3, hle_Stream_SetMode);
    HOOK(0x0041CFE8, hle_Stream_SetI3DL2Source);

    xt_create_host(mixer_thread, nullptr, "audio-mixer", THREAD_PRIORITY_HIGHEST);
}
