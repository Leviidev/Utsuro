// audio.h -- the host's sound output: stereo float frames pushed from XAudio2's mixer.
#pragma once

#include <cstddef>

namespace utsuro::audio {

constexpr unsigned kRate = 48000;

/// Starts the output (idempotent); false if there is no audio device.
bool Init();
/// Queues `frames` interleaved stereo float frames at kRate.
void Write(const float* interleaved, size_t frames);

} // namespace utsuro::audio
