#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Zombie
{
    // 플레이어에게 전하는 문구. 새 줄이 뒤에 붙는다.
    //
    // 규칙이 둘이고 둘 다 발소리 때문에 있다. 발소리는 초당 두 번 난다.
    // 줄은 스스로 늙어 사라지고, 같은 줄은 자기 쿨다운이 도는 동안 다시 안 올라온다.
    // 두 번째가 없으면 화면이 한 문장의 반복이 된다.
    //
    // Game 에서 떼어낸 이유는 여기 있는 것 중 세계를 필요로 하는 게 하나도 없기
    // 때문이다. 예전에는 침입 사건을 비우는 코드와 엉켜 있었는데, 그쪽은 세계가
    // 필요해서 남았다.
    class StatusLog
    {
    public:
        struct Line
        {
            std::string text;
            float remaining = 0.0f;
        };

        // 한 화면에 최대 이만큼. 넘치면 가장 오래된 것이 나간다.
        static constexpr std::size_t MaxLines = 3;

        // seconds 는 한 줄이 떠 있는 시간, cooldown 은 같은 문장이 다시 안 올라오는
        // 시간이다. 설정에서 한 번 받고 판 도중에는 안 바뀐다.
        void Configure(float seconds, float cooldown);

        void Push(const std::string& text);
        void Advance(float deltaTime);
        void Clear();
        const std::vector<Line>& Lines() const { return lines; }

    private:
        std::vector<Line> lines;
        // 문장 자체를 열쇠로 삼는다. 그래서 "문 두드리는 소리"와 "창문이 깨졌다"가
        // 서로를 밀어내지 않는다 — 종류마다 자기 시계를 갖는다.
        std::vector<std::pair<std::string, float>> cooldowns;
        float lineSeconds = 4.0f;
        float repeatCooldown = 3.0f;
    };
}
