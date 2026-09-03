#pragma once

#include <AI/AStar.h>
#include <Game/AudioCues.h>
#include <Game/Balance.h>
#include <Game/BreachSystem.h>
#include <Game/Hud.h>
#include <Game/StatusLog.h>
#include <Platform/TextRaster.h>
#include <Platform/Terminal.h>
#include <Render/RaycastRenderer.h>
#include <World/MapGenerator.h>
#include <cstdint>
#include <filesystem>
#include <random>
#include <vector>

namespace Zombie
{
    // 살아 움직이는 좀비 하나.
    //
    // 맵이 정한 시작 자리(ZombieSpawn)와 다르다. 저쪽은 배치이고 이쪽은 상태다.
    struct ZombieAgent
    {
        Vec2 position{};
        std::vector<Int2> path;
        std::size_t pathIndex = 0;
        float repathTimer = 0.0f;
        float attackTimer = 0.0f;
        // 몸이 향한 방향. 진행 방향으로 **스냅이 아니라 서서히** 돌린다. A* 경로는
        // 네 방향뿐이라, 스냅하면 좀비가 움직이기 한 걸음 전에 제자리에서 홱 돈다.
        float facing = 0.0f;
        // 보폭 위상(1이 한 주기). **시간이 아니라 걸은 거리로** 나아간다.
        // 그래서 문에 기대고 있는 좀비가 제자리걸음을 하지 않고, 걸음걸이가 세계의
        // 함수가 된다 — 그게 렌더러를 순수 함수로, 렌더 다이제스트를 오라클로 유지한다.
        float stridePhase = 0.0f;
        // 0 이 서 있는 것, 1 이 완전히 쓰러진 것. alive 가 false 가 된 뒤에 돌고
        // 그대로 남는다.
        float deathProgress = 0.0f;
        // 다음에 울 때까지 남은 초. 플레이어를 알아챈 좀비는 계속 그렇다고 말한다.
        // 깨어난 뒤 조용해진 좀비는 플레이어가 믿지 않게 되는 좀비다.
        float growlTimer = 0.0f;
        bool alert = false;
        bool alive = true;

        // 침입 창문 뒤의 밀폐 포켓에 미리 놓인 좀비에게 선다. 그 창문이 깨지기
        // 전까지 완전히 정지해 있다. 그게 유일한 입장 경로다.
        bool hasBreachLink = false;
        Int2 breachPosition{};
        Int2 otherBreachPosition{ -1, -1 };
        // 두 연결은 생성 결과의 정체성이고 바꾸지 않는다. 소음에 실제로 반응한
        // 첫 출구만 여기에 고정해, 한 사건에서 두 출구가 모두 반응해도 목표가
        // 호출 순서에 따라 다시 뒤집히지 않게 한다.
        Int2 selectedBreachPosition{ -1, -1 };
        bool waitingBehindBreach = false;
    };

    // 상태, 화면 흐름, 루프, Update 디스패치, 렌더 상태 조립.
    class Game
    {
    public:
        // seedPinned 는 시드를 명령줄에서 요청받았다는 뜻이다. 고정이면 메뉴가 매번
        // 그것을 재현하는데, `--seed` 가 존재하는 이유가 그것이다. 고정이 아니면
        // 메뉴가 시작할 때마다 새 맵을 굴린다 — "시작"을 누른 플레이어가 기대하는 것.
        // 기본값이 있는 이유는 자체 테스트가 이 선택 없이 Game 을 만들 수 있게 하려는 것.
        //
        // executable 은 argv[0] 이고, **판 도중에 밸런스 파일을 다시 찾기 위해서만**
        // 들고 있는다. 자체 테스트는 Game 을 직접 만들고 다시 읽을 것이 없어서 기본값이다.
        Game(std::uint32_t seed, Balance balance, bool seedPinned = false,
            std::filesystem::path executable = {});
        int Run();

        // argv[0] 을 받아 게임과 같은 방식으로 출하 설정을 찾는다. 비어 있으면
        // 그 검사 하나만 건너뛴다 — 따로 떼어낸 사본을 실패시키지 않으려는 것.
        static int RunSelfTest(const std::filesystem::path& executable = {});
        static int WriteSnapshot(std::uint32_t seed, const std::filesystem::path& path);

    private:
        // 자체 테스트는 SelfTest.cpp 안의 static 검사 한 덩어리라, 그중 어느 것도
        // 이 헤더에 선언할 필요가 없다. **일부러 이 private 상태 안으로 들어온다** —
        // 공개 표면만 볼 수 있는 테스트는 이 프로젝트에서 실제로 망가졌던 것들을
        // 검사할 수 없다.
        friend struct SelfTestAccess;

        void Reset(std::uint32_t newSeed);
        void Update(float deltaTime, const InputState& input);

        // 3.9 의 화면 흐름. 흐름 키 셋에 대해 눌린 순간만 잡는다. Update() 와 떼어
        // 놓은 이유는, 이것이 세계가 멈춰 있는 동안에도 도는 유일한 부분이기 때문이다.
        void ApplyScreenInput(const InputState& input);

        // 포인터가 어느 선택지 위에 있는가, 없으면 None. **클릭이 도착한 프레임에만**
        // 묻는다. 쉬고 있는 마우스를 위해 배치를 초당 예순 번 다시 만들지 않는다.
        ScreenAction PointerChoice() const;

        // 생성을 예약한다. **맵은 여기서 안 만든다.** Run() 이 "맵을 만드는 중" 카드를
        // 먼저 띄우고 그 프레임이 화면에 나간 다음에 만든다. 그래야 멈춤이 멈춤이
        // 아니라 일하는 것으로 읽힌다.
        void BeginRun(std::uint32_t newSeed);

        // 지금 물려 있는 것과 절대 같지 않은 시드. "새 맵"이 같은 맵을 돌려주면
        // 우연이 아니라 고장 난 키로 읽힌다.
        std::uint32_t NextRandomSeed() const;

        // 밸런스 파일을 다시 읽고 무엇이 움직였는지 보고한다. 맵을 만들 때만 효과가
        // 있는 값은 **그렇다고 이름을 대며** 알린다. 조용히 적용하면 "또 안 먹네"가
        // 되고, 그게 이 기능이 없애려던 결론이다.
        void ReloadBalance();

        RunSummary MakeRunSummary() const;
        void UpdatePlayer(float deltaTime, const InputState& input);
        void UpdateZombies(float deltaTime);
        // 살아 있는 좀비는 작은 원형 몸을 가진다. A* 는 여전히 정적 타일만 보고,
        // 실제 이동이 끝난 뒤 이 반경으로 서로 밀어내 길찾기와 군중 처리를 분리한다.
        static constexpr float ZombieCollisionRadius = 0.24f;
        bool CanZombieOccupy(Vec2 position) const;
        void SeparateZombies();
        void EmitNoise(float amount, float reactionChance);
        void Shoot();
        void BreakWindow(Int2 position);
        void TryInteract();
        // 이 타일이 속한 출입구의 두 타일을 함께 연다.
        void OpenDoorway(Int2 tile);
        bool TryVaultWindow(Int2 window);

        // 소리가 **실제로 닿는** 좀비 전부. 침입구가 듣는 것과 같은 음향 필드로 잰다.
        // 누가 듣는지를 정하는 것은 생거리가 아니라 벽이다.
        void AlertZombiesWithinEarshot(float amount);

        void PushLog(const std::string& text);

        // 세계 안에 자리가 있는 소리. 음량을 좀비가 듣는 것과 같은 음향 필드에서
        // 뽑는다. 그래서 플레이어와 소리 사이의 벽이 단순한 거리가 아니라 벽으로 들린다.
        //
        // **들렸는지를 돌려준다.** 상태 로그가 여기에 기대고 있다 — 로그의 줄이
        // 플레이어가 뭔가를 듣는다고 말하므로, 그것이 사실이어야 한다.
        bool PlayWorldCue(AudioCue cue, Int2 tile);

        // 좀비가 깨어나는 **유일한** 길. 예전에 손으로 alert 를 세우던 경로가 전부
        // 여기를 지난다. 그래서 어느 경로도 소리를 빠뜨릴 수 없다 — 울음소리가
        // 소음 경로 하나에만 달려 있었고, 그냥 눈으로 본 좀비는 조용히 왔다.
        void WakeZombie(ZombieAgent& zombie);

        void UpdateLog(float deltaTime);

        // HUD 표시와 상태 로그를 프레임 픽셀 위에 배치한다. 크기와 위치가 전부
        // 프레임에서 나오므로, 플레이어가 어떤 창을 열었든 UI 가 화면의 같은 비율을
        // 차지한다.
        std::vector<TextSpan> MakeOverlay(int frameWidth, int frameHeight) const;

        // 지금 화면의 카드를, 이미 다 그려진 프레임 위에 얹는다. Playing 에는 카드가
        // 없고 BuildScreenLayout 이 그 경우 아무것도 안 돌려주므로, 호출부마다
        // 조건을 다는 대신 무조건 부른다.
        void DrawScreenCard(PixelBuffer& frame) const;

        void StirBreachZombie(Int2 breachPosition);
        bool CanPlayerOccupy(Vec2 position) const;
        void MovePlayer(Vec2 delta);
        RenderState MakeRenderState(const InputState& input) const;
        std::vector<RenderActor> MakeRenderActors() const;
        std::vector<Int2> MakeGuidePath() const;

        // 탑다운 위에 겹치는 것 — 안내 경로, 추적 경로, 비용 히트맵.
        //
        // **여기서 A* 를 새로 돌리는 것은 히트맵의 필드뿐이고**, 경로는 둘 다 이미
        // 있는 것을 옮겨 담는다. 안내 경로는 MakeGuidePath(), 추적 경로는 좀비가
        // 지금 따라가고 있는 zombie.path 다. 화면이 자기 경로를 따로 계산하면
        // 좀비가 실제로 가는 길과 다른 길을 그릴 수 있다.
        TopDownOverlay MakeTopDownOverlay(int frameWidth, int frameHeight);

        // 마우스가 가리키는 타일, 지도 밖이면 격자 밖 좌표.
        //
        // **지도에 테두리를 두르는 쪽과 비용을 적는 쪽이 이 함수 하나를 부른다.**
        // 둘이 각자 계산하면 밝아진 칸과 숫자가 말하는 칸이 다른 칸이 될 수 있고,
        // 그건 이 저장소가 좀비 스프라이트와 화면 카드에서 이미 두 번 겪은 버그다.
        Int2 TopDownCursorTile(int frameWidth, int frameHeight) const;

        // 탑다운 좌상단에 적을 숫자들. 커서 밑 타일은 렌더러의 탑다운 배치를
        // 거꾸로 타서 구하므로, 화면이 밝히는 타일과 숫자가 말하는 타일이 같다.
        DebugReadout MakeDebugReadout(int frameWidth, int frameHeight);

        // 플레이어 타일에서 편 **이동** 비용 필드. 음향 쪽은 BreachSystem 이
        // 이미 캐시하고 있어서 이쪽만 있으면 된다.
        //
        // 같은 타일이고 배치도 안 바뀌었으면 다시 안 편다 — BreachSystem 이
        // 음향 필드에 쓰는 것과 같은 (원점, Revision) 규칙이다.
        const std::vector<float>& TraversalField();

        bool IsInspectingGeneration() const;

        // Base Seed — `--seed` 가 받은 값이거나 시작할 때의 시계.
        // 결과 화면의 R 이 **정확히 이것으로** 다시 시작한다. 재시도가 옮겨갔을 수도
        // 있는 Grid::seed 와 따로 두는 이유가 그것이다.
        std::uint32_t seed = 0;
        bool seedPinned = false;
        // 기본이 MainMenu 가 아니라 Playing 이다. 직접 만들어진 Game — 자체 테스트가
        // 열댓 번 그렇게 한다 — 은 이미 진행 중인 게임이고, 메뉴는 Run() 이 그 앞에
        // 세우는 것이다.
        Screen screen = Screen::Playing;
        std::uint32_t pendingSeed = 0;
        std::filesystem::path executablePath;
        bool quitRequested = false;
        // 지금까지의 기록. 시간은 **Playing 단계에서만** 쌓이므로 일시정지는 논
        // 시간이 아니고, 결과 카드가 읽히는 동안 숫자가 계속 올라가지 않는다.
        float runSeconds = 0.0f;
        int shotsFired = 0;
        int shotsHit = 0;
        int kills = 0;
        // 생성자가 덮어쓴다. 0 이 아니라 출하 숫자에서 출발한다는 것은, Game 이
        // 잠깐이라도 규칙 없는 게임인 순간이 없다는 뜻이다.
        Balance balance = Balance::Defaults();
        MapGenerator generator;
        BreachSystem breachSystem;
        Grid grid;
        std::vector<MapGenerationSnapshot> generationTrace;
        RaycastRenderer renderer;
        std::vector<ZombieAgent> zombies;
        Vec2 playerPosition{};
        float playerAngle = 0.0f;
        int health = 3;
        int ammo = 3;
        bool hasKey = false;
        bool won = false;
        bool lost = false;
        // 뒤돌아보기 키를 누르고 있는 동안 참. **카메라만 돌린다** — 몸은 앞을 향한
        // 채이고 사격은 거절된다.
        bool lookingBehind = false;
        bool showDebugMap = false;
        bool developerDebug = false;
        bool debugTopDown = false;
        // F4 가 돌리는 비용 히트맵. developerDebug 밖에서는 아무 데도 안 쓰인다.
        CostOverlay debugCostOverlay = CostOverlay::None;
        // 이 맵의 출구 → 열쇠 A* 비용. **Reset() 에서 한 번 잰다** — 맵이 사는 동안
        // 변하지 않는 값이라 프레임마다 다시 물을 이유가 없고, 디버그 화면이
        // `minimum_key_path_cost` 규칙이 실제로 얼마를 냈는지 보여준다.
        float keyPathCost = 0.0f;
        // TraversalField() 의 캐시. 원점이나 배치가 움직였을 때만 다시 편다.
        std::vector<float> traversalField;
        Int2 traversalOrigin{ -1, -1 };
        std::uint32_t traversalRevision = 0;
        bool traversalValid = false;
        std::size_t generationStep = 0;
        float rollTimer = 0.0f;
        float rollCooldown = 0.0f;
        float stamina = 0.0f;
        float staminaIdleTime = 0.0f;
        // 사격 반동의 남은 초. **총알이 실제로 나갔을 때만** 선다. 탄약이 없어서
        // 거절된 방아쇠는 총을 흔들지 않는다. 조준선 반동이 같은 시계를 쓴다 —
        // 한 사건에 시계 하나다.
        float weaponRecoil = 0.0f;
        // 조준 흔들림. 위상은 고정 시간 간격으로 쌓이고 세기는 플레이어가 하는 일을
        // 향해 서서히 간다. 그래서 같은 입력이면 언제나 같은 흔들림이 나온다.
        // 7.x 가 난수를 금지하는 이유가 정확히 그것이고, 이 프로젝트의 오라클들이
        // 거기에 기대고 있다.
        float aimSwayPhase = 0.0f;
        float aimSway = 0.0f;
        float aimSwayTarget = 0.0f;
        // 마지막 피격의 붉은 테두리가 남은 초. 플레이어가 죽으면 Update() 가
        // UpdatePlayer 를 그만 부르므로, 마지막 일격의 붉은 화면이 그 자리에 얼어붙는다.
        // **일부러 그렇다** — 방을 평범하게 비추는 화면으로 잦아드는 대신 죽음의
        // 그림으로 남는다.
        float hitFlash = 0.0f;

        // 여기서는 기본 생성만 하고 실제로 여는 것은 Run() 이다. 생성자에서 열면
        // 자체 테스트가 직접 만드는 열댓 개의 Game 마다 XAudio2 엔진이 하나씩 뜬다.
        AudioBank audio;
        // 오디오 장치를 열면서 나온 말. **Reset() 이 판마다 로그를 비우기 때문에**
        // 들고 있는다. 이게 없으면 장치 없음·파일 없음 한 줄이 첫 생성에서 지워지고
        // 영영 안 보인다.
        std::string startupNote;
        StatusLog statusLog;
        float measuredFps = 0.0f;
        float framesThisSecond = 0.0f;
        float fpsWindow = 0.0f;
        float footstepTimer = 0.0f;
        std::mt19937 noiseRandom;
        bool previousRoll = false;
        bool previousInteract = false;
        bool previousDebug = false;
        bool previousDeveloperDebug = false;
        bool previousDebugTopDown = false;
        bool previousDebugCostField = false;
        bool previousGenerationStep = false;
        bool previousNextGenerationStep = false;
        bool previousRegenerate = false;
        bool previousMenu = false;
        bool previousConfirm = false;
        bool previousNewSeed = false;
        bool previousReloadBalance = false;
        Int2 aimPixel{ RaycastRenderer::DefaultWidth / 2, RaycastRenderer::DefaultHeight / 2 };
    };
}
