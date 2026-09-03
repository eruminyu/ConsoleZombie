#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// 이 게임의 수학은 여기가 전부다. 벡터 라이브러리를 따로 두지 않은 이유는
// 실제로 쓰는 연산이 아래 몇 개뿐이기 때문이다.
namespace Zombie
{
    // 타일 좌표. 격자 위의 칸을 가리킨다.
    struct Int2
    {
        int x = 0;
        int y = 0;

        friend bool operator==(const Int2&, const Int2&) = default;
    };

    // 세계 좌표. 타일 하나가 1.0 이고, 플레이어와 좀비는 칸 사이에 설 수 있다.
    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;

        Vec2 operator+(const Vec2& rhs) const { return { x + rhs.x, y + rhs.y }; }
        Vec2 operator-(const Vec2& rhs) const { return { x - rhs.x, y - rhs.y }; }
        Vec2 operator*(float scale) const { return { x * scale, y * scale }; }
        Vec2& operator+=(const Vec2& rhs) { x += rhs.x; y += rhs.y; return *this; }
    };

    // 벡터의 길이.
    inline float Length(const Vec2& value)
    {
        return std::sqrt(value.x * value.x + value.y * value.y);
    }

    // 길이 1로 맞춘 같은 방향. 길이가 0에 가까우면 0 벡터를 돌려준다 —
    // 좀비가 목표 타일 위에 정확히 서면 실제로 그렇게 되고, 나누면 NaN 이 나온다.
    inline Vec2 Normalize(const Vec2& value)
    {
        const float length = Length(value);
        return length > 0.0001f ? value * (1.0f / length) : Vec2{};
    }

    // 세계 좌표가 속한 타일. 음수 좌표에서도 맞으려면 잘라내기가 아니라 floor 여야 한다.
    inline Int2 ToTile(const Vec2& value)
    {
        return { static_cast<int>(std::floor(value.x)), static_cast<int>(std::floor(value.y)) };
    }

    // 타일의 한가운데. 좀비가 A* 경로를 따라갈 때 겨누는 지점이 이것이다.
    inline Vec2 TileCenter(const Int2& value)
    {
        return { static_cast<float>(value.x) + 0.5f, static_cast<float>(value.y) + 0.5f };
    }

    // 타일 단위 사각형. 방·리프·복도 Band·클릭 상자가 전부 이것이다.
    struct Rect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        // Right() 와 Bottom() 은 경계 바깥 첫 칸이다. 포함이 아니라 배타적이므로
        // Contains() 의 비교가 < 이지 <= 가 아니다.
        int Right() const { return x + width; }
        int Bottom() const { return y + height; }
        Int2 Center() const { return { x + width / 2, y + height / 2 }; }
        bool Contains(Int2 point) const
        {
            return point.x >= x && point.x < Right() && point.y >= y && point.y < Bottom();
        }

        friend bool operator==(const Rect&, const Rect&) = default;
    };

    // 화면에 나가는 색. 프레임버퍼가 이 값을 그대로 들고 있다.
    struct Rgb
    {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;

        // 밝기 조절. 거리에 따른 음영과 카드 뒤 화면 어둡게가 전부 이걸 쓴다.
        // clamp 가 없으면 1.0 보다 큰 배율에서 채널이 넘쳐 어두운 색으로 감긴다.
        static Rgb Scale(Rgb color, float factor)
        {
            const auto channel = [factor](std::uint8_t value)
            {
                return static_cast<std::uint8_t>(std::clamp(value * factor, 0.0f, 255.0f));
            };
            return { channel(color.r), channel(color.g), channel(color.b) };
        }

        friend bool operator==(const Rgb&, const Rgb&) = default;
    };
}
