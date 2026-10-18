#include "host/audio.h"

#include "engine/log.h"

#import <AVFoundation/AVFoundation.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace utsuro::audio {

namespace {

constexpr size_t kRingFrames = kRate;   // one second

struct Ring {
    std::vector<float> data = std::vector<float>(kRingFrames * 2, 0.f);
    std::atomic<size_t> read {0}, write {0};   // frame counters
};

Ring g_ring;
AVAudioEngine* g_engine = nil;
bool g_started = false;
std::mutex g_init_mutex;

} // namespace

bool Init() {
    std::scoped_lock lock {g_init_mutex};
    if (g_started) return g_engine != nil;
    g_started = true;
    @autoreleasepool {
#if TARGET_OS_IPHONE
        // iOS routes nothing until the app has a session: playback, so the silent switch does not mute a game.
        NSError* session_error = nil;
        AVAudioSession* session = [AVAudioSession sharedInstance];
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&session_error] || ![session setActive:YES error:&session_error])
            UT_WARN("audio: cannot activate the audio session: %s", [[session_error localizedDescription] UTF8String]);
#endif
        g_engine = [[AVAudioEngine alloc] init];
        AVAudioFormat* format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32 sampleRate:kRate channels:2 interleaved:NO];
        AVAudioSourceNode* node = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL* silence, const AudioTimeStamp*, AVAudioFrameCount frames, AudioBufferList* list) {
            float* left = static_cast<float*>(list->mBuffers[0].mData);
            float* right = list->mNumberBuffers > 1 ? static_cast<float*>(list->mBuffers[1].mData) : left;
            size_t r = g_ring.read.load(std::memory_order_acquire);
            const size_t w = g_ring.write.load(std::memory_order_acquire);
            const size_t have = w - r;
            const size_t n = std::min<size_t>(frames, have);
            for (size_t i = 0; i < n; ++i) {
                const size_t k = ((r + i) % kRingFrames) * 2;
                left[i] = g_ring.data[k];
                right[i] = g_ring.data[k + 1];
            }
            for (size_t i = n; i < frames; ++i) left[i] = right[i] = 0.f;
            g_ring.read.store(r + n, std::memory_order_release);
            *silence = n == 0 ? YES : NO;
            return noErr;
        }];
        [g_engine attachNode:node];
        [g_engine connect:node to:g_engine.mainMixerNode format:format];
        NSError* err = nil;
        if (![g_engine startAndReturnError:&err]) {
            UT_WARN("audio: cannot start the engine: %s", [[err localizedDescription] UTF8String]);
            g_engine = nil;
            return false;
        }
    }
    UT_INFO("audio: output running at %u Hz", kRate);
    return true;
}

void Write(const float* interleaved, size_t frames) {
    if (g_engine == nil) return;
    size_t w = g_ring.write.load(std::memory_order_relaxed);
    const size_t r = g_ring.read.load(std::memory_order_acquire);
    // Never let the title run more than ~300 ms ahead of what is heard: drop the oldest instead.
    const size_t limit = kRate * 3 / 10;
    if (w - r + frames > limit) {
        const size_t drop = w - r + frames - limit;
        g_ring.read.store(r + std::min(drop, w - r), std::memory_order_release);
    }
    for (size_t i = 0; i < frames; ++i) {
        const size_t k = ((w + i) % kRingFrames) * 2;
        g_ring.data[k] = interleaved[i * 2];
        g_ring.data[k + 1] = interleaved[i * 2 + 1];
    }
    g_ring.write.store(w + frames, std::memory_order_release);
}

} // namespace utsuro::audio
