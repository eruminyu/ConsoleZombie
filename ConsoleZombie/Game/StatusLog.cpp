#include "StatusLog.h"

#include <algorithm>

namespace Zombie
{
    // 설정에서 두 시간을 받는다. 판 도중에 안 바뀐다.
    void StatusLog::Configure(float seconds, float cooldown)
    {
        lineSeconds = seconds;
        repeatCooldown = cooldown;
    }

    // 한 줄 올린다. 같은 문장이 쿨다운 중이면 조용히 버린다.
    void StatusLog::Push(const std::string& text)
    {
        for (const auto& entry : cooldowns)
        {
            if (entry.first == text && entry.second > 0.0f) return;
        }
        cooldowns.emplace_back(text, repeatCooldown);
        lines.push_back({ text, lineSeconds });
        if (lines.size() > MaxLines) lines.erase(lines.begin());
    }

    // 시간을 흘린다. 수명이 다한 줄과 다 돈 쿨다운을 같이 걷어낸다.
    //
    // 일시정지가 이 함수를 안 부른다. 세계가 멈췄는데 로그만 늙으면 아직 못 읽은
    // 줄이 사라진다.
    void StatusLog::Advance(float deltaTime)
    {
        for (Line& line : lines) line.remaining -= deltaTime;
        lines.erase(std::remove_if(lines.begin(), lines.end(),
            [](const Line& line) { return line.remaining <= 0.0f; }), lines.end());
        for (auto& entry : cooldowns) entry.second -= deltaTime;
        cooldowns.erase(std::remove_if(cooldowns.begin(), cooldowns.end(),
            [](const auto& entry) { return entry.second <= 0.0f; }), cooldowns.end());
    }

    // 판이 새로 시작할 때 비운다. 쿨다운까지 같이 비워야 새 판 첫 줄이 안 씹힌다.
    void StatusLog::Clear()
    {
        lines.clear();
        cooldowns.clear();
    }
}
