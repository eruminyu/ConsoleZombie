#include "Hud.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace Zombie
{
    // HUD 글자의 em 높이. 설정이 0 이면 프레임 높이에서 계산한다.
    int HudTextHeight(int frameHeight, int configuredTextHeight)
    {
        if (configuredTextHeight > 0) return configuredTextHeight;
        return std::clamp(frameHeight / 18, 9, 16);
    }

    namespace
    {
        // 원하는 크기 이하에서 **실제로 들어가는** 가장 큰 높이. 가장자리를 넘는
        // 글자는 줄어드는 게 아니라 **그냥 잘리고**, 반쪽 단어는 작은 글자보다 나쁘다.
        // 좁은 창에서만 물린다.
        int FitHeight(const std::string& text, int preferred, int available)
        {
            int fitted = preferred;
            while (fitted > 7 && TextRaster::MeasureWidth(text, fitted) > available) --fitted;
            return fitted;
        }

        // 프레임에 맞춰 가로 가운데로 놓은 한 줄.
        TextSpan CentredLine(const std::string& text, int top, int height,
            Rgb colour, int frameWidth, int margin)
        {
            const int fitted = FitHeight(text, height, frameWidth - margin * 2);
            return { { (frameWidth - TextRaster::MeasureWidth(text, fitted)) / 2, top },
                text, colour, fitted };
        }

        // 두 번째 팔레트가 아니라 **흰색 쪽으로** 민다. 그러면 줄이 자기 뜻을
        // 유지한 채로 — 초록은 진행, 회색은 뒤로 — 포인터 아래에서 무게만 얻는다.
        Rgb Brighten(Rgb colour, float amount)
        {
            const auto lift = [amount](std::uint8_t channel)
            {
                return static_cast<std::uint8_t>(channel
                    + static_cast<int>((255 - channel) * amount));
            };
            return { lift(colour.r), lift(colour.g), lift(colour.b) };
        }

        std::string FormatDuration(float seconds)
        {
            const int whole = static_cast<int>(seconds < 0.0f ? 0.0f : seconds);
            const std::string tail = std::to_string(whole % 60);
            return std::to_string(whole / 60) + ":" + (tail.size() < 2 ? "0" + tail : tail);
        }
    }

    // HUD 오버레이 전체 — 체력, FPS, 열쇠, 탄약, 상태 로그, 뒤돌아보기 표시.
    //
    // 값이 들어가고 글자 덩어리가 나온다. 세계는 안 건드린다.
    std::vector<TextSpan> BuildHud(const HudState& state, const StatusLog& log,
        int frameWidth, int frameHeight)
    {
        const int height = HudTextHeight(frameHeight, state.configuredTextHeight);
        const int margin = std::max(2, frameWidth / 120);
        // 구운 마스크는 요청한 em 높이보다 조금 더 높다.
        const int line = height + 5;
        const auto width = [height](const std::string& text)
        {
            return TextRaster::MeasureWidth(text, height);
        };

        std::vector<TextSpan> overlay;

        // 좌상단 — 하트와 그 이름.
        std::string hearts;
        for (int index = 0; index < std::max(0, state.health); ++index) hearts += "\u2665";
        const std::string healthText = hearts + " 남은 체력";
        // 각각 폭 절반. 체력 표시와 FPS 가 한 줄을 나눠 쓴다.
        const int healthHeight = FitHeight(healthText, height, frameWidth / 2 - margin * 2);
        overlay.push_back({ { margin, 2 }, healthText, { 208, 72, 66 }, healthHeight });

        // 우상단 — FPS, 그리고 열쇠를 손에 넣으면 열쇠.
        const std::string fps = "FPS " + std::to_string(static_cast<int>(state.framesPerSecond + 0.5f));
        overlay.push_back({ { frameWidth - width(fps) - margin, 2 }, fps, { 132, 138, 148 }, height });
        if (state.hasKey)
        {
            const std::string key = "열쇠";
            overlay.push_back({ { frameWidth - width(key) - margin, 2 + line }, key,
                { 235, 180, 44 }, height });
        }

        // 우하단 — 남은 탄약. 막대가 숫자보다 빨리 읽히고, 숫자는 정확히 알아야
        // 할 때를 위해 있다.
        std::string rounds;
        for (int index = 0; index < std::max(0, state.ammo); ++index) rounds += "\u25AE";
        const std::string ammoText = "남은 탄환 " + rounds;
        const int ammoHeight = FitHeight(ammoText, height, frameWidth - margin * 2);
        overlay.push_back({ { frameWidth - TextRaster::MeasureWidth(ammoText, ammoHeight) - margin,
            frameHeight - line - 2 }, ammoText, { 217, 164, 59 }, ammoHeight });

        // 화면 가운데 조금 아래, 플레이어의 어깨 너머 — 무엇이 들리는가.
        const int logTop = frameHeight / 2 + frameHeight / 8;
        const std::vector<StatusLog::Line>& lines = log.Lines();
        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            const StatusLog::Line& entry = lines[index];
            // 마지막 1초 동안 잦아든다. 그래야 나가는 줄이 들어오는 줄로 오해받지 않는다.
            const float fade = std::clamp(entry.remaining, 0.0f, 1.0f);
            const Rgb ink = Rgb::Scale(Rgb{ 224, 218, 200 }, 0.45f + 0.55f * fade);
            const int lineHeight = FitHeight(entry.text, height, frameWidth - margin * 2);
            overlay.push_back({ { (frameWidth - TextRaster::MeasureWidth(entry.text, lineHeight)) / 2,
                logTop + static_cast<int>(index) * line }, entry.text, ink, lineHeight });
        }

        // 카메라가 어느 쪽을 보고 있는지 말한다. 양쪽이 똑같이 생긴 복도에서는
        // 뒤집힌 화면만으로는 알 수 없고, 사라진 총도 **왜 사라졌는지를 알아야만**
        // 의도된 것으로 읽힌다.
        if (state.lookingBehind)
        {
            const std::string marker = "뒤를 보는 중";
            const int markerHeight = FitHeight(marker, height, frameWidth - margin * 2);
            overlay.push_back({ { (frameWidth - TextRaster::MeasureWidth(marker, markerHeight)) / 2,
                2 + line }, marker, { 150, 158, 172 }, markerHeight });
        }

        return overlay;
    }

    // 개발자 탑다운의 숫자들.
    //
    // 한 자리 소수까지만 적는다. A* 비용은 타일당 1 근처의 값을 더한 것이라
    // 소수 둘째 자리는 읽는 사람에게 아무 말도 안 한다.
    std::vector<TextSpan> BuildDebugReadout(const DebugReadout& readout,
        int frameWidth, int frameHeight, int configuredTextHeight)
    {
        std::vector<TextSpan> overlay;
        // HUD 보다 한 단 작다. 여기 있는 것은 플레이어가 아니라 **개발자가 읽는
        // 숫자**이고, 지도가 이미 프레임의 가운데를 다 쓴다.
        const int height = std::max(8, HudTextHeight(frameHeight, configuredTextHeight) - 2);
        const int line = height + 3;
        const int margin = std::max(2, frameWidth / 120);
        int top = 2;

        const auto number = [](float value)
        {
            // std::to_string(float) 은 소수 여섯 자리를 낸다. 잘라 쓴다.
            const int tenths = static_cast<int>(value * 10.0f + (value < 0.0f ? -0.5f : 0.5f));
            return std::to_string(tenths / 10) + "." + std::to_string(std::abs(tenths % 10));
        };
        const auto push = [&overlay, &top, height, line, margin](const std::string& text, Rgb colour)
        {
            overlay.push_back({ { margin, top }, text, colour, height });
            top += line;
        };

        push("A* 비용", { 150, 158, 172 });
        if (readout.hasGuide)
        {
            // 안내 경로가 어디로 가는 것인지도 같이 적는다. 열쇠를 주운 순간
            // 목표가 바뀌는데, 선만 보면 그냥 경로가 튄 것처럼 보인다.
            push(std::string(readout.guideToExit ? "출구 " : "열쇠 ") + number(readout.guideCost)
                + " · " + std::to_string(readout.guideTiles) + "칸", { 48, 220, 226 });
        }
        else
        {
            push("안내 없음", { 110, 116, 126 });
        }
        // 생성 규칙이 이 맵에서 실제로 얼마를 냈는가. 하한이 40 인데 57 이 나왔다면,
        // 그 숫자 둘이 `minimum_key_path_cost` 가 하는 일 전부다.
        push("배치 " + number(readout.keyCost) + " / " + number(readout.keyCostMinimum),
            { 235, 180, 44 });

        if (readout.chasers > 0)
        {
            push("추적 " + std::to_string(readout.chasers) + "마리", { 231, 90, 76 });
            if (readout.hasNearestChase)
            {
                push("최근접 " + number(readout.nearestChaseCost)
                    + " · " + std::to_string(readout.nearestChaseTiles) + "칸", { 231, 90, 76 });
            }
        }
        else
        {
            push("추적 없음", { 110, 116, 126 });
        }

        top += line / 2;
        if (readout.hasCursor)
        {
            push("커서 " + std::to_string(readout.cursorTile.x) + ","
                + std::to_string(readout.cursorTile.y), { 150, 158, 172 });
            // 무한대는 숫자로 적지 않는다. "못 간다"와 "아주 멀다"는 다른 말이고,
            // 큰 수를 적으면 둘이 같아 보인다.
            push(" 이동 " + (std::isfinite(readout.cursorTraversal)
                ? number(readout.cursorTraversal) : std::string("-")), { 130, 200, 150 });
            push(" 음향 " + (std::isfinite(readout.cursorAcoustic)
                ? number(readout.cursorAcoustic) : std::string("-")), { 200, 160, 110 });
        }

        top += line / 2;
        // 히트맵의 스케일을 적는 것이 색을 설명하는 유일한 방법이다. 색만 보면
        // 짙은 빨강이 "멀다"인 것은 알겠지만 **얼마나** 인지는 알 수 없다.
        switch (readout.overlay)
        {
        case CostOverlay::Traversal:
            push("F4 이동 ≤" + number(readout.overlayScale), { 130, 200, 150 });
            break;
        case CostOverlay::Acoustic:
            push("F4 음향 ≤" + number(readout.overlayScale), { 200, 160, 110 });
            break;
        case CostOverlay::None:
            push("F4 히트맵", { 110, 116, 126 });
            break;
        }
        return overlay;
    }

    // 화면 카드 하나를 만든다 — 그릴 글자와 누를 수 있는 상자를 **한 번에**.
    //
    // Playing 은 빈 것을 돌려준다.
    ScreenLayout BuildScreenLayout(Screen screen, const RunSummary& run,
        int frameWidth, int frameHeight, int configuredTextHeight, Int2 pointer)
    {
        ScreenLayout layout;
        std::vector<TextSpan>& overlay = layout.spans;
        // Playing 은 자기 카드가 없다. 호출을 거절하는 대신 **빈 것을 돌려주면**
        // Run() 의 호출부 하나가 분기 없이 깔끔해진다.
        if (screen == Screen::Playing) return layout;

        // 주어진 em 에서 결과 카드가 얼마나 높은가 — 두 배 높이 제목, 간격,
        // 통계 넉 줄, 반 간격, 선택지 셋, 그리고 마지막 줄 자신의 상자.
        //
        // 여기 한 번만 적는다. 아래의 축소와 카드 안의 세로 가운데 맞추기가 **둘 다
        // 이 값에 대해 같은 말을 해야** 하기 때문이다.
        const auto resultStack = [](int em) { return em * 2 + (em + 5) * 19 / 2; };

        int height = HudTextHeight(frameHeight, configuredTextHeight);
        const int margin = std::max(2, frameWidth / 120);
        // 결과 카드는 어느 화면이 그리는 것보다 높다. 아래쪽이 프레임 밖으로
        // 떨어지게 두는 대신 **글자를 줄여서** 전체가 들어가게 한다.
        //
        // 출하 기본 렌더 천장인 200행에서 8픽셀이 넘쳤고, **잘려 나간 줄이 하필
        // 메뉴로 돌아가는 방법을 말하는 줄**이었다. 방금 죽은 플레이어에게 가장
        // 필요한 단 한 줄이다.
        if (screen == Screen::Dead || screen == Screen::Escaped)
        {
            while (height > 7 && resultStack(height) > frameHeight - margin * 2) --height;
        }
        const int line = height + 5;

        // 방 건너에서 읽히라고 만든 한 줄. 두 배 높이이고, 좁은 프레임이 실제로
        // 내주는 만큼을 받는다.
        const auto title = [&](const std::string& text, Rgb colour, int top)
        {
            overlay.push_back(CentredLine(text, top, height * 2, colour, frameWidth, margin));
        };

        // 플레이어가 키로든 포인터로든 고를 수 있는 줄. **글자와 클릭 상자를 여기서
        // 같이 만들고, 다른 어디서도 안 만든다.**
        //
        // 밝기는 여기서 정하지 **않는다.** 카드는 프레임에 맞추려고 아직 위로 옮겨질
        // 수 있고, 그 이동 전에 줄을 밝히면 **줄이 있게 될 자리가 아니라 있을
        // 예정이었던 자리**를 밝히게 된다. 어느 줄이 밝은지는 맨 마지막에,
        // 최종 상자를 보고 정한다.
        std::vector<std::size_t> choiceSpans;
        const auto choice = [&](const std::string& text, int top, Rgb colour, ScreenAction action)
        {
            const TextSpan span = CentredLine(text, top, height, colour, frameWidth, margin);
            const int textWidth = TextRaster::MeasureWidth(text, span.height);
            // 글자 둘레에 약간의 여유. 포인터는 터미널 셀 단위로 들어오고 셀 하나가
            // 프레임 픽셀 여러 개라, 잉크에 딱 붙인 상자는 마우스가 자꾸 미끄러지는
            // 상자가 된다.
            const int pad = std::max(2, span.height / 3);
            const Rect bounds{ span.pixel.x - pad, span.pixel.y - pad,
                textWidth + pad * 2, span.height + pad * 2 };
                        choiceSpans.push_back(overlay.size());
            layout.buttons.push_back({ bounds, action });
            overlay.push_back(span);
        };

        switch (screen)
        {
        case Screen::MainMenu:
        {
            // 이 뒤에서는 아무것도 돌고 있지 않다. 그래서 얼어붙은 그림을 피해
            // 눌려 앉는 대신, 타이틀 화면이 있어야 할 자리에 놓일 수 있다.
            title("CONSOLE ZOMBIE", { 226, 220, 206 }, frameHeight / 3);
            choice("Enter   시작", frameHeight / 2 + line, { 150, 205, 160 }, ScreenAction::Start);
            choice("Esc   종료", frameHeight / 2 + line * 2, { 150, 158, 172 }, ScreenAction::Quit);
            // 조작법은 도움말 화면이 아니라 여기 있어야 한다. **플레이어가 바쁘지
            // 않은 유일한 순간**이다.
            //
            // 긴 두 줄이 아니라 짧은 세 줄이다. 긴 줄은 짧은 줄보다 훨씬 먼저 안
            // 들어가기 시작하고, **안 들어가는 힌트는 없느니만 못하다** — 잘린 채로
            // 그려지고, 그건 작은 창이 아니라 게임의 결함으로 읽힌다.
            // 이것들조차 안 들어갈 만큼 좁은 프레임에서는 아예 뺀다.
            const auto hint = [&](const std::string& text, int top)
            {
                const TextSpan span =
                    CentredLine(text, top, height, Rgb{ 120, 126, 136 }, frameWidth, margin);
                if (span.pixel.x < margin) return;
                overlay.push_back(span);
            };
            hint("W A S D 이동 · Shift 달리기", frameHeight / 2 + line * 4);
            hint("Space 구르기 · 마우스 조준과 사격", frameHeight / 2 + line * 5);
            hint("E 문과 창문 · F 뒤돌아보기 · Esc 일시정지", frameHeight / 2 + line * 6);
            break;
        }
        case Screen::Generating:
            // 한 프레임, 생성이 도는 동안만. 재시도가 붙으면 1초에 가까워질 수
            // 있고, 그 정도면 가만히 있는 메뉴가 일하는 것이 아니라 멈춘 것으로 읽힌다.
            title("맵을 만드는 중", { 200, 196, 184 }, frameHeight / 2 - height);
            break;
        case Screen::Paused:
            title("일시정지", { 214, 210, 198 }, frameHeight / 2 - height * 2);
            choice("Enter   계속하기", frameHeight / 2 + line, { 150, 205, 160 }, ScreenAction::Resume);
            choice("Esc   메인 메뉴로", frameHeight / 2 + line * 2, { 150, 158, 172 }, ScreenAction::ToMenu);
            break;
        case Screen::Dead:
        case Screen::Escaped:
        {
            const bool escaped = screen == Screen::Escaped;
            // **카드 자신의 높이에서 역산해** 프레임 가운데에 놓는다.
            // 4분의 1 지점 같은 고정 비율은 창 크기 하나에서만 우연히 맞는 추측이다.
            const int top = std::max(margin, (frameHeight - resultStack(height)) / 2);
            title(escaped ? "탈출했다" : "죽었다",
                escaped ? Rgb{ 96, 210, 120 } : Rgb{ 214, 76, 68 }, top);

            int row = top + height * 2 + line;
            const auto stat = [&](const std::string& text, Rgb colour)
            {
                overlay.push_back(CentredLine(text, row, height, colour, frameWidth, margin));
                row += line;
            };
            stat("걸린 시간 " + FormatDuration(run.seconds), { 206, 200, 188 });
            stat("사용 탄약 " + std::to_string(run.shotsFired) + "발   명중 "
                + std::to_string(run.shotsHit), { 217, 164, 59 });
            stat("처치 " + std::to_string(run.kills), { 206, 200, 188 });
            // Base Seed 는 자기 줄을 갖는다. **적어둘 값어치가 있는 것이 그것**이고,
            // 열 자리 숫자 셋을 한 줄에 놓으면 어느 카드가 그리는 것보다 넓어진다.
            // 나머지 둘은 그 시드가 실제로 무슨 맵을 냈고 몇 번 다시 굴렸는지를
            // 말하는 각주다.
            stat("Seed " + std::to_string(run.baseSeed), { 132, 138, 148 });
            stat("맵 " + std::to_string(run.effectiveSeed) + "   재시도 "
                + std::to_string(run.seedAttempt), { 132, 138, 148 });

            row += line / 2;
            choice("R   같은 맵 다시", row, { 150, 205, 160 }, ScreenAction::Restart);
            row += line;
            choice("N   새 맵", row, { 150, 205, 160 }, ScreenAction::NewMap);
            row += line;
            choice("Esc   메인 메뉴로", row, { 150, 158, 172 }, ScreenAction::ToMenu);
            break;
        }
        default:
            break;
        }

        // 모든 카드에 걸리는 마지막 안전장치. **프레임 아래로 나가는 것이 있어서는
        // 안 된다** — 가장자리를 넘는 글자는 줄어드는 게 아니라 잘리고, 반만 그려진
        // 줄은 작은 창이 아니라 렌더링 결함으로 읽힌다.
        //
        // **카드가 버튼까지 통째로 움직인다.** 글자만 옮기고 클릭 상자를 두고 가면
        // 밝아진 줄과 눌리는 줄이 다른 자리에 놓이는데, BuildScreenLayout 이
        // 존재하는 이유가 정확히 그것을 불가능하게 만드는 것이다.
        int lowest = 0;
        int highest = frameHeight;
        for (const TextSpan& span : layout.spans)
        {
            // 구운 마스크는 요청한 em 보다 조금 높다 — 줄 간격이 이미 감안하고 있는
            // 그 5픽셀이다.
            lowest = std::max(lowest, span.pixel.y + span.height + 5);
            highest = std::min(highest, span.pixel.y);
        }
        const int overflow = lowest + margin - frameHeight;
        if (overflow > 0)
        {
            const int lift = std::min(overflow, std::max(0, highest - margin));
            for (TextSpan& span : layout.spans) span.pixel.y -= lift;
            for (ScreenButton& button : layout.buttons) button.bounds.y -= lift;
        }

        // **지금, 그리고 지금에서야** 포인터 아래 줄을 밝힌다. 잠시 뒤 클릭이
        // 검사받는 것과 **같은 상자**를 보고서다. 배치 도중에 정하고 나서 카드를
        // 옮겼더니 줄이 원래 있던 자리 기준으로 밝아졌고, 카드 올리기를 넣은 첫
        // 실행에서 자체 테스트가 그렇다고 말했다.
        //
        // **찾았다고 일찍 빠져나가지 않는다.** 상자 둘이 겹치는 일이 생기면 둘 다
        // 밝아지고 테스트가 그것을 보고하는데, 클릭은 조용히 앞의 것을 가져갈 것이다.
        // **화면에 드러나는 불일치가 고쳐지는 불일치다.**
        for (std::size_t index = 0; index < layout.buttons.size(); ++index)
        {
            if (!layout.buttons[index].bounds.Contains(pointer)) continue;
            TextSpan& lit = layout.spans[choiceSpans[index]];
            lit.color = Brighten(lit.color, 0.55f);
        }
        return layout;
    }
}
