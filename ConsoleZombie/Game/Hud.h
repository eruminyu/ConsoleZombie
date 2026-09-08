#pragma once

#include <Game/StatusLog.h>
#include <Platform/TextRaster.h>
#include <cstdint>
#include <vector>

namespace Zombie
{
    // HUD 가 필요로 하는 것 전부, 그리고 그 외에는 아무것도.
    //
    // 배치가 예전에는 Game 안에 있었고, 그러면 그것을 읽으려면 Game 이 또 무엇을
    // 들고 있는지까지 알아야 했다. 하는 일은 숫자 몇 개를 글자 덩어리로 바꾸는
    // 것뿐이고 **세계를 하나도 안 건드린다** — 격자도, 좀비도, 설정값 하나를 넘는
    // 밸런스도. 그 성질이 유지되도록, 그리고 게임을 통째로 세우지 않고도 결과를
    // 검사할 수 있도록 떼어 놓았다.
    struct HudState
    {
        int health = 0;
        int ammo = 0;
        bool hasKey = false;
        float framesPerSecond = 0.0f;
        bool lookingBehind = false;
        // 0 이면 프레임에서 크기를 계산한다. 그 외에는 적힌 대로 따른다.
        int configuredTextHeight = 0;
    };

    // 탑다운 디버그 화면이 켤 수 있는 비용 히트맵.
    //
    // 둘은 **다른 비용**이고 그것이 요점이다. 이동 비용은 갈 수 있는 곳만 재고
    // 벽은 아예 못 들어간다. 음향 비용은 벽에도 들어가되 비싸게 친다. 화면에서
    // 둘을 번갈아 보면 "벽이 소리를 먹는다"가 문장이 아니라 그림이 된다.
    enum class CostOverlay
    {
        None,
        Traversal,
        Acoustic
    };

    // 개발자 모드가 화면에 적는 숫자 전부.
    //
    // `HudState` 와 같은 모양이고 이유도 같다 — 값이 들어가고 글자 덩어리가 나오며
    // 세계는 안 건드린다. A* 를 부르는 것도, 격자를 읽는 것도 여기가 아니라 게임 쪽이다.
    struct DebugReadout
    {
        // 안내 경로 — 플레이어에서 열쇠, 열쇠를 얻었으면 탈출구까지.
        bool hasGuide = false;
        float guideCost = 0.0f;
        int guideTiles = 0;
        bool guideToExit = false;
        // 이 맵의 출구 → 열쇠 A* 비용과, 생성기가 요구한 하한. 규칙이 지켜졌다는
        // 것을 맵마다 눈으로 확인하는 줄이다.
        float keyCost = 0.0f;
        float keyCostMinimum = 0.0f;
        // 지금 쫓고 있는 좀비 수와 그중 가장 싼 경로.
        int chasers = 0;
        bool hasNearestChase = false;
        float nearestChaseCost = 0.0f;
        int nearestChaseTiles = 0;
        // 포인터가 가리키는 타일과 거기까지의 두 비용. 격자 밖이면 hasCursor 가 거짓이다.
        bool hasCursor = false;
        Int2 cursorTile{};
        float cursorTraversal = 0.0f;
        float cursorAcoustic = 0.0f;
        CostOverlay overlay = CostOverlay::None;
        float overlayScale = 0.0f;
    };

    // 개발자 탑다운 좌상단의 숫자들. 프레임 픽셀 좌표로 나온다.
    //
    // 지도는 프레임 가운데에 앉고 격자가 정사각형이라, 출하 해상도에서 왼쪽에
    // 여든 픽셀 남짓이 빈다. 숫자는 거기 산다.
    std::vector<TextSpan> BuildDebugReadout(const DebugReadout& readout,
        int frameWidth, int frameHeight, int configuredTextHeight);

    // 게임이 어느 화면에 있는가. 3.9 의 흐름을 값 하나로.
    //
    // Generating 이 단순한 틈이 아니라 화면인 이유는, 생성이 실제로 눈에 보이는
    // 시간을 쓰기 때문이다 — 자체 테스트에서 64개 시드에 27초쯤 걸린다.
    // 그렇다고 말하는 프레임이 얼어붙은 메뉴보다 낫다.
    enum class Screen
    {
        MainMenu,
        Generating,
        Playing,
        Paused,
        Dead,
        Escaped
    };

    // 한 판이 어땠는가. 플레이 중에 채워지고 결과 화면이 읽는다.
    //
    // shotsHit 과 kills 는 지금 언제나 같다. 총알 하나가 좀비 하나이기 때문이다.
    // 그래도 따로 센다 — **좀비가 한 방을 견디는 날 둘이 갈라지고**, 카운터가
    // 하나면 그날 없는 것이 아니라 조용히 틀린 것이 된다.
    struct RunSummary
    {
        float seconds = 0.0f;
        int shotsFired = 0;
        int shotsHit = 0;
        int kills = 0;
        // 3.9 는 결과 화면에 셋 다 요구한다. Base Seed 는 `--seed` 가 받은 것,
        // Effective Seed 는 실제로 맵을 만든 것, AttemptIndex 는 거기까지 몇 번
        // 다시 굴렸는지다. **맵을 재현하려면 Base Seed 가 필요하고, 맵을 설명하려면
        // 나머지 둘이 필요하다.**
        std::uint32_t baseSeed = 0;
        std::uint32_t effectiveSeed = 0;
        int seedAttempt = 0;
    };

    // HUD 글자의 em 높이(프레임 픽셀).
    //
    // 상수가 아니라 **프레임에 대한 비율**이다. 그래야 플레이어가 어떤 창을 열었든
    // HUD 가 화면에서 같은 크기로 보인다. 9는 돋움체의 한글 비트맵이 해상되기를
    // 그만두는 지점이고, 8에서는 음절이 덩어리로 뭉개진다. 그래서 작은 창일수록
    // 글자가 비례적으로 커지는데, 그게 실패해야 할 올바른 방향이다.
    int HudTextHeight(int frameHeight, int configuredTextHeight);

    std::vector<TextSpan> BuildHud(const HudState& state, const StatusLog& log,
        int frameWidth, int frameHeight);

    // 화면의 한 줄을 골랐을 때 무슨 일이 일어나는가.
    //
    // 키보드와 포인터가 둘 다 여기로 모인다. 그래서 화면 하나가 내주는 선택지 목록이,
    // 플레이어가 어느 손으로 뻗든 **정확히 하나**다.
    enum class ScreenAction
    {
        None,
        Start,
        Quit,
        Resume,
        ToMenu,
        Restart,
        NewMap
    };

    // 고를 수 있는 줄 하나 — 프레임 어디에 있고 무엇을 하는가.
    struct ScreenButton
    {
        Rect bounds{};
        ScreenAction action = ScreenAction::None;
    };

    // 카드 전체 — 그릴 것과 누를 수 있는 것.
    //
    // **둘이 한 패스에서 나오는 것은 일부러다.** 좀비 스프라이트가 그리기와 명중
    // 판정이 각자의 상수 사본에서 같은 상자를 계산하고 있었고, 그것이 어긋나면서
    // **보이는데 못 쏘는 좀비**가 나왔다. 글자가 한 곳에 있고 클릭 상자가 다른 곳에
    // 있는 메뉴는 모자만 바꿔 쓴 같은 버그다.
    struct ScreenLayout
    {
        std::vector<TextSpan> spans;
        std::vector<ScreenButton> buttons;
    };

    // 메뉴, 일시정지 카드, 결과 카드 둘을 프레임 픽셀 위에 배치한다.
    //
    // BuildHud 와 같은 모양이고 이유도 같다 — 값이 들어가고 글자 덩어리가 나오며
    // 세계를 안 건드린다. **Playing 은 아무것도 안 돌려준다.** 그래서 Run() 이
    // 분기로 감싸는 대신 무조건 부를 수 있다.
    //
    // pointer 는 마우스의 프레임 픽셀 위치다. 그 아래 줄을 더 밝게 그리므로,
    // 밝아진 줄과 클릭 상자가 어느 줄인지에 대해 다른 말을 할 수 없다.
    // "포인터 없음"을 뜻하려면 프레임 밖 좌표를 주면 된다.
    ScreenLayout BuildScreenLayout(Screen screen, const RunSummary& run,
        int frameWidth, int frameHeight, int configuredTextHeight, Int2 pointer);
}
