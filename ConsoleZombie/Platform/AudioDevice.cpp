#include "AudioDevice.h"

#include <Windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

// XAudio2 9 는 Windows 자체에 들어 있다. 그래서 빌드 준비가 이 두 줄이 전부다 —
// 재배포 패키지도, 프로젝트 파일의 추가 링커 입력도 없다.
//
// Media Foundation 도 안 쓴다. 모든 자산이 wav 안의 PCM 이고 파서가 그것을 직접
// 다루므로 시작할 디코더가 없다. COM 은 별개 문제이고 어차피 필요하다 —
// 무엇이 그걸 실측하게 했는지는 Open() 에 있다.
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace Zombie
{
    namespace
    {
        // 같은 포맷의 소리 몇 개가 겹치기에 충분한 수. 이 수를 넘으면 무한정
        // 늘리는 대신 새 소리가 가장 오래된 보이스를 뺏는다. 깨어난 좀비로 가득한
        // 방이 같은 초에 침입구 여러 곳을 두드릴 수 있어서 필요한 규칙이다.
        constexpr std::size_t MaxVoicesPerFormat = 8;

        // 클립의 포맷을 WAVEFORMATEX 로 옮긴다. 16비트 PCM 만 다루므로 고정값이 많다.
        void FillFormat(WAVEFORMATEX& format, const WavData& clip)
        {
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = static_cast<WORD>(clip.channels);
            format.nSamplesPerSec = static_cast<DWORD>(clip.sampleRate);
            format.wBitsPerSample = 16;
            format.nBlockAlign = static_cast<WORD>(clip.channels * 2);
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            format.cbSize = 0;
        }

        // 재생 버퍼. pAudioData 가 클립의 메모리를 그대로 가리킨다 — 복사하지 않는다.
        // 그래서 뱅크가 클립을 재할당하면 안 된다.
        XAUDIO2_BUFFER MakeBuffer(const WavData& clip, bool loop)
        {
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = static_cast<UINT32>(clip.samples.size() * sizeof(std::int16_t));
            buffer.pAudioData = reinterpret_cast<const BYTE*>(clip.samples.data());
            buffer.Flags = XAUDIO2_END_OF_STREAM;
            if (loop) buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
            return buffer;
        }
    }

    AudioDevice::~AudioDevice()
    {
        Close();
    }

    // 엔진과 마스터링 보이스를 연다. 실패해도 게임은 그대로 돈다.
    std::string AudioDevice::Open()
    {
        if (engine) return {};

        // XAudio2 는 이 스레드에 COM 이 필요하다. 그리고 **첫 번째 호출이 아니라
        // 두 번째 호출에서** 필요하다. XAudio2Create 는 평범한 export 라 아파트먼트
        // 없이도 성공한다. 그 다음 CreateMasteringVoice 가 MMDevice 로 오디오
        // 엔드포인트를 열거하는데 그게 COM 이고, CO_E_NOTINITIALIZED (0x800401F0) 로
        // 실패한다.
        //
        // 이 PC 실측: 이 줄이 없으면 엔진은 멀쩡히 만들어진 것처럼 보이고, 헤드폰에서
        // 소리가 나고 있는데도 마스터링 보이스가 "출력 장치 없음"처럼 읽히는 값을
        // 돌려준다.
        //
        // 이건 이 게임이 하지 않는 Media Foundation 초기화와는 다른 얘기다. 자산이
        // 전부 wav 안의 PCM 이라 MFStartup 은 없다. 아파트먼트는 디코더가 아니라
        // 오디오 장치를 위한 것이다.
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // S_FALSE 는 스레드에 이미 아파트먼트가 있었다는 뜻이고, 문서는 그래도
        // 짝을 맞추라고 한다. RPC_E_CHANGED_MODE 는 다른 종류가 있었다는 뜻이고
        // 쓸 수는 있지만 짝을 맞추면 안 된다. 그래서 **성공한 호출에만** 짝을 맞춘다.
        ownsCom = SUCCEEDED(comResult);

        if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            engine = nullptr;
            ReleaseCom();
            return "오디오 장치를 열지 못했다";
        }
        if (const HRESULT mastered = engine->CreateMasteringVoice(&master); FAILED(mastered))
        {
            engine->Release();
            engine = nullptr;
            master = nullptr;
            ReleaseCom();
            // 코드를 같이 찍는다. "출력 장치 없음"이라는 말이 이미 한 번 틀렸고,
            // 다음 사람은 찾아볼 수 있는 것을 받을 자격이 있다.
            char reason[64]{};
            std::snprintf(reason, sizeof(reason), "오디오 출력을 열지 못했다 (0x%08lX)",
                static_cast<unsigned long>(mastered));
            return reason;
        }
        return {};
    }

    // 보이스를 먼저, 엔진을 나중에 정리한다.
    void AudioDevice::Close()
    {
        // 보이스가 붙어 있는 채로 엔진을 없애는 것은 정의되지 않은 동작이다.
        // 음악 보이스는 풀에 없으므로 따로 처리한다.
        if (musicVoice)
        {
            musicVoice->Stop(0);
            musicVoice->DestroyVoice();
            musicVoice = nullptr;
        }
        for (VoicePool& pool : pools)
        {
            for (IXAudio2SourceVoice* voice : pool.voices)
            {
                if (!voice) continue;
                voice->Stop(0);
                voice->DestroyVoice();
            }
        }
        pools.clear();
        if (master)
        {
            master->DestroyVoice();
            master = nullptr;
        }
        if (engine)
        {
            engine->Release();
            engine = nullptr;
        }
        // 엔진 다음이지 절대 그 전이 아니다. 살아 있는 XAudio2 객체 밑에서
        // 아파트먼트를 해제하는 것은 그것이 들고 있는 장치에 대한 use-after-free 다.
        ReleaseCom();
    }

    // Open() 이 실제로 초기화를 떠맡았을 때만 해제한다.
    void AudioDevice::ReleaseCom()
    {
        if (!ownsCom) return;
        CoUninitialize();
        ownsCom = false;
    }

    // 마스터 음량. 이미 재생 중인 소리에도 닿는다.
    void AudioDevice::SetMasterVolume(float volume)
    {
        if (master) master->SetVolume(std::clamp(volume, 0.0f, 1.0f));
    }

    // 이 클립의 포맷으로 쓸 수 있는 소스 보이스 하나를 내준다.
    //
    // 노는 것을 먼저 재활용하고, 없으면 새로 만들고, 한도를 넘었으면 뺏는다.
    IXAudio2SourceVoice* AudioDevice::AcquireVoice(const WavData& clip)
    {
        VoicePool* pool = nullptr;
        for (VoicePool& candidate : pools)
        {
            if (candidate.sampleRate == clip.sampleRate && candidate.channels == clip.channels)
            {
                pool = &candidate;
                break;
            }
        }
        if (!pool)
        {
            pools.push_back(VoicePool{ clip.sampleRate, clip.channels, {} });
            pool = &pools.back();
        }

        // 큐에 아무것도 없는 보이스가 노는 보이스다. 재사용하면 생성 대신 제출
        // 한 번으로 끝난다. 소리마다 소스 보이스를 만드는 값은 여럿이 동시에 울릴 때
        // 끊김으로 들릴 만큼 비싸다.
        for (IXAudio2SourceVoice* voice : pool->voices)
        {
            XAUDIO2_VOICE_STATE state{};
            voice->GetState(&state);
            if (state.BuffersQueued == 0) return voice;
        }

        if (pool->voices.size() >= MaxVoicesPerFormat)
        {
            // 전부 바쁘다. 한없이 늘리는 대신 맨 앞 것을 뺏는다. 가장 오래된 소리가
            // 사라져도 가장 덜 눈치채인다.
            IXAudio2SourceVoice* voice = pool->voices.front();
            voice->Stop(0);
            voice->FlushSourceBuffers();
            return voice;
        }

        WAVEFORMATEX format{};
        FillFormat(format, clip);
        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(engine->CreateSourceVoice(&voice, &format))) return nullptr;
        pool->voices.push_back(voice);
        return voice;
    }

    // 한 번 울리는 소리. 음량과 좌우 위치를 받는다.
    void AudioDevice::PlayOneShot(const WavData& clip, float volume, float pan)
    {
        if (!engine || !clip.Valid() || volume <= 0.0f) return;
        IXAudio2SourceVoice* voice = AcquireVoice(clip);
        if (!voice) return;

        voice->Stop(0);
        voice->FlushSourceBuffers();
        const XAUDIO2_BUFFER buffer = MakeBuffer(clip, false);
        if (FAILED(voice->SubmitSourceBuffer(&buffer))) return;
        voice->SetVolume(std::clamp(volume, 0.0f, 1.0f));

        if (clip.channels == 1)
        {
            // 정파워(constant power) 패닝. 플레이어 앞을 스쳐 지나가는 소리가
            // 가운데에서 음량이 꺼지지 않는다. 선형 한 쌍으로 하면 꺼진다.
            const float clamped = std::clamp(pan, -1.0f, 1.0f);
            const float matrix[2] = {
                std::sqrt((1.0f - clamped) * 0.5f),
                std::sqrt((1.0f + clamped) * 0.5f) };
            // 1x2 — 원본 채널 하나를 출력 채널 둘로. 이 행렬의 크기가
            // 원본 채널 × 출력 채널이라, World 큐를 불러올 때 모노로 접는 것이다.
            // 스테레오 그대로 패닝하면 2x2 가 되고 좌우가 섞인다.
            voice->SetOutputMatrix(nullptr, 1, 2, matrix);
        }
        voice->Start(0);
    }

    // 배경음. 자기 전용 보이스를 새로 만들어 무한 반복한다.
    void AudioDevice::PlayMusic(const WavData& clip, float volume)
    {
        if (!engine || !clip.Valid()) return;
        StopMusic();
        WAVEFORMATEX format{};
        FillFormat(format, clip);
        if (FAILED(engine->CreateSourceVoice(&musicVoice, &format)))
        {
            musicVoice = nullptr;
            return;
        }
        const XAUDIO2_BUFFER buffer = MakeBuffer(clip, true);
        if (FAILED(musicVoice->SubmitSourceBuffer(&buffer)))
        {
            musicVoice->DestroyVoice();
            musicVoice = nullptr;
            return;
        }
        musicVoice->SetVolume(std::clamp(volume, 0.0f, 1.0f));
        musicVoice->Start(0);
    }

    // 배경음을 멈추고 보이스를 없앤다. PlayMusic() 이 매번 새로 만들므로 남겨둘
    // 이유가 없다.
    void AudioDevice::StopMusic()
    {
        if (!musicVoice) return;
        musicVoice->Stop(0);
        musicVoice->FlushSourceBuffers();
        musicVoice->DestroyVoice();
        musicVoice = nullptr;
    }
}
