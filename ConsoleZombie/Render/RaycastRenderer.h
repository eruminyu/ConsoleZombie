#pragma once

#include <Render/PixelBuffer.h>
#include <World/Grid.h>
#include <vector>

namespace Zombie
{
    struct SelfTestAccess;

    // 렌더러가 좀비에 대해 아는 것 전부. A* 경로도 타이머도 여기 없다.
    struct RenderActor
    {
        Vec2 position{};
        bool alive = true;
        bool alert = false;
        // 몸이 향한 방향(라디안). 실루엣이 같이 돌아서, 플레이어를 못 본 좀비와
        // 본 좀비가 다르게 보인다.
        float facing = 0.0f;
        // 보폭 위상(1이 한 주기). 시간이 아니라 **걸은 거리**로 나아간다.
        // 그래서 문에 막힌 좀비가 제자리걸음을 하지 않고, 더 중요하게는
        // **걸음걸이가 시계가 아니라 세계의 함수**가 된다.
        //
        // 렌더 다이제스트는 렌더러가 격자·상태·배우의 순수 함수로 남아 있는
        // 동안에만 오라클이다.
        float stridePhase = 0.0f;
        // 0 이 서 있는 것, 1 이 완전히 쓰러진 것. **죽은 배우도 건너뛰지 않고
        // 그린다** — 맵 전체에 탄약이 몇 발뿐이니, 한 발을 어디에 썼는지는
        // 바닥에 남겨둘 값어치가 있다.
        float deathProgress = 0.0f;
    };

    // 투영된 좀비 하나 — 화면 어디에 떨어지고 어떤 모양인가.
    //
    // **그리기와 명중 판정이 이것을 같은 함수에서 받는다.** 6.x 는 총알이 그려진
    // 마스크에 대고 판정되기를 요구하는데, 예전에는 양쪽이 같은 상수 셋의 각자
    // 사본에서 상자를 계산했다. 마스크가 인자를 받게 되면서 그 중복은
    // **보이는데 못 쏘는 좀비**라는 버그가 됐을 것이다.
    struct ZombieSprite
    {
        // 배우가 카메라 뒤에 있거나 시야 밖이면 false.
        //
        // 그리기와 명중 판정이 **이 플래그 하나**에서 포기한다. 각자 자기 시야
        // 컷오프를 들고 있던 것이 둘이 어긋난 경로다 — 두 컷오프 사이에 있는
        // 좀비가 그려지는데 쏠 수는 없었다.
        bool visible = false;
        float left = 0.0f;
        float top = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
        float distance = 0.0f;
        // 여기부터는 마스크에 넘어가는 인자다.
        float stride = 0.0f;
        // 0 이 옆모습, 1 이 정면이나 정후면.
        float profile = 1.0f;
        // 몸이 향한 방향과 보는 사람 쪽 방향 사이 각의 코사인.
        // 양수면 좀비가 카메라를 향하고 있다는 뜻이고, **그때만 눈이 이쪽 면에 있다.**
        float towards = 1.0f;
        float collapse = 0.0f;
    };

    // 한 프레임을 그리는 데 필요한 상태 전부. 렌더러는 Game 을 모른다.
    struct RenderState
    {
        Vec2 playerPosition{};
        float playerAngle = 0.0f;
        Int2 aimPixel{ 60, 40 };
        int health = 3;
        int ammo = 3;
        bool hasKey = false;
        bool showDebugMap = false;
        float rollReady = 1.0f;
        // 0..1. 달리면 줄고, 구르기가 고정량을 뜯어간다.
        float stamina = 1.0f;
        // HUD 글자의 em 높이(프레임 픽셀). 게이지가 체력 표시 아래에 앉는데,
        // 그 표시가 더 이상 고정 2픽셀이 아니다.
        int uiTextHeight = 12;
        // 뒤돌아보기 키는 플레이어를 안 돌리고 카메라만 돌린다.
        bool lookingBehind = false;
        // 무기가 얼마나 아래로 밀렸는가, 프레임 높이에 대한 비율로. 쉬면 0 이다.
        // 픽셀이 아니라 비율인 이유는, 무기 자신의 크기가 그렇듯 반동도 어느 창에서나
        // 같은 몸짓이어야 하기 때문이다.
        float recoil = 0.0f;
        // 위의 aimPixel 에 **이미 접혀 들어간** 흔들림과 반동을, 프레임 픽셀로.
        // 무기가 같은 움직임을 탈 수 있게 하려고 들고 온다.
        //
        // **다시 계산하지 않고 들고 오는 것이 요점이다.** 7.x 는 숨은 산탄을
        // 금지한다 — 총알은 그려진 조준선이 있는 자리로 정확히 간다. 그것이
        // 성립하는 이유는 aimPixel 이 누가 읽기 전에 **한 번** 옮겨지고, 그리기와
        // 명중 판정이 둘 다 그 하나의 값을 읽기 때문이다. 렌더러에서 오프셋을
        // 두 번째로 더하는 것이 곧 둘이 어긋나는 길이다.
        Vec2 aimOffset{};
        // 가장자리가 얼마나 붉은가. 쉬면 0. 맞으면 서고 서서히 잦아든다.
        float hitFlash = 0.0f;

        // 카메라가 향한 방향.
        //
        // **그려지는 것은 전부 이것을 지나고, 플레이어가 행동할 수 있는 것은 전부
        // playerAngle 을 지난다.** 둘을 떼어 놓는 것이 이 메커닉 전체다 —
        // 뒤돌아보기는 거기 무엇이 있는지 보여줄 뿐 쏘게 해주지 않는다.
        // 둘을 만나게 하면 F 가 공짜 즉시 회전이 되고, 긴장이 기대고 있는 느린
        // A/D 회전이 의미를 잃는다.
        float ViewAngle() const
        {
            constexpr float half = 3.14159265f;
            return lookingBehind ? playerAngle + half : playerAngle;
        }
    };

    // 탑다운 지도가 프레임 어디에 어떤 배율로 앉는가.
    //
    // **그리는 쪽과 읽는 쪽이 같은 답을 봐야 한다.** 커서 밑의 타일을 알아내려면
    // 이 매핑을 거꾸로 타야 하는데, 그 산술을 두 번째로 적는 순간 커서가 가리키는
    // 타일과 화면이 밝히는 타일이 다른 타일이 될 수 있다. 화면 카드의 글자와 클릭
    // 상자를 `BuildScreenLayout()` 하나가 만드는 것과 같은 이유다.
    struct TopDownLayout
    {
        int scale = 1;
        int originX = 0;
        int originY = 0;

        // 타일의 한가운데에 해당하는 프레임 픽셀.
        Int2 CenterOf(Int2 tile) const
        {
            return { originX + tile.x * scale + scale / 2, originY + tile.y * scale + scale / 2 };
        }

        // 이 프레임 픽셀이 떨어지는 타일. 지도 밖이면 격자 밖 좌표가 나오므로
        // 받는 쪽이 `Grid::Contains()` 로 거른다.
        Int2 TileAt(Int2 pixel) const
        {
            // 나눗셈이 아니라 floor 여야 한다. 원점 왼쪽·위쪽에서 C 의 나눗셈은
            // 0 으로 끌어당기므로, -1 픽셀과 +1 픽셀이 둘 다 타일 0 이 된다.
            const int x = pixel.x - originX;
            const int y = pixel.y - originY;
            const auto floorDiv = [](int value, int divisor)
            {
                return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
            };
            return { floorDiv(x, scale), floorDiv(y, scale) };
        }
    };

    // 디버그 탑다운이 세계 위에 겹쳐 그리는 것 전부.
    //
    // **렌더러는 A* 를 부르지 않는다.** 여기 들어오는 경로와 비용은 게임이 이미
    // 들고 있는 바로 그것이다 — 추적 경로는 그 좀비가 실제로 따라가는
    // `ZombieAgent::path` 이고, 음향 필드는 침입구가 압력을 받을지 판정할 때 쓴
    // 그 배열이다. 화면이 다시 계산하면 게임에 대한 두 번째 의견이 생기고,
    // 이 저장소는 그런 두 번째 의견을 **보이는데 못 쏘는 좀비**로 한 번 출하했다.
    struct TopDownOverlay
    {
        // 플레이어 → 열쇠, 열쇠를 얻었으면 → 탈출구.
        std::vector<Int2> guidePath;
        // 플레이어를 알아챈 좀비들이 지금 따라가는 경로. 각 경로의 첫 칸이 그
        // 좀비가 지금 향하고 있는 타일이라 더 밝게 그린다.
        std::vector<std::vector<Int2>> chasePaths;
        // 타일당 비용. 비어 있으면 히트맵을 안 그린다. 색인은 y * grid.Width() + x.
        std::vector<float> costField;
        // 램프의 꼭대기에 해당하는 비용. 여기를 넘거나 무한대인 타일은 **칠하지
        // 않는다** — 안 칠해진 곳이 "거기까지는 안 간다/안 들린다"는 뜻이고,
        // 그것이 히트맵이 말하려는 것의 절반이다.
        float costScale = 1.0f;
        Rgb costNear{ 60, 200, 120 };
        Rgb costFar{ 220, 70, 50 };
        // 마우스가 가리키는 타일. 숫자 쪽이 이 칸의 비용을 적으므로 **지도에도
        // 어느 칸인지 나와야 한다** — 좌표만 적혀 있으면 읽는 사람이 48×48 격자에서
        // 그 칸을 눈으로 세어야 한다.
        bool hasCursor = false;
        Int2 cursorTile{};
    };

    // 조준선이 처음 닿는 환경 타일. 좀비는 따로 판정하므로 이것은 그들 **뒤의**
    // 세계 기하만 설명한다.
    struct EnvironmentHit
    {
        bool valid = false;
        Int2 position{};
        Tile tile = Tile::Wall;
        float distance = 0.0f;
    };

    // DDA 레이캐스팅으로 2.5D 화면을 그린다.
    class RaycastRenderer
    {
    public:
        // 자체 테스트와 --snapshot 이 쓰는 고정 기준 크기.
        static constexpr int DefaultWidth = 120;
        static constexpr int DefaultHeight = 80;
        // 자동 맞춤의 상하한. 위쪽은 아주 큰 터미널에서 출력량이 폭주하지 않게 막는다.
        // 1440x900 은 어떤 글꼴로도 보여줄 수 있는 것을 한참 넘는다.
        static constexpr int MinWidth = 60;
        static constexpr int MinHeight = 40;
        static constexpr int MaxWidth = 480;
        static constexpr int MaxHeight = 320;
        static constexpr float FieldOfView = 1.22173048f; // 70도

        explicit RaycastRenderer(int width = DefaultWidth, int height = DefaultHeight);

        int Width() const { return frameWidth; }
        int Height() const { return frameHeight; }

        // 프레임을 뷰포트에 맞추면 픽셀이 1:1 로 매핑되고, 그게 그 창이 낼 수 있는
        // 가장 선명한 그림이다. 이미 맞는 크기면 아무 일도 안 하므로 사용자가
        // 창을 조절하거나 최대화하는 동안 매 프레임 불러도 안전하다.
        void Resize(int width, int height);

        const PixelBuffer& Render(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors);
        const PixelBuffer& RenderTopDown(
            const Grid& grid,
            const RenderState& state,
            const std::vector<RenderActor>& actors,
            const TopDownOverlay& overlay,
            bool showLiveState);

        // 탑다운 지도의 배치. **RenderTopDown 이 쓰는 것과 같은 함수이고,
        // 그래서 커서를 타일로 되돌리는 쪽도 같은 답을 본다.** 격자 크기만 읽으므로
        // 그리지 않고도 물을 수 있다.
        static TopDownLayout MakeTopDownLayout(int frameWidth, int frameHeight, const Grid& grid);
        int HitTestZombie(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors) const;
        EnvironmentHit HitTestEnvironment(const Grid& grid, const RenderState& state) const;

    private:
        friend struct SelfTestAccess;

        // 레이가 무엇을 찾는가. Blocking 은 렌더러의 모든 패스가 쓰는 벽 탐색이고,
        // Opening 은 그 벽 앞의 첫 통과 가능한 흔적을 찾는다.
        enum class RayTarget
        {
            Blocking,
            Opening
        };

        struct RayHit
        {
            float distance = 1000.0f;
            float textureX = 0.0f;
            Tile tile = Tile::Wall;
            bool side = false;
            Int2 position{};
            bool hit = false;
        };

        // 세계의 한 점이 화면 어디에 떨어지는가, 그리고 애초에 화면 안인가.
        // 열쇠와 탄약이 둘 다 바닥에 선 납작한 표식이고 자리를 잡을 때 같은 여섯
        // 가지를 물었다.
        struct Billboard
        {
            bool visible = false;
            int centerX = 0;
            float distance = 0.0f;
        };

        Billboard ProjectBillboard(const RenderState& state, Vec2 world) const;
        RayHit CastRay(const Grid& grid, Vec2 origin, float angle,
            RayTarget target = RayTarget::Blocking) const;
        static float NormalizeAngle(float angle);
        static bool IsOpening(Tile tile);

        // 투영된 상자와 모양 인자를, **한 곳에서, 그리기와 명중 판정 둘 다에게.**
        // ZombieSprite 를 보라.
        ZombieSprite MakeZombieSprite(const RenderActor& actor, const RenderState& state) const;

        static bool IsZombiePixel(float u, float v, const ZombieSprite& shape);

        // 눈 두 개를, 마스크와 같은 좌표계에서. 따로 둔 이유는 이것이 모양이 아니라
        // **색**이기 때문이다 — 눈에 맞은 총알은 머리에 맞은 것이고, 명중 판정은
        // 눈이 어디 있는지 알 필요가 없어야 한다.
        static bool IsZombieEyePixel(float u, float v, const ZombieSprite& shape);

        void DrawSprites(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors);
        void DrawExitZone(const Grid& grid, const RenderState& state);
        void DrawOpeningRemnants(const Grid& grid, const RenderState& state);
        void DrawKey(const Grid& grid, const RenderState& state);
        void DrawAmmoPickups(const Grid& grid, const RenderState& state);
        void DrawWeapon(float recoil, Vec2 sway);
        void DrawHitFlash(float amount);
        void DrawWeaponAndHud(const RenderState& state);
        void DrawMinimap(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors);

        int frameWidth = DefaultWidth;
        int frameHeight = DefaultHeight;
        PixelBuffer frame;
        std::vector<float> depthBuffer;
    };
}
