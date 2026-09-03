#include "AudioCues.h"

#include <Windows.h>

#include <algorithm>

namespace Zombie
{
    namespace
    {
        // 파일 이름이 고정이다. 이 이름 중 하나로 wav 를 Assets/Sound 에 떨궈 넣으면
        // 그 큐가 소리를 내기 시작하고, 다른 것은 아무것도 안 고쳐도 된다.
        //
        // 큐 열두 개이고 지금은 전부 파일이 있다 (d9bdd03 에서 door_open.wav 로
        // 마지막 빈 칸이 채워졌다). 빈 칸이 생겨도 그 큐만 조용할 뿐이므로, 슬롯을
        // 먼저 만들어 두는 것이 나중에 열거형·표·호출부를 한꺼번에 고치는 것보다 낫다.
        constexpr std::array<AudioCueInfo, static_cast<std::size_t>(AudioCue::Count)> Table = { {
            { "gunshot.wav",      AudioCueKind::Ui,    0.85f },
            // 일부러 작다. 보폭마다 울리는 소리이고, 총성과 겨루는 발소리는 배경이기를
            // 그만두고 게임 자체가 되어버린다.
            { "footstep.wav",     AudioCueKind::Ui,    0.45f },
            { "ammo_pickup.wav",  AudioCueKind::Ui,    0.90f },
            { "key_pickup.wav",   AudioCueKind::Ui,    0.90f },
            { "escaped.wav",      AudioCueKind::Ui,    1.00f },
            { "dead.wav",         AudioCueKind::Ui,    1.00f },
            { "breach_knock.wav", AudioCueKind::World, 1.00f },
            // 문이 열리는 소리를, 그 일이 일어난 자리에서 듣는다. 플레이어가 연 문과
            // 좀비가 밀어 연 문은 **같은 소리**다. 다른 것은 어디서 났는지뿐이고
            // 음향 필드가 이미 그것을 말한다.
            { "door_open.wav",    AudioCueKind::World, 0.85f },
            { "door_break.wav",   AudioCueKind::World, 1.00f },
            { "window_break.wav", AudioCueKind::World, 1.00f },
            { "zombie_growl.wav", AudioCueKind::World, 0.90f },
            { "bgm_loop.wav",     AudioCueKind::Music, 1.00f },
        } };

        // 배열 크기는 열거형에서 나오는데 초기화는 중괄호 목록으로 한다. 그리고
        // **목록이 짧으면 컴파일 오류가 아니라 0으로 채워진다.** 이게 없으면 열거형에
        // 항목을 더하고 표에 줄을 안 더했을 때 이름 없는 큐가 조용히 생긴다.
        // 아무것도 안 불러오고 아무 말도 안 한다.
        constexpr bool EveryCueNamed()
        {
            for (const AudioCueInfo& info : Table)
            {
                if (!info.file || info.file[0] == '\0') return false;
            }
            return true;
        }
        static_assert(EveryCueNamed(),
            "every AudioCue needs a row in Table; the array zero-fills rather than erroring");
    }

    // 큐 하나의 파일 이름·종류·보정값.
    const AudioCueInfo& CueInfo(AudioCue cue)
    {
        return Table[static_cast<std::size_t>(cue)];
    }

    // Assets/Sound 폴더를 찾는다. 못 찾으면 빈 경로.
    std::filesystem::path FindSoundFolder()
    {
        const std::filesystem::path relative = std::filesystem::path("Assets") / "Sound";
        std::error_code error;

        // argv[0] 이 아니라 모듈 경로를 쓴다. 그래야 main 에서 아무것도 내려보내지
        // 않아도 된다. 배포는 Assets 를 실행 파일 옆에 두고, 개발 배치는 Bin 위로
        // 몇 단계 올라간 곳에 둔다.
        wchar_t module[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0)
        {
            std::filesystem::path directory = std::filesystem::path(module).parent_path();
            for (int depth = 0; depth < 8 && !directory.empty(); ++depth)
            {
                const std::filesystem::path candidate = directory / relative;
                if (std::filesystem::is_directory(candidate, error)) return candidate;
                const std::filesystem::path parent = directory.parent_path();
                if (parent.empty() || parent == directory) break;
                directory = parent;
            }
        }

        // 마지막 수단은 작업 디렉터리. Visual Studio 가 잡아주는 것이 그것이다.
        const std::filesystem::path here = std::filesystem::current_path(error) / relative;
        if (!error && std::filesystem::is_directory(here, error)) return here;
        return {};
    }

    // 장치를 열고 표의 파일을 전부 불러온다. 없는 것은 조용히 넘어간다.
    std::string AudioBank::Open(const std::filesystem::path& soundRoot,
        bool enabled, float masterVolume, float sfxVolume, float bgmVolume)
    {
        sfxGain = std::clamp(sfxVolume, 0.0f, 1.0f);
        bgmGain = std::clamp(bgmVolume, 0.0f, 1.0f);
        missing.clear();

        if (!enabled) return "소리가 꺼져 있다 (audio_enabled = 0)";

        // 장치가 먼저다. 장치가 없으면 재생할 곳이 없으니, 샘플 수 메가바이트를
        // 디코드하는 것은 버릴 일을 하는 것이다.
        if (const std::string failure = device.Open(); !failure.empty()) return failure;
        device.SetMasterVolume(masterVolume);

        for (std::size_t index = 0; index < Table.size(); ++index)
        {
            const AudioCueInfo& info = Table[index];
            WavData clip = LoadWav(soundRoot / info.file);
            if (!clip.Valid())
            {
                // 파일이 없거나, 이 파서가 받지 않는 파일이다. 여기서부터는 둘이
                // 같은 것이다 — 소리가 안 나는 큐.
                missing.emplace_back(info.file);
                continue;
            }
            // World 큐는 좌우로 배치되고, 배치하려면 원본 채널이 하나여야 한다.
            // Ui 와 Music 은 파일이 가진 그대로 둔다.
            if (info.kind == AudioCueKind::World) DownmixToMono(clip);
            clips[index] = std::move(clip);
        }

        if (missing.empty()) return {};
        std::string line = "소리 없는 큐 " + std::to_string(missing.size()) + "개: ";
        for (std::size_t index = 0; index < missing.size(); ++index)
        {
            if (index > 0) line += ", ";
            line += missing[index];
        }
        return line;
    }

    // 세 음량을 갱신한다. F5 로 밸런스를 다시 읽을 때 불린다.
    void AudioBank::SetVolumes(float masterVolume, float sfxVolume, float bgmVolume)
    {
        sfxGain = std::clamp(sfxVolume, 0.0f, 1.0f);
        bgmGain = std::clamp(bgmVolume, 0.0f, 1.0f);
        // 마스터는 장치로 가므로 **이미 재생 중인 소리에도 닿는다.** 나머지 둘은
        // 재생할 때마다 적용되므로 이 뒤에 시작하는 것에만 영향을 준다.
        if (device.IsOpen()) device.SetMasterVolume(masterVolume);
    }

    // 가운데에서 한 번. 플레이어 자신이 낸 소리에 쓴다.
    void AudioBank::Play(AudioCue cue, float volume)
    {
        const WavData& clip = clips[static_cast<std::size_t>(cue)];
        device.PlayOneShot(clip, sfxGain * CueInfo(cue).gain
            * std::clamp(volume, 0.0f, 1.0f), 0.0f);
    }

    // 세계 어딘가에서 난 소리. 음량과 좌우 위치를 호출자가 계산해서 준다.
    void AudioBank::PlayPositional(AudioCue cue, float volume, float pan)
    {
        if (volume <= 0.0f) return;
        const WavData& clip = clips[static_cast<std::size_t>(cue)];
        device.PlayOneShot(clip, sfxGain * CueInfo(cue).gain * std::clamp(volume, 0.0f, 1.0f), pan);
    }

    void AudioBank::PlayMusic(AudioCue cue)
    {
        device.PlayMusic(clips[static_cast<std::size_t>(cue)], bgmGain * CueInfo(cue).gain);
    }

    void AudioBank::StopMusic()
    {
        device.StopMusic();
    }
}
