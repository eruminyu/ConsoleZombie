#pragma once

#include <Platform/AudioDevice.h>
#include <Platform/WavFile.h>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace Zombie
{
    // 게임이 낼 수 있는 소리 전부. 하나 더하려면 여기 한 줄, AudioCues.cpp 의 표에
    // 한 줄, 그리고 소리가 나는 자리에 호출 하나다. 그 다음 Assets/Sound 에 이름이
    // 맞는 wav 를 떨궈 넣으면 코드를 더 안 고쳐도 소리가 난다.
    enum class AudioCue
    {
        Gunshot,
        Footstep,
        AmmoPickup,
        KeyPickup,
        Escaped,
        Dead,
        BreachKnock,
        DoorOpen,
        DoorBreak,
        WindowBreak,
        ZombieGrowl,
        BgmLoop,
        Count
    };

    enum class AudioCueKind
    {
        // 가운데에서, 감쇠 없이. 플레이어가 한 일과 게임이 알려주는 것.
        Ui,
        // 위치가 있다. 음향 필드로 감쇠되고 좌우로 배치된다. 패닝 행렬이 1x2 가
        // 되도록 불러올 때 모노로 접는다.
        World,
        // 자기 전용 보이스에서 자기 음량으로 무한 반복.
        Music
    };

    struct AudioCueInfo
    {
        const char* file;
        AudioCueKind kind;
        // 큐 하나의 보정값. 효과음/배경음 음량 위에 곱해진다. 자산은 서로 크기가
        // 맞춰진 채로 오지 않으므로, 파일을 건드리지 않고 여기서 맞춘다.
        float gain;
    };

    const AudioCueInfo& CueInfo(AudioCue cue);

    // Assets/Sound 를 찾는다. Balance 가 설정을 찾는 방식과 같다 — 실행 파일 옆부터,
    // 그 다음 개발 배치를 위해 위로 올라가며, 마지막으로 작업 디렉터리.
    // 아무 데도 없으면 빈 경로이고, 그것은 그냥 모든 큐가 조용하다는 뜻이다.
    std::filesystem::path FindSoundFolder();

    // 디코드된 클립들과 장치를 소유한다.
    //
    // **여기서는 아무것도 던지지 않고 아무것도 요란하게 실패하지 않는다.** 장치가
    // 없는 것과 파일이 없는 것이 같은 곳에서 끝난다 — 그 큐는 소리가 안 나고 게임은
    // 계속 간다. 보고는 시작할 때의 요약 한 줄뿐이다.
    class AudioBank
    {
    public:
        // 장치를 열고 있는 파일을 불러온다. 성공하면 빈 문자열, 아니면 상태 한 줄.
        //
        // **자체 테스트가 도는 생성자에서 부르면 안 된다.** 자체 테스트가 Game 을
        // 열댓 개 만드는데, 그러면 XAudio2 엔진이 열댓 개 뜬다. Game::Run() 을 보라.
        std::string Open(const std::filesystem::path& soundRoot,
            bool enabled, float masterVolume, float sfxVolume, float bgmVolume);

        bool IsOpen() const { return device.IsOpen(); }

        // 세 음량을 다시 준다. 장치도 클립도 건드리지 않는다. 밸런스 파일은 판 도중에
        // 다시 읽을 수 있고, 그중 귀가 가장 먼저 알아채는 부분이 이것이다.
        void SetVolumes(float masterVolume, float sfxVolume, float bgmVolume);

        // 가운데에서, 큐 자신의 보정값에 volume 을 곱해서. volume 은 **어디서 났는지가
        // 아니라 무엇을 했는지가** 크기를 정하는 큐를 위한 것이다 — 달리는 발소리와
        // 걷는 발소리의 차이 같은 것.
        void Play(AudioCue cue, float volume = 1.0f);

        // volume 은 음향 필드에서 온 0..1, pan 은 -1..1.
        void PlayPositional(AudioCue cue, float volume, float pan);

        void PlayMusic(AudioCue cue);
        void StopMusic();

    private:
        AudioDevice device;
        // **크기가 고정이고 Open 뒤에 절대 재할당되지 않는다.** XAudio2 가 보이스를
        // 재생하는 동안 이 샘플들을 있는 자리에서 직접 읽기 때문이다. 재생 중인
        // 클립을 재할당하는 것이 이 API 의 대표적인 크래시다.
        std::array<WavData, static_cast<std::size_t>(AudioCue::Count)> clips;
        std::vector<std::string> missing;
        float sfxGain = 1.0f;
        float bgmGain = 0.35f;
    };
}
