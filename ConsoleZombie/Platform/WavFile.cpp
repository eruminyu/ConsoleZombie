#include "WavFile.h"

#include <cstring>
#include <fstream>

namespace Zombie
{
    namespace
    {
        constexpr std::uint16_t FormatPcm = 1;
        // WAVE_FORMAT_EXTENSIBLE. 평범한 PCM 에도 이 값을 쓰는 인코더가 흔해서
        // 거절하면 다른 모든 면에서 정상인 파일을 버리게 된다. 서브 포맷은 청크
        // 더 뒤에 있고, 우리가 확인하는 비트 깊이는 두 경우 모두 같은 자리에 있다.
        constexpr std::uint16_t FormatExtensible = 0xFFFE;

        std::uint16_t ReadU16(const std::uint8_t* at)
        {
            return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
        }

        std::uint32_t ReadU32(const std::uint8_t* at)
        {
            return static_cast<std::uint32_t>(at[0])
                | (static_cast<std::uint32_t>(at[1]) << 8)
                | (static_cast<std::uint32_t>(at[2]) << 16)
                | (static_cast<std::uint32_t>(at[3]) << 24);
        }

        // 청크 이름 네 글자 비교.
        bool Tag(const std::uint8_t* at, const char* name)
        {
            return std::memcmp(at, name, 4) == 0;
        }
    }

    // RIFF 헤더를 확인하고 청크를 끝까지 걸으며 fmt 와 data 를 줍는다.
    WavData ParseWav(const std::uint8_t* bytes, std::size_t size)
    {
        WavData clip;
        if (!bytes || size < 12) return clip;
        if (!Tag(bytes, "RIFF") || !Tag(bytes + 8, "WAVE")) return clip;

        int bitsPerSample = 0;
        const std::uint8_t* data = nullptr;
        std::size_t dataSize = 0;

        // 청크는 파일 끝까지 이어진다. 하나가 헤더 8바이트 + 페이로드다.
        // 홀수 크기 페이로드는 짝수로 패딩되는데, 그 패딩 바이트를 건너뛰는 것이
        // 패딩 있는 파일에서 순회를 어긋나지 않게 해준다.
        std::size_t offset = 12;
        while (offset + 8 <= size)
        {
            const std::uint32_t chunkSize = ReadU32(bytes + offset + 4);
            const std::size_t payload = offset + 8;
            if (payload + chunkSize > size) break;

            if (Tag(bytes + offset, "fmt ") && chunkSize >= 16)
            {
                const std::uint16_t format = ReadU16(bytes + payload);
                if (format != FormatPcm && format != FormatExtensible) return WavData{};
                clip.channels = ReadU16(bytes + payload + 2);
                clip.sampleRate = static_cast<int>(ReadU32(bytes + payload + 4));
                bitsPerSample = ReadU16(bytes + payload + 14);
            }
            else if (Tag(bytes + offset, "data"))
            {
                data = bytes + payload;
                dataSize = chunkSize;
            }

            offset = payload + chunkSize + (chunkSize & 1u);
        }

        if (bitsPerSample != 16 || clip.channels <= 0 || clip.sampleRate <= 0 || !data)
        {
            return WavData{};
        }

        clip.samples.resize(dataSize / sizeof(std::int16_t));
        // 캐스트가 아니라 memcpy 인 이유: 페이로드가 int16 읽기에 정렬돼 있다는
        // 보장이 없다. 리틀 엔디언은 이 게임이 도는 모든 기기에서 참이다.
        if (!clip.samples.empty()) std::memcpy(clip.samples.data(), data, clip.samples.size() * sizeof(std::int16_t));
        return clip;
    }

    // 파일 전체를 읽어 파서에 넘긴다. 자산이 전부 몇백 KB 라 스트리밍하지 않는다.
    WavData LoadWav(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return WavData{};
        const std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return ParseWav(bytes.data(), bytes.size());
    }

    // 채널 평균으로 모노를 만든다. 이미 모노면 아무 일도 안 한다.
    void DownmixToMono(WavData& clip)
    {
        if (clip.channels <= 1) return;
        const std::size_t frames = clip.FrameCount();
        std::vector<std::int16_t> mono(frames);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            // 나누기 전에 int 로 더한다. 제자리에서 더하면 최대 진폭에 가까운
            // 두 채널이 평균을 내는 도중에 int16 을 넘친다.
            int total = 0;
            for (int channel = 0; channel < clip.channels; ++channel)
            {
                total += clip.samples[frame * static_cast<std::size_t>(clip.channels)
                    + static_cast<std::size_t>(channel)];
            }
            mono[frame] = static_cast<std::int16_t>(total / clip.channels);
        }
        clip.samples = std::move(mono);
        clip.channels = 1;
    }
}
