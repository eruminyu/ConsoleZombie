#pragma once

#include <Core/Types.h>
#include <Render/PixelBuffer.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Zombie
{
    // UI 텍스트 한 덩어리. 위치의 단위가 터미널 셀이 아니라 **프레임 픽셀**이다.
    //
    // 셀은 틀린 단위였다. 셀 크기는 터미널 폰트가 정하고 게임은 그것을 못 바꾼다.
    // 그래서 QHD 화면에서 한글 한 음절이 화면 픽셀로 9x10 쯤 나왔고
    // 돌릴 손잡이가 없었다. 프레임 픽셀은 렌더 해상도를 따라 커지고, 렌더 해상도는
    // 게임 것이다. 덕분에 어느 화면에서든 글자가 화면의 같은 비율을 차지한다.
    struct TextSpan
    {
        Int2 pixel{};
        std::string text;
        Rgb color{ 236, 232, 220 };
        // em 높이(프레임 픽셀). 구운 마스크는 이보다 조금 더 높다.
        int height = 12;
    };

    // UTF-8 을 GDI 로 커버리지 마스크에 구워 캐시한다.
    //
    // UI 개편 때 비트맵 폰트로는 7픽셀 높이의 한글을 그릴 수 없다는 결론이 났다.
    // 맞는 말이지만, **게임이 글꼴을 가질 필요가 애초에 없었다** — 기기에 이미 들어
    // 있다. 어느 쪽이든 안티에일리어싱은 끄므로 글리프가 회색 뭉개짐이 아니라
    // 딱 떨어지는 켜짐/꺼짐 픽셀로 나온다. 픽셀 프레임이 원하는 것이 정확히 그것이다.
    //
    // **굵기는 후보마다 다르고 일괄 규칙이 없다.** HUD 크기에서 곡선을 임계값으로
    // 자르는 일은 한 글꼴을 살리고 다른 글꼴을 망친다. 글꼴별 근거는 전부 실측이고
    // TextRaster.cpp 에 있다.
    class TextRaster
    {
    public:
        struct Mask
        {
            int width = 0;
            int height = 0;
            // width * height 개, 값은 0 아니면 255. GDI 가 거절하면 비어 있다.
            std::vector<std::uint8_t> coverage;

            bool Ink(int x, int y) const
            {
                if (x < 0 || y < 0 || x >= width || y >= height) return false;
                return coverage[static_cast<std::size_t>(y * width + x)] != 0;
            }
        };

        // 캐시된다. 같은 문자열을 프레임마다 불러도 맵 조회 한 번 값이다.
        static const Mask& Bake(const std::string& utf8, int pixelHeight);

        // 이 덩어리가 차지할 프레임 픽셀 폭. 바이트 수로 우측 정렬이나 가운데
        // 정렬을 하면 한글 줄이 전부 엉뚱한 자리에 간다.
        static int MeasureWidth(const std::string& utf8, int pixelHeight);

        static void Draw(PixelBuffer& target, const TextSpan& span);
        static void DrawAll(PixelBuffer& target, const std::vector<TextSpan>& spans);

        // GDI 가 실제로 물려준 글꼴 이름. 폴백이 이것을 비교하게 된 지금, 이 이름이
        // 곧 HUD 가 진짜로 그려지는 글꼴이고 디버그 타이틀바가 이것을 보여준다.
        static const std::string& FaceName();

        // 이름을 요청했을 때 GDI 가 무엇으로 해석하는지. 캐시를 건드리지 않는다.
        //
        // 자체 테스트를 위해 공개했다. 폴백 전체가 경험적 주장 하나에 기대고 있다 —
        // **없는 글꼴에도 CreateFontW 가 성공하고, 그 대체가 되읽기에 드러난다**는 것.
        // 그만큼 무게를 지는 주장은 가정할 게 아니라 게임이 도는 기기에서 확인해야 한다.
        static std::string ResolveFace(const std::string& face, int pixelHeight);

        // 되읽은 이름이 "요청이 받아들여졌다"는 뜻인가. 대소문자를 구분하지 않고,
        // 한국어 Windows 에서는 글꼴이 한국어 이름으로 답할 수 있다.
        static bool FaceMatches(const std::string& requested, const std::string& actual);
    };
}
