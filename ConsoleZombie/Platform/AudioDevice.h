#pragma once

#include <Platform/WavFile.h>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace Zombie
{
    // XAudio2 를 이 게임이 필요로 하는 최소한으로 감쌌다 — 클립 하나를 음량과 좌우
    // 위치를 주고 한 번 재생하거나, 한 클립을 계속 돌리거나.
    //
    // Open() 이 성공하기 전까지 모든 메서드가 아무 일도 안 한다. 그리고 Open() 실패는
    // 게임이 반응하는 오류가 아니다. 소리 장치가 없는 기기는 소리가 안 날 뿐 나머지가
    // 똑같이 동작하고, 덕분에 자체 테스트가 하드웨어를 건드리지 않고 재생 경로를
    // 전부 밟을 수 있다.
    class AudioDevice
    {
    public:
        AudioDevice() = default;
        ~AudioDevice();
        AudioDevice(const AudioDevice&) = delete;
        AudioDevice& operator=(const AudioDevice&) = delete;

        // 성공하면 빈 문자열, 아니면 상태 로그에 그대로 올릴 짧은 사유.
        std::string Open();
        void Close();
        bool IsOpen() const { return engine != nullptr; }

        void SetMasterVolume(float volume);

        // pan 은 -1(왼쪽)에서 1(오른쪽)이고 **단일 채널 클립에만** 적용된다.
        // 모노 원본의 출력 행렬은 1x2 이고, 그게 채널을 서로 접지 않고 패닝되는
        // 유일한 형태다. 다채널 클립은 만든 그대로 재생된다.
        //
        // **클립이 소리보다 오래 살아야 한다.** XAudio2 는 재생 중에 호출자의 메모리에서
        // 샘플을 직접 읽으므로, 클립을 들고 있는 뱅크가 재할당하면 안 된다. 이 API 의
        // 대표적인 크래시가 그것이다.
        void PlayOneShot(const WavData& clip, float volume, float pan);

        // StopMusic() 까지 반복한다. 자기 전용 보이스를 쓰고 원샷 풀에서 가져오지
        // 않는다. 총성이 연달아 나도 음악이 밀려나지 않는다.
        void PlayMusic(const WavData& clip, float volume);
        void StopMusic();

    private:
        // 원샷 보이스를 포맷별로 나눠 모아둔다. 소스 보이스는 만들 때 WAVEFORMATEX 가
        // 고정되기 때문이다. 이 게임의 자산은 48kHz 와 24kHz 두 종류라, 공용 포맷
        // 하나로 두면 한쪽을 리샘플해야 한다. 포맷마다 풀을 두면 파서가 받아들이는
        // 어떤 파일이든 있는 그대로 재생된다.
        struct VoicePool
        {
            int sampleRate = 0;
            int channels = 0;
            std::vector<IXAudio2SourceVoice*> voices;
        };

        IXAudio2SourceVoice* AcquireVoice(const WavData& clip);

        // Open() 의 CoInitializeEx 와 짝을 맞춘다. **거기서 실제로 초기화를 떠맡았을
        // 때만** 부른다 — 남이 이미 초기화한 아파트먼트를 우리가 해제하면 안 된다.
        void ReleaseCom();

        IXAudio2* engine = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* musicVoice = nullptr;
        std::vector<VoicePool> pools;
        bool ownsCom = false;
    };
}
