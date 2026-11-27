// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <windows.h>
#include <xaudio2.h>

#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
        if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf);
        if (FAILED(hr)) {
            LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
            return;
        }
        if (std::getenv("XBE_TEST_HIDDEN")) master->SetVolume(0.0f);  // [local change] a hidden test run is silent (a recording still gets the sound)
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        uint32_t depth = kMinQueued;
        ULONGLONG waiting = 0;
        bool said_stuck = false;
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGR("audio: output starved, queueing %u ms", depth * 10);
            }
            if (st.BuffersQueued >= depth) {
                // [local change, The Suffering: Ties That Bind] the sound card taking nothing for a second is said once
                // (a game whose sounds never end refuses new ones: the record must show whether the output stopped)
                if (!waiting) waiting = GetTickCount64();
                else if (!said_stuck && GetTickCount64() - waiting > 1000) {
                    said_stuck = true;
                    LOGW("audio: the output has taken nothing for a second (%u blocks queued)", st.BuffersQueued);
                }
                Sleep(2);
                continue;
            }
            if (said_stuck) LOGW("audio: the output takes blocks again after %llu ms", GetTickCount64() - waiting);
            waiting = 0;
            said_stuck = false;
            if (n && n % 1000 == 0) LOGI("audio: %u blocks of 10 ms mixed so far", n);
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            if (g_tap) g_tap(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
