#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Zombie
{
    // 디코드된 클립 하나. 16비트 인터리브 샘플이고, XAudio2 가 원하는 형태이자
    // 이 게임의 모든 자산이 실제로 가진 형태다.
    struct WavData
    {
        int sampleRate = 0;
        int channels = 0;
        std::vector<std::int16_t> samples;

        bool Valid() const { return sampleRate > 0 && channels > 0 && !samples.empty(); }

        // 샘플이 아니라 프레임 수다. 스테레오 클립은 벡터 원소 수의 절반이다.
        std::size_t FrameCount() const
        {
            return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
        }

        float Seconds() const
        {
            return sampleRate > 0 ? static_cast<float>(FrameCount()) / static_cast<float>(sampleRate) : 0.0f;
        }
    };

    // 고정 오프셋으로 읽지 않고 RIFF 청크 목록을 순회한다.
    //
    // 방어적으로 짠 것이 아니라 실측 대응이다. 이 게임의 자산 둘 다 fmt 와 data
    // 사이에 26바이트 LIST 청크를 달고 있어서 샘플이 0x4E 에서 시작한다. 최소한의
    // 라이터가 만드는 0x2C 가 아니다. 짧은 배치를 가정한 파서는 **이미 있는 파일
    // 두 개에서 메타데이터를 오디오로 읽고 조용히 잡음을 낸다.**
    //
    // PCM 16비트만 받는다. 나머지는 무효로 돌려주고, 호출자는 그것을 오류가 아니라
    // 소리 없는 큐로 다룬다.
    WavData ParseWav(const std::uint8_t* bytes, std::size_t size);

    // 파일이 없으면 무효한 WavData 를 돌려준다. 파일 없는 큐는 실패가 아니라
    // 조용한 큐다.
    WavData LoadWav(const std::filesystem::path& path);

    // 채널을 평균 내 하나로 접는다.
    //
    // World 큐만 불러올 때 이걸 거친다. 메모리가 절반이 되는 것도 있지만 진짜 이유는
    // XAudio2 출력 행렬이 1x2 가 되어야 패닝이 깨끗하기 때문이다. 행렬 크기는
    // 원본 채널 × 출력 채널이라, 스테레오를 패닝하려면 2x2 가 되고 결국 좌우가 섞인다.
    void DownmixToMono(WavData& clip);
}
