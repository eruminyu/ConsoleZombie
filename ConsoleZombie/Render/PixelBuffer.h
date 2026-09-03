#pragma once

#include <Core/Types.h>
#include <algorithm>
#include <vector>

namespace Zombie
{
    // RGB 논리 프레임버퍼. 게임이 그리는 대상은 터미널 셀이 아니라 여기다.
    //
    // 셀 크기는 터미널 폰트가 정하고 게임이 못 바꾼다. 프레임은 게임 것이다.
    // UI 를 셀 문자에서 프레임 픽셀로 옮긴 이유가 그것이고, 되돌리면 QHD 에서
    // 다시 안 읽힌다.
    class PixelBuffer
    {
    public:
        PixelBuffer(int width, int height)
            : width(width), height(height), pixels(static_cast<std::size_t>(width * height))
        {
        }

        int Width() const { return width; }
        int Height() const { return height; }
        const std::vector<Rgb>& Pixels() const { return pixels; }

        // 전체를 한 색으로. 매 프레임 하늘과 바닥을 칠하기 전의 바탕이다.
        void Clear(Rgb color)
        {
            std::fill(pixels.begin(), pixels.end(), color);
        }

        // 범위 밖 좌표는 조용히 버린다. 스프라이트와 글자는 프레임 가장자리를
        // 넘길 수 있고, 그리는 쪽마다 잘라내기를 적게 하지 않으려는 것이다.
        void Set(int x, int y, Rgb color)
        {
            if (x >= 0 && y >= 0 && x < width && y < height)
            {
                pixels[static_cast<std::size_t>(y * width + x)] = color;
            }
        }

        // 범위 밖은 검정. Set() 과 짝을 맞춘 것이다.
        Rgb Get(int x, int y) const
        {
            if (x < 0 || y < 0 || x >= width || y >= height) return {};
            return pixels[static_cast<std::size_t>(y * width + x)];
        }

        void FillRect(int x, int y, int rectWidth, int rectHeight, Rgb color)
        {
            for (int py = y; py < y + rectHeight; ++py)
                for (int px = x; px < x + rectWidth; ++px)
                    Set(px, py, color);
        }

        // Bresenham 직선. 디버그 탑다운의 안내 경로가 유일한 사용처다.
        // 정수만 쓰므로 부동소수점 누적 오차로 선이 휘지 않는다.
        void Line(int x0, int y0, int x1, int y1, Rgb color)
        {
            const int dx = std::abs(x1 - x0);
            const int sx = x0 < x1 ? 1 : -1;
            const int dy = -std::abs(y1 - y0);
            const int sy = y0 < y1 ? 1 : -1;
            int error = dx + dy;
            while (true)
            {
                Set(x0, y0, color);
                if (x0 == x1 && y0 == y1) break;
                const int twice = error * 2;
                if (twice >= dy) { error += dy; x0 += sx; }
                if (twice <= dx) { error += dx; y0 += sy; }
            }
        }

    private:
        int width = 0;
        int height = 0;
        std::vector<Rgb> pixels;
    };
}
