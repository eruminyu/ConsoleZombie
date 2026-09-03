#include "TextRaster.h"

#include <Windows.h>
#include <algorithm>
#include <unordered_map>

namespace Zombie
{
    namespace
    {
        // 후보 글꼴 하나. 굵기를 후보마다 따로 들고 있는 이유는, 외곽선 글꼴과
        // 비트맵이 박힌 글꼴에서 정답이 다르기 때문이다.
        struct FaceChoice
        {
            const wchar_t* face;
            int weight;
            // 한국어 Windows 의 GetTextFaceW 가 대신 돌려줄 수 있는 이름.
            // **이 칸이 없으면 아래 검사가 설치돼 있는 글꼴을 거절한다.**
            const wchar_t* korean;
        };

        // 굵기가 글꼴마다 다른 이유는 NONANTIALIASED_QUALITY 아래에서 두 외곽선
        // 글꼴이 다르게 굴기 때문이다. 이 품질은 손으로 맞춘 비트맵을 읽는 대신
        // 곡선을 임계값으로 잘라낸다. 아래 근거는 전부 em 11 에서 실측했다 —
        // 이 천장에서 HUD 가 실제로 잡는 크기다.
        //
        // 맑은 고딕은 normal 이다. 힌팅이 잘리는 동안에도 획을 붙들어 주고,
        // semibold 는 더 나빴다. Draw() 가 잉크 픽셀마다 8방향으로 테두리를 두르는데,
        // 이 크기에서 한글 자음 속 빈 공간이 1~2픽셀이다. 획이 굵어지면 테두리가
        // 그 공간을 메워버리고 음절이 덩어리가 된다.
        //
        // Noto Sans KR 은 반대라서 semibold 로 요청한다. normal 이 1픽셀 획으로
        // 잘려 내려가고 그 획이 군데군데 끊겨서 음절이 다른 음절로 읽힌다.
        // **600 만이 도움이 된다는 점에 주의.** 600 이라야 GDI 가 굵게를 합성하고,
        // 그러면 자간까지 같이 넓어진다. 700 이상은 다시 regular 에 가까운 것으로
        // 해석되고, 배포된 SemiBold 패밀리를 이름으로 직접 부르면 자간이 regular
        // 그대로라 얻는 것이 없다.
        //
        // 비트맵 글꼴들은 normal 이다. 글리프가 애초에 1픽셀 획으로 그려져 있고,
        // 굵기를 올리면 GDI 가 굵게를 합성해서 그 글꼴이 잘해놓은 것을 뭉갠다.
        constexpr FaceChoice FaceCandidates[] =
        {
            { L"Malgun Gothic", FW_NORMAL, L"맑은 고딕" },
            { L"Noto Sans KR", FW_SEMIBOLD, nullptr },
            { L"DotumChe", FW_NORMAL, L"돋움체" },
            { L"GulimChe", FW_NORMAL, L"굴림체" },
            { L"Dotum", FW_NORMAL, L"돋움" },
            { L"Gulim", FW_NORMAL, L"굴림" },
            // 목록을 닫는 자리. 한글 글꼴이 하나도 없는 기기에서도 ASCII 부분은
            // 그려지게 한다. 뒤에 떨어질 곳이 없으므로 GDI 가 무엇으로 해석하든
            // 그대로 받는다.
            { L"Consolas", FW_NORMAL, nullptr }
        };

        // 서수 비교, 대소문자 무시, 로케일 없음. 비교하는 이름은 이 파일의 표와
        // GDI 에서 오지 사용자에게서 오지 않는다.
        bool SameFace(const wchar_t* left, const wchar_t* right)
        {
            if (!left || !right) return false;
            return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
        }

        // UTF-8 → UTF-16. Windows 텍스트 API 가 전부 와이드 문자다.
        std::wstring Widen(const std::string& utf8)
        {
            const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                static_cast<int>(utf8.size()), nullptr, 0);
            if (count <= 0) return {};
            std::wstring wide(static_cast<std::size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                wide.data(), count);
            return wide;
        }

        // UTF-16 → UTF-8. 되읽은 글꼴 이름을 밖으로 내보낼 때 쓴다.
        std::string Narrow(const std::wstring& wide)
        {
            const int count = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return {};
            std::string text(static_cast<std::size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                text.data(), count, nullptr, nullptr);
            return text;
        }

        // 메모리 DC 하나와 크기별 글꼴 하나를 프로세스가 사는 동안 들고 있는다.
        //
        // 문자열마다 만들면 HUD 하나가 초당 GDI 객체 수백 개가 된다. 마스크는 어차피
        // 캐시되므로 이 비용은 새 크기가 나올 때만 치른다.
        class FontCache
        {
        public:
            static FontCache& Instance()
            {
                static FontCache cache;
                return cache;
            }

            HDC Device() const { return device; }

            // 이 em 높이의 글꼴을 공용 DC 에 선택하고 돌려준다. GDI 가 안 주면
            // null 이고, 그 경우 모든 마스크가 비어 나온다.
            HFONT Select(int pixelHeight)
            {
                if (!device) return nullptr;
                if (const auto found = fonts.find(pixelHeight); found != fonts.end())
                {
                    SelectObject(device, found->second);
                    return found->second;
                }

                for (std::size_t index = 0; index < std::size(FaceCandidates); ++index)
                {
                    const FaceChoice& choice = FaceCandidates[index];
                    HFONT candidate = CreateFontW(-pixelHeight, 0, 0, 0, choice.weight,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, choice.face);
                    if (!candidate) continue;
                    const HGDIOBJ previous = SelectObject(device, candidate);

                    // **CreateFontW 는 없는 글꼴에도 성공한다.** 논리 글꼴을 만들 뿐이고,
                    // GDI 의 매퍼가 선택 시점에 다른 것을 고른다. 그래서 반환값은 아무것도
                    // 증명하지 않고, 실제로 무엇이 물렸는지 아는 길은 이 되읽기뿐이다.
                    //
                    // 2026-08-29 까지 아무도 이것을 비교하지 않았다. 그 말은 **첫 항목
                    // 아래의 목록에 도달할 수 없었다**는 뜻이고, 맑은 고딕이 없는 기기는
                    // GDI 가 고른 아무 글꼴로 HUD 를 그렸다 — **다른 글꼴에 맞춰 실측한
                    // 굵기로.** 여기 굵기는 글꼴마다 다르고 실측된 값이다. Noto Sans KR 에
                    // 맑은 고딕용 굵기를 주는 것이 정확히 1픽셀 획으로 잘려 깨지는 경우다.
                    wchar_t actual[LF_FACESIZE]{};
                    GetTextFaceW(device, LF_FACESIZE, actual);

                    // 마지막 후보는 무엇으로 해석되든 받는다. 뒤에 아무것도 없고,
                    // 대체된 글꼴로 ASCII 부분이라도 그리는 것이 아무것도 안 그리는
                    // 것보다 낫다.
                    const bool lastResort = index + 1 == std::size(FaceCandidates);
                    if (!lastResort && !SameFace(actual, choice.face)
                        && !SameFace(actual, choice.korean))
                    {
                        // 이건 아니다. 선택된 채로 지우면 DC 가 해제된 메모리를 든
                        // 핸들을 갖게 되므로, 옛 글꼴을 먼저 되돌려 놓는다.
                        SelectObject(device, previous);
                        DeleteObject(candidate);
                        continue;
                    }

                    faceName = Narrow(actual);
                    fonts.emplace(pixelHeight, candidate);
                    return candidate;
                }
                return nullptr;
            }

            const std::string& Face() const { return faceName; }

        private:
            FontCache()
            {
                HDC screen = GetDC(nullptr);
                device = CreateCompatibleDC(screen);
                if (screen) ReleaseDC(nullptr, screen);
                if (device)
                {
                    SetBkMode(device, TRANSPARENT);
                    SetTextColor(device, RGB(255, 255, 255));
                    SetTextAlign(device, TA_LEFT | TA_TOP);
                }
            }

            HDC device = nullptr;
            std::unordered_map<int, HFONT> fonts;
            std::string faceName;
        };

        // 문자열 하나를 켜짐/꺼짐 픽셀 마스크로 굽는다.
        TextRaster::Mask BakeMask(const std::string& utf8, int pixelHeight)
        {
            TextRaster::Mask mask;
            const std::wstring wide = Widen(utf8);
            if (wide.empty()) return mask;

            FontCache& cache = FontCache::Instance();
            if (!cache.Select(pixelHeight)) return mask;
            HDC device = cache.Device();

            SIZE extent{};
            if (!GetTextExtentPoint32W(device, wide.c_str(),
                static_cast<int>(wide.size()), &extent))
            {
                return mask;
            }
            const int width = std::clamp(static_cast<int>(extent.cx), 1, 4096);
            const int height = std::clamp(static_cast<int>(extent.cy), 1, 512);

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            // 높이가 음수면 위에서 아래로 가는 비트맵이 된다. 프레임의 행 순서와 같다.
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(device, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bitmap || !bits) return mask;

            HGDIOBJ previous = SelectObject(device, bitmap);
            RECT all{ 0, 0, width, height };
            FillRect(device, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            TextOutW(device, 0, 0, wide.c_str(), static_cast<int>(wide.size()));
            // **GDI 는 그리기를 모아서 처리한다.** 이 줄이 없으면 GDI 가 아직 쓰지도
            // 않은 비트를 읽는다. 마스크가 빈 채로 나오고 실패한 티가 안 난다.
            GdiFlush();

            mask.width = width;
            mask.height = height;
            mask.coverage.assign(static_cast<std::size_t>(width) * height, 0);
            const auto* pixels = static_cast<const std::uint32_t*>(bits);
            for (std::size_t index = 0; index < mask.coverage.size(); ++index)
            {
                mask.coverage[index] = (pixels[index] & 0xFFu) > 127u
                    ? static_cast<std::uint8_t>(255) : static_cast<std::uint8_t>(0);
            }

            SelectObject(device, previous);
            DeleteObject(bitmap);
            return mask;
        }

        std::unordered_map<std::string, TextRaster::Mask>& MaskCache()
        {
            static std::unordered_map<std::string, TextRaster::Mask> cache;
            return cache;
        }
    }

    // 캐시된 마스크. 없으면 굽는다.
    const TextRaster::Mask& TextRaster::Bake(const std::string& utf8, int pixelHeight)
    {
        static const Mask empty;
        if (utf8.empty()) return empty;
        const int height = std::clamp(pixelHeight, 5, 96);

        auto& cache = MaskCache();
        const std::string key = std::to_string(height) + '\x01' + utf8;
        if (const auto found = cache.find(key); found != cache.end()) return found->second;
        // FPS 표시가 초마다 새 문자열을 만들어내므로 긴 세션에서 한없이 자란다.
        // 어차피 캐시다. 나이를 추적하는 것보다 통째로 버리는 것이 싸고,
        // 다시 굽는 값은 GDI 호출 한 번이다.
        if (cache.size() > 512) cache.clear();
        return cache.emplace(key, BakeMask(utf8, height)).first->second;
    }

    // 이 문자열이 차지할 프레임 픽셀 폭.
    int TextRaster::MeasureWidth(const std::string& utf8, int pixelHeight)
    {
        return Bake(utf8, pixelHeight).width;
    }

    // 프레임에 글자 한 덩어리를 그린다. 외곽선 한 번, 잉크 한 번, **두 패스**다.
    void TextRaster::Draw(PixelBuffer& target, const TextSpan& span)
    {
        const Mask& mask = Bake(span.text, span.height);
        if (mask.coverage.empty()) return;

        // 글리프마다 어두운 테두리를 두른다. 셀 오버레이 시절에는 글자 뒤 셀 전체를
        // 어둡게 할 수 있었지만, 픽셀 글리프에는 셀이 없다. 이게 없으면 플레이어가
        // 밝은 벽을 보는 순간 글자가 사라진다.
        constexpr Rgb outline{ 8, 8, 10 };
        for (int y = 0; y < mask.height; ++y)
        {
            for (int x = 0; x < mask.width; ++x)
            {
                if (!mask.Ink(x, y)) continue;
                for (int offsetY = -1; offsetY <= 1; ++offsetY)
                    for (int offsetX = -1; offsetX <= 1; ++offsetX)
                        if (!mask.Ink(x + offsetX, y + offsetY))
                            target.Set(span.pixel.x + x + offsetX, span.pixel.y + y + offsetY, outline);
            }
        }
        // 잉크는 마지막에 자기 패스로. 한 패스로 하면 뒤 글자의 외곽선이 앞 글자의
        // 획을 파먹는다.
        for (int y = 0; y < mask.height; ++y)
            for (int x = 0; x < mask.width; ++x)
                if (mask.Ink(x, y)) target.Set(span.pixel.x + x, span.pixel.y + y, span.color);
    }

    void TextRaster::DrawAll(PixelBuffer& target, const std::vector<TextSpan>& spans)
    {
        for (const TextSpan& span : spans) Draw(target, span);
    }

    // 되읽은 이름이 요청한 글꼴을 뜻하는가. 한국어 이름도 같은 것으로 친다.
    bool TextRaster::FaceMatches(const std::string& requested, const std::string& actual)
    {
        const std::wstring wantWide = Widen(requested);
        const std::wstring gotWide = Widen(actual);
        if (SameFace(wantWide.c_str(), gotWide.c_str())) return true;
        // 어떤 글꼴이 한국어 이름으로 답하는지를 아는 곳은 위의 표뿐이다.
        // 그래서 답을 추측이 아니라 표에서 가져온다.
        for (const FaceChoice& choice : FaceCandidates)
        {
            if (!SameFace(wantWide.c_str(), choice.face)) continue;
            return SameFace(gotWide.c_str(), choice.korean);
        }
        return false;
    }

    // 이름을 요청하면 GDI 가 무엇을 물리는지. 자체 테스트가 폴백의 전제를 확인하는 데 쓴다.
    std::string TextRaster::ResolveFace(const std::string& face, int pixelHeight)
    {
        // DC 도 글꼴도 자기 것을 쓴다. 일부러다 — HUD 가 그리고 있는 캐시를
        // 건드리면 안 되고, 뭔가 선택된 채로 남기고 나가서도 안 된다.
        HDC screen = GetDC(nullptr);
        HDC device = CreateCompatibleDC(screen);
        if (screen) ReleaseDC(nullptr, screen);
        if (!device) return {};

        const std::wstring wide = Widen(face);
        std::string resolved;
        if (HFONT font = CreateFontW(-std::max(1, pixelHeight), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, wide.c_str()))
        {
            const HGDIOBJ previous = SelectObject(device, font);
            wchar_t actual[LF_FACESIZE]{};
            GetTextFaceW(device, LF_FACESIZE, actual);
            resolved = Narrow(actual);
            SelectObject(device, previous);
            DeleteObject(font);
        }
        DeleteDC(device);
        return resolved;
    }

    // HUD 가 실제로 그려지고 있는 글꼴 이름. --self-test 가 이것을 찍는다.
    const std::string& TextRaster::FaceName()
    {
        return FontCache::Instance().Face();
    }
}
