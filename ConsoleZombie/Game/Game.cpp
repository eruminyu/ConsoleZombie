#include "Game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Zombie
{
    namespace
    {
        // 생성기 자신의 기본값 위에 플레이어가 튜닝할 수 있는 값을 덮어씌운 것.
        // 7단계에서 좀비와 탄약 예산이 같은 관을 타게 됐다.
        MapGenerationSettings MakeMapSettings(const Balance& balance)
        {
            MapGenerationSettings settings;
            // 그대로 통과시킨다. 예전에는 범위 제한을 여기서 걸었는데, 그러면
            // 게임 안쪽 호출 한 단계 깊은 곳이라 **Balance 를 읽는 다른 모든 곳이 —
            // 자체 테스트 포함 — 잘리지 않은 숫자를 보게 됐다.** 지금은
            // Balance::ApplyLimits 에 있어서, Balance 는 누가 보기 전에 이미 온전하다.
            settings.minimumKeyPathCost = balance.minimumKeyPathCost;
            settings.zombieCount = balance.zombieCount;
            settings.extraAmmoRounds = balance.extraAmmoRounds;
            settings.startAmmo = balance.startAmmo;
            return settings;
        }

        // BreachSystem 이 받는 두 튜닝을, 각각 한 곳에서만 조립한다.
        //
        // 예전에는 생성자와 ReloadBalance() 가 아홉 필드를 각각 적었다. 그러면
        // 밸런스 키 하나를 두 번 적어야 하고, **한쪽만 적은 키는 F5 가 조용히
        // 적용을 거부하는 값**이 된다. 이 저장소에서 이미 일어난 사고다 (5013bdc).
        BreachTuning MakeBreachTuning(const Balance& balance)
        {
            return BreachTuning{
                balance.breachWarningPressure,
                balance.breachCrackPressure,
                balance.breachBreakPressure,
                balance.breachPressureDecay,
                balance.breachDecayDelay };
        }

        NoiseTuning MakeNoiseTuning(const Balance& balance)
        {
            return NoiseTuning{
                balance.noiseAcousticFalloff,
                balance.noisePressureScale,
                balance.breachExistingPressureWeight,
                balance.breachMaxResponders };
        }

        // 이 좀비가 플레이어를 쫓고 있는가.
        //
        // 깨어 있는 것만으로는 부족하다. 감염 공간 안에서 자기 침입구를 밀고 있는
        // 좀비는 alert 이면서 **경로가 없다.** 쫓는 마리 수에 그것을 세면 화면에
        // 아무 선도 없는데 숫자만 올라간다.
        bool ChasesPlayer(const ZombieAgent& zombie)
        {
            return zombie.alive && zombie.alert && !zombie.waitingBehindBreach;
        }

        // 이 좀비가 아직 안 지난 경로. 첫 칸이 지금 향하고 있는 타일이다.
        //
        // **다시 계산하지 않고 자르기만 한다.** 이것이 좀비가 실제로 따라가는 그
        // 경로여야 한다 — 화면이 자기 A* 를 돌리면 다시 계산 주기 사이에 화면과
        // AI 가 서로 다른 길을 말하게 된다.
        std::vector<Int2> RemainingPath(const ZombieAgent& zombie)
        {
            if (zombie.pathIndex >= zombie.path.size()) return {};
            return { zombie.path.begin() + static_cast<std::ptrdiff_t>(zombie.pathIndex),
                zombie.path.end() };
        }

        // 남은 경로를 마저 걷는 데 드는 이동 비용.
        //
        // AStar::PathCost 는 첫 칸 값을 안 낸다 — 이미 거기 서 있다고 보기 때문이다.
        // 남은 경로의 첫 칸은 **아직 안 들어간 칸**이라 그 값을 되돌려 더한다.
        float RemainingCost(const Grid& grid, const std::vector<Int2>& remaining)
        {
            if (remaining.empty()) return 0.0f;
            return AStar::PathCost(grid, remaining) + grid.TraversalCost(remaining.front());
        }

        // 히트맵 램프의 꼭대기에 해당하는 비용.
        //
        // **화면의 색과 화면의 숫자가 이 함수 하나를 읽는다.** 스케일이 두 곳에
        // 적히면 범례가 그림에 대해 틀린 말을 하게 되고, 그건 범례가 없는 것보다 나쁘다.
        //
        // 둘 다 임의의 상수가 아니라 규칙에서 나온 숫자다. 이동 쪽은 열쇠 배치가
        // 재는 하한이고, 음향 쪽은 **총 한 발이 실제로 닿는 거리**다 —
        // 유효 소음이 `원본 − 비용 × 감쇠` 이므로 0 이 되는 지점이 그것이다.
        float CostOverlayScale(const Balance& balance, CostOverlay overlay)
        {
            switch (overlay)
            {
            case CostOverlay::Traversal:
                return std::max(1.0f, balance.minimumKeyPathCost);
            case CostOverlay::Acoustic:
                return std::max(1.0f, balance.shotNoise / std::max(0.01f, balance.noiseAcousticFalloff));
            case CostOverlay::None:
                break;
            }
            return 0.0f;
        }

        // 프레임을 눕혀서 그 위의 글자가 읽히게 한다. Playing 을 뺀 모든 화면이
        // **더 이상 플레이되고 있지 않은 그림** 위의 카드이고, 카드 뒤가 원래 대비
        // 그대로면 둘 다 안 읽힌다.
        void Dim(PixelBuffer& frame, float scale)
        {
            for (int y = 0; y < frame.Height(); ++y)
                for (int x = 0; x < frame.Width(); ++x)
                    frame.Set(x, y, Rgb::Scale(frame.Get(x, y), scale));
        }
    }

    // 놀 수 있는 게임 하나를 세운다. 생성자가 끝난 시점에 이미 맵이 있고 좀비가 서 있다.
    //
    // 오디오는 여기서 안 연다. Run() 이 연다.
    Game::Game(std::uint32_t seed, Balance balance, bool seedPinned,
        std::filesystem::path executable)
        : seed(seed), seedPinned(seedPinned), executablePath(std::move(executable)),
        balance(balance), generator(MakeMapSettings(balance)),
        breachSystem(MakeBreachTuning(balance), MakeNoiseTuning(balance))
    {
        if (!this->balance.problems.empty())
            throw std::invalid_argument("Game cannot start with invalid balance settings");
        statusLog.Configure(balance.uiLogSeconds, balance.uiLogCooldown);
        Reset(seed);
    }

    // 게임 루프. 고정 60Hz 업데이트와 설정이 정한 렌더 주기를 따로 돈다.
    //
    // 화면 흐름(3.9)이 이 안에서 굴러간다 —
    // EnvironmentCheck → MainMenu → Generating → Playing → Paused / Dead / Escaped.
    int Game::Run()
    {
        // 3.9 의 EnvironmentCheck. 자기 화면이 없는데, 여기서 실패했다는 것은
        // **화면을 그릴 곳이 없다는 뜻**이기 때문이다.
        TerminalSession terminal(RaycastRenderer::DefaultWidth, RaycastRenderer::DefaultHeight / 2);
        if (!terminal.IsValid())
        {
            std::cerr << "Windows Terminal VT output is required. Run --self-test for a headless check.\n";
            return 2;
        }
        // 생성자는 이미 놀 수 있는 게임을 세워 놓았다. Run() 은 그 앞에 메뉴를
        // 세운다 — 한 판은 프로세스가 시작할 때가 아니라 **플레이어가 시작하라고
        // 할 때** 시작한다.
        screen = Screen::MainMenu;
        quitRequested = false;
        // 건드리는 것은 셀 크기뿐이고, 그것조차 conhost 만 따른다.
        // **창은 손으로 최대화한다.** 프로그램에서 최대화하면 마우스 입력과
        // 창 조작 버튼이 함께 망가진다.
        terminal.TryApplyFontHeight(balance.terminalFontHeight);
        terminal.SetOutputLimits(balance.presentMaxColumns, balance.presentMaxRows);

        // 생성자가 아니라 여기다. 자체 테스트가 Game 을 직접 만드는데, 생성자에서
        // 열면 그때마다 XAudio2 엔진이 하나씩 뜬다.
        //
        // 자산은 실행 파일 옆에 있고 빌드도 거기로 복사한다. 그래서 **설정 파일과
        // 같은 함정을 공유한다** — 다음 빌드 전까지는 exe 옆의 낡은 사본이 저장소의
        // 새 것을 이긴다.
        if (const std::string note = audio.Open(FindSoundFolder(),
                balance.audioEnabled != 0, balance.masterVolume,
                balance.sfxVolume, balance.bgmVolume);
            !note.empty())
        {
            // 무엇이 잘못됐든 한 줄이다. 안 열리는 장치와 파일 없는 큐 여덟 개는
            // 둘 다 플레이어가 알고 싶을 수 있는 것이지만, 각각 한 줄씩 쓸 값은 아니다.
            startupNote = note;
        }

        using Clock = std::chrono::steady_clock;
        // 루프의 쉬는 시간. **이것이 없으면 render_fps 의 위쪽 절반이 도달 불가능하다.**
        const FrameSleeper sleeper;
        if (!sleeper.IsHighResolution())
        {
            // 조용히 느린 것과 느리다고 말하는 것은 다르다. 이 기기에서는 60 을
            // 부탁해도 50 근처가 천장이고, 그것을 모르면 밸런스 파일을 의심하게 된다.
            //
            // **덮어쓰지 않고 붙인다.** 바로 위에서 오디오가 같은 자리에 한 줄을
            // 남겼을 수 있고, 둘 다 첫 생성에서 한 번만 나올 기회를 갖는다.
            startupNote = startupNote.empty()
                ? std::string("고해상도 타이머가 없어 프레임 상한이 낮다")
                : startupNote + " · 고해상도 타이머 없음";
        }
        constexpr float fixedStep = 1.0f / 60.0f;
        // **루프 밖에서 한 번이 아니라 루프 안에서 읽는다.** 예전에는 render_fps 를
        // 여기서 const 로 잡아뒀고, 그래서 밸런스를 다시 읽으면 숫자는 움직이는데
        // 루프는 옛 값을 계속 썼다 — 이 기능 전체가 없애려던 바로 그 침묵이,
        // 한 단계 더 안쪽에 숨어 있었던 것이다.
        float renderStep = 1.0f / static_cast<float>(balance.renderFps);
        // 값이 움직였을 때만 다시 적용한다. 나머지와 달리 이것은 콘솔 호스트에게
        // 말을 거는 일이고, 매 프레임 하는 것이 공짜가 아니다.
        int appliedFontHeight = balance.terminalFontHeight;
        auto previous = Clock::now();
        float accumulator = 0.0f;
        float renderAccumulator = renderStep;
        InputState latestInput{};
        // 렌더러가 처음 대입할 때 크기가 잡힌다.
        PixelBuffer presentation(1, 1);

        while (!quitRequested)
        {
            const auto now = Clock::now();
            const float elapsed = std::min(0.25f, std::chrono::duration<float>(now - previous).count());
            previous = now;
            accumulator += elapsed;
            renderAccumulator += elapsed;
            // **그린 반복만이 아니라 루프의 모든 반복**에 대해 실제 시간을 잰다.
            // 루프는 1밀리초씩 자므로, 렌더 분기 안에서 세면 프레임 수를 실제 걸린
            // 시간의 작은 조각으로 나누게 된다. 30FPS 로 도는 기기가 수백 FPS 를
            // 보고했던 이유다.
            fpsWindow += elapsed;
            // 프레임에 대해 밸런스가 말하는 것 전부를 매번 새로 읽는다.
            // 싸다 — 정수 두 번 쓰고 나누기 한 번이다.
            renderStep = 1.0f / static_cast<float>(std::max(1, balance.renderFps));
            terminal.SetOutputLimits(balance.presentMaxColumns, balance.presentMaxRows);
            if (balance.terminalFontHeight != appliedFontHeight)
            {
                appliedFontHeight = balance.terminalFontHeight;
                terminal.TryApplyFontHeight(appliedFontHeight);
            }
            const bool pendingClick = latestInput.leftClicked;
            latestInput = terminal.PollInput();
            latestInput.leftClicked = latestInput.leftClicked || pendingClick;

            while (accumulator >= fixedStep)
            {
                Update(fixedStep, latestInput);
                accumulator -= fixedStep;
                latestInput.leftClicked = false;
            }

            if (renderAccumulator >= renderStep)
            {
                renderAccumulator = std::fmod(renderAccumulator, renderStep);
                // 프레임 하나의 델타가 아니라 1초 동안 세어서 낸다. 그래야 화면의
                // 숫자가 읽을 수 있을 만큼 가만히 있는다.
                ++framesThisSecond;
                if (fpsWindow >= 1.0f)
                {
                    measuredFps = framesThisSecond / fpsWindow;
                    framesThisSecond = 0.0f;
                    fpsWindow = 0.0f;
                }
                // 프레임을 **사용자가 실제로 열어 놓은 창**에 맞춘다, 설정한 천장까지.
                // 천장 아래에서는 1:1 이고 그게 그 창이 낼 수 있는 가장 선명한 그림이다.
                // 천장 위에서는 같은 프레임을 확대해 창을 채우므로 비용이 모니터를
                // 따라 커지지 않는다.
                //
                // 사용자는 실행한 뒤에 손으로 최대화하므로 **시작할 때 한 번으로는
                // 할 수 없다.**
                renderer.Resize(
                    std::min(terminal.ContentColumns(), balance.renderMaxWidth),
                    std::min(terminal.ContentRows() * 2, balance.renderMaxHeight));
                const RenderState renderState = MakeRenderState(latestInput);
                const std::vector<RenderActor> actors = MakeRenderActors();
                std::ostringstream title;
                const bool worldOnScreen =
                    screen != Screen::MainMenu && screen != Screen::Generating;
                if (!worldOnScreen)
                {
                    // 이 둘 뒤에는 세계가 없다. 메뉴는 타이틀 화면이고, 생성 카드는
                    // 그것이 알리는 맵이 아직 존재하지 않는 동안 떠 있다. 그래서 둘 다
                    // 이전 판의 그림 대신 자기만의 평평한 바탕을 받는다.
                    presentation = PixelBuffer(renderer.Width(), renderer.Height());
                    presentation.Clear(Rgb{ 12, 13, 16 });
                    DrawScreenCard(presentation);
                    terminal.Present(presentation);
                    title << "Console Zombie";
                }
                else if (developerDebug && debugTopDown)
                {
                    const bool live = !IsInspectingGeneration();
                    const Grid& displayGrid = live ? grid : generationTrace[generationStep].grid;
                    const std::vector<RenderActor> noActors;
                    // 과거 스냅샷에는 겹칠 것이 없다. 그 격자에는 좀비도 플레이어도
                    // 없고, 지금 세계의 경로를 옛 지형 위에 그리면 **있지도 않은 벽을
                    // 통과하는 선**이 나온다.
                    const TopDownOverlay overlay = live
                        ? MakeTopDownOverlay(renderer.Width(), renderer.Height())
                        : TopDownOverlay{};
                    // 3D 화면과 같은 이유로 **복사본에** 글자를 얹는다.
                    // RenderTopDown 은 세계의 순수 함수로 남는다.
                    presentation = renderer.RenderTopDown(
                        displayGrid,
                        renderState,
                        live ? actors : noActors,
                        overlay,
                        live);
                    if (live)
                    {
                        TextRaster::DrawAll(presentation, BuildDebugReadout(
                            MakeDebugReadout(presentation.Width(), presentation.Height()),
                            presentation.Width(), presentation.Height(), balance.uiTextHeight));
                    }
                    terminal.Present(presentation);

                    title << "DEBUG TOP-DOWN | seed " << seed << " | ";
                    if (live)
                    {
                        title << "LIVE | cyan path: " << (hasKey ? "EXIT" : "KEY");
                    }
                    else
                    {
                        title << "GEN " << (generationStep + 1) << '/' << generationTrace.size()
                            << " | " << generationTrace[generationStep].label;
                    }
                    title << " | F3 3D | F4 cost | [ ] steps | F2 exit";
                }
                else
                {
                    // 타이틀바가 지고 있던 것은 전부 화면으로 옮겼다. 플레이어가
                    // 이미 보고 있는 곳이다. 제목은 게임 이름만 말한다.
                    //
                    // UI 는 **복사본에** 그린다. 그래야 Render() 가 세계의 순수 함수로
                    // 남는다. 프레임 복사는 수백 KB 의 memcpy 이고, 그 프레임을 채운
                    // 레이캐스트 옆에서는 눈에 띄지도 않는다.
                    presentation = renderer.Render(grid, renderState, actors);
                    // 일시정지와 결과 카드 둘은 카드를 얹기 전에 세계를 눕힌다.
                    // Playing 은 손대지 않는다. **렌더 다이제스트가 못 움직이는 이유가
                    // 그것이다** — 이 일이 Render() 밖에서 일어난다.
                    if (screen != Screen::Playing) Dim(presentation, 0.35f);
                    // HUD 는 아직 진행 중인 판의 것이다. 결과 카드에서는 판이 끝났고
                    // 의미 있는 숫자가 카드 자신에 적혀 있으니, 하트와 탄약은 그 위의
                    // 어수선함일 뿐이다.
                    if (screen == Screen::Playing || screen == Screen::Paused)
                    {
                        TextRaster::DrawAll(presentation,
                            MakeOverlay(presentation.Width(), presentation.Height()));
                    }
                    DrawScreenCard(presentation);
                    terminal.Present(presentation);
                    title << "Console Zombie";
                    if (developerDebug)
                    {
                        title << " | DEBUG INVINCIBLE | F3 top-down | F2 exit | config "
                            << (balance.loadedFromFile ? balance.sourcePath.string() : "built-in defaults");
                    }
                }
                // **두 화면 모두에.** F2 가 탑다운으로 떨어지는데, 망가진 HUD 가
                // 처음 부르는 질문이 "지금 무슨 글꼴이 물려 있냐"이기 때문이다.
                // 분기 하나에만 걸어두면 정확히 물어보는 순간에 표시가 없었다.
                //
                // 이름은 **요청한 것이 아니라 GDI 가 돌려준 것**이다. CreateFontW() 는
                // 없는 글꼴에도 성공하고 조용히 대체하므로, 후보 목록은 요청이고 이것이
                // 답이다. 없으면 "글꼴이 안 바뀌었다"와 "글꼴이 바뀌었는데도 여전히
                // 이상하다"가 같은 그림이 된다.
                if (developerDebug)
                {
                    // 처음 굽기 전까지는 비어 있다. 탑다운은 굽지 않으므로, 첫
                    // 프레임에서 F2 를 누르면 빈 이름이 찍히고 그것이 "글꼴 없음"으로
                    // 읽힌다. 뜻하는 바의 정반대다.
                    const std::string& face = TextRaster::FaceName();
                    title << " | font " << (face.empty() ? "(not baked yet)" : face)
                        << " em " << HudTextHeight(renderer.Height(), balance.uiTextHeight)
                        << " frame " << renderer.Width() << 'x' << renderer.Height();
                }
                terminal.SetTitle(title.str());

                // **정확히 여기이고 다른 어디도 아니다.** 맵을 만드는 중이라고 말하는
                // 카드가 이미 화면에 있고, 맵은 그 밑에서 만들어진다. 띄우기 전에
                // 만들면 재시도가 몰아치는 내내 이전 프레임이 그대로 굳는데,
                // 그게 정확히 멈춘 것처럼 보이는 그림이다.
                if (screen == Screen::Generating)
                {
                    Reset(pendingSeed);
                    audio.PlayMusic(AudioCue::BgmLoop);
                    // 루프가 들고 있던 경과 시간은 맵이 없던 프레임의 것이다.
                    // 그냥 두면 **새 판의 첫 걸음이 생성에 걸린 시간만큼 길다.**
                    previous = Clock::now();
                    accumulator = 0.0f;
                }
            }
            // 1밀리초를 **정말로** 1밀리초 자기 위해 FrameSleeper 를 지난다.
            // `sleep_for` 는 여기서 15.63ms 를 잤고, 그게 render_fps 를 60 으로
            // 올려도 52 밖에 안 나오던 이유다. 자세한 것은 FrameSleeper 에.
            sleeper.Sleep(std::chrono::milliseconds(1));
        }
        return 0;
    }

    // 맵을 만들고 한 판을 처음 상태로 세운다.
    //
    // **좀비를 만드는 곳은 여기 하나뿐이다.** 플레이 도중에는 아무것도 좀비를
    // 새로 만들지 않는다.
    void Game::Reset(std::uint32_t newSeed)
    {
        // 생성이 성공하기 전에는 현재 판을 건드리지 않는다. 실패한 마지막 시도의
        // Grid나 스냅샷이 LIVE 상태로 섞일 수 없게 한 번에 교체한다.
        std::vector<MapGenerationSnapshot> nextTrace;
        Grid nextGrid = generator.Generate(newSeed, &nextTrace);
        seed = newSeed;
        grid = std::move(nextGrid);
        generationTrace = std::move(nextTrace);
        breachSystem.Initialize(grid);
        // 이 맵이 배치 규칙에 대해 실제로 낸 값. **한 번만 잰다** — 맵이 사는 동안
        // 안 변한다. 방향은 검증기와 같은 출구 → 열쇠다. 뒤집으면 양 끝 타일의
        // 값이 달라서 다른 숫자가 나올 수 있고, 그러면 화면의 숫자가 규칙이 본
        // 숫자가 아니게 된다.
        keyPathCost = AStar::PathCost(grid,
            AStar::FindPath(grid, grid.exit.origin, grid.keyPosition));
        // 새 격자다. 옛 필드는 옛 맵의 것이다.
        traversalValid = false;
        generationStep = generationTrace.size();
        playerPosition = TileCenter(grid.playerSpawn);
        // 방향은 격자에 저장하지 않고 두 위치에서 계산한다. 같은 사실의 세 번째
        // 사본은 어긋날 수 있는 세 번째 것이다.
        const Vec2 towardsExit = TileCenter(grid.exit.origin) - playerPosition;
        playerAngle = std::atan2(towardsExit.y, towardsExit.x);
        health = balance.playerHealth;
        ammo = balance.startAmmo;
        hasKey = false;
        won = false;
        lost = false;
        // Reset 은 곧 한 판의 시작이다. 어느 화면이 요청했든 마찬가지다.
        screen = Screen::Playing;
        runSeconds = 0.0f;
        shotsFired = 0;
        shotsHit = 0;
        kills = 0;
        rollTimer = 0.0f;
        rollCooldown = 0.0f;
        weaponRecoil = 0.0f;
        hitFlash = 0.0f;
        aimSwayPhase = 0.0f;
        aimSway = 0.0f;
        aimSwayTarget = 0.0f;
        stamina = balance.staminaMax;
        staminaIdleTime = balance.staminaRegenDelay;
        statusLog.Clear();
        // 설정이 없다는 것은 플레이어가 **누가 튜닝한 밸런스로 놀고 있지 않다**는
        // 뜻이므로 그것은 여전히 말한다. 어느 파일을 읽었는지는 개발자의 질문이라
        // 플레이어 화면이 아니라 디버그 타이틀바에 있다.
        if (!startupNote.empty()) PushLog(startupNote);
        if (!balance.loadedFromFile) PushLog("GameBalance.ini 를 찾지 못해 기본값으로 실행 중이다");
        PushLog("열쇠를 찾아서 탈출구로 돌아와라");
        footstepTimer = 0.0f;
        noiseRandom.seed(seed ^ 0x9e3779b9u);
        // **맵의 모든 좀비가 이 목록 하나에서 나온다.** 여기서 한 번 놓인다.
        // 플레이 도중에 좀비를 새로 만드는 것은 아무것도 없다.
        zombies.clear();
        for (const ZombieSpawn& spawn : grid.zombieSpawns)
        {
            ZombieAgent agent;
            agent.position = TileCenter(spawn.position);
            // 갈 곳이 생기기 전에 향할 방향. **타일에서 읽는다.** 그래야 서로
            // 다르면서도 같은 시드의 모든 실행에서 같다. 여기서 난수를 뽑으면
            // **렌더 다이제스트 안에 난수가 들어간다.**
            agent.facing = static_cast<float>((spawn.position.x * 7 + spawn.position.y * 13) % 8)
                * 0.78539816f;
            if (spawn.WaitsBehindBreach())
            {
                agent.hasBreachLink = true;
                agent.breachPosition = spawn.breachPosition;
                agent.otherBreachPosition = spawn.otherBreachPosition;
                agent.waitingBehindBreach = true;
            }
            zombies.push_back(agent);
        }
    }

    // 고정 시간 간격 한 번. 어느 화면이냐에 따라 무엇이 도는지가 갈린다.
    //
    // Playing 이 아니면 세계도 시계도 멈춘다. 상태 로그까지 같이 멈추는데,
    // 안 그러면 아직 못 읽은 줄이 일시정지 뒤에서 사라진다.
    void Game::Update(float deltaTime, const InputState& input)
    {
        // 마우스는 터미널 셀 단위로 들어오고 프레임은 다른 크기일 수 있다.
        // **뷰포트 전체가 아니라 프레임이 그려지는 셀 블록을 통해** 매핑한다.
        // 아니면 상한이 걸린 프레임에서 조준선이 포인터에서 여백 폭만큼 밀린다.
        // 아무것도 보고되지 않으면(자체 테스트) 1:1 로 떨어진다.
        const int aimColumns = input.contentCells.x > 0 ? input.contentCells.x : renderer.Width();
        const int aimRows = input.contentCells.y > 0 ? input.contentCells.y : renderer.Height() / 2;
        const int aimCellX = input.mouseCell.x - input.contentOrigin.x;
        const int aimCellY = input.mouseCell.y - input.contentOrigin.y;
        aimPixel.x = std::clamp(aimCellX * renderer.Width() / std::max(1, aimColumns),
            0, renderer.Width() - 1);
        aimPixel.y = std::clamp((aimCellY * 2 + 1) * renderer.Height() / std::max(1, aimRows * 2),
            0, renderer.Height() - 1);

        // 다른 무엇보다 먼저, 그리고 모든 화면에서. 다시 읽기는 게임 안의 수가
        // 아니라 **게임에 대해 하는 일**이다.
        if (input.reloadBalance && !previousReloadBalance) ReloadBalance();
        lookingBehind = input.lookBehind;
        if (input.debugMap && !previousDebug) showDebugMap = !showDebugMap;
        if (input.developerDebug && !previousDeveloperDebug)
        {
            developerDebug = !developerDebug;
            debugTopDown = developerDebug;
            generationStep = generationTrace.size();
        }
        if (developerDebug && input.debugTopDown && !previousDebugTopDown)
        {
            debugTopDown = !debugTopDown;
            if (debugTopDown) generationStep = generationTrace.size();
        }
        // F4 — 비용 히트맵 한 칸. 3D 화면에서도 눌러 둘 수 있게 debugTopDown 을
        // 안 본다. F3 로 탑다운에 들어가면 이미 켜져 있는 것이 자연스럽다.
        if (developerDebug && input.debugCostField && !previousDebugCostField)
        {
            debugCostOverlay = debugCostOverlay == CostOverlay::None ? CostOverlay::Traversal
                : debugCostOverlay == CostOverlay::Traversal ? CostOverlay::Acoustic
                : CostOverlay::None;
        }
        if (developerDebug && debugTopDown)
        {
            if (input.previousGenerationStep && !previousGenerationStep && generationStep > 0)
                --generationStep;
            if (input.nextGenerationStep && !previousNextGenerationStep && generationStep < generationTrace.size())
                ++generationStep;
        }
        // 판 도중에 다음 맵으로 넘기는 것이 예전에는 누구에게나 R 이었다. 지금 R 은
        // 결과 카드의 "같은 맵 다시"이고, **뜻 둘이 키 하나를 나눠 쓸 수 없다.**
        // 옛것은 개발 편의였으므로 나머지와 함께 F2 뒤로 들어갔다.
        if (developerDebug && input.regenerate && !previousRegenerate) Reset(seed + 1);
        ApplyScreenInput(input);
        if (screen == Screen::Playing && !IsInspectingGeneration() && !won && !lost)
        {
            // 논 시간만. 일시정지는 노는 것이 아니고, 결과 카드는 읽히는 동안
            // 계속 세면 안 된다.
            runSeconds += deltaTime;
            UpdatePlayer(deltaTime, input);
            UpdateZombies(deltaTime);
            breachSystem.Update(deltaTime, grid);
            if (!hasKey && Length(playerPosition - TileCenter(grid.keyPosition)) < 0.55f)
            {
                hasKey = true;
                grid.Set(grid.exit.origin, Tile::ExitOpen);
                grid.Set(grid.exit.Second(), Tile::ExitOpen);
                PushLog("열쇠를 손에 넣었다. 탈출구로 돌아가라");
                audio.Play(AudioCue::KeyPickup);
            }
            // 추가 탄약은 보너스이지 요구가 아니다. 생성기가 그것들을 시작→열쇠
            // 동선 밖에 두므로, 하나를 지나치는 것은 언제나 선택이다.
            for (std::size_t index = 0; index < grid.ammoPickups.size(); )
            {
                if (Length(playerPosition - TileCenter(grid.ammoPickups[index].position)) < 0.55f)
                {
                    ammo += grid.ammoPickups[index].rounds;
                    grid.ammoPickups.erase(grid.ammoPickups.begin() + static_cast<std::ptrdiff_t>(index));
                    PushLog("탄환을 주웠다");
                    audio.Play(AudioCue::AmmoPickup);
                    continue;
                }
                ++index;
            }
            const bool wasOver = won || lost;
            if (hasKey && grid.exit.Contains(ToTile(playerPosition))) won = true;
            if (health <= 0) lost = true;
            // 값이 바뀌는 프레임에만. 이 블록은 매 업데이트마다 도니까, 가드가
            // 없으면 결말이 초당 예순 번 다시 발동한다.
            if (!wasOver && (won || lost))
            {
                screen = won ? Screen::Escaped : Screen::Dead;
                audio.StopMusic();
                audio.Play(won ? AudioCue::Escaped : AudioCue::Dead);
            }
        }

        // 실제 시계가 아니라 **세계와 함께** 늙는다. 일시정지 화면 뒤에서 수명이
        // 다한 줄은 아무도 읽지 못한 줄이다.
        if (screen == Screen::Playing) UpdateLog(deltaTime);
        previousRoll = input.roll;
        previousInteract = input.interact;
        previousDebug = input.debugMap;
        previousDeveloperDebug = input.developerDebug;
        previousDebugTopDown = input.debugTopDown;
        previousDebugCostField = input.debugCostField;
        previousGenerationStep = input.previousGenerationStep;
        previousNextGenerationStep = input.nextGenerationStep;
        previousRegenerate = input.regenerate;
        previousMenu = input.menu;
        previousConfirm = input.confirm;
        previousNewSeed = input.newSeed;
        previousReloadBalance = input.reloadBalance;
    }

    // 화면 흐름 키 셋(Esc / Enter / N)과 포인터 클릭을 처리한다.
    //
    // 고정 시간 간격 안에서 **세계가 멈춰 있어도 도는** 유일한 부분이다.
    void Game::ApplyScreenInput(const InputState& input)
    {
        // 누른 상태가 아니라 **누른 순간**을 본다. Esc 는 "한 화면 뒤로"인데,
        // 누른 상태로 읽으면 고정 업데이트 세 번에 흐름 전체를 걸어 나간다.
        const bool menu = input.menu && !previousMenu;
        const bool confirm = input.confirm && !previousConfirm;
        const bool freshSeed = input.newSeed && !previousNewSeed;
        const bool again = input.regenerate && !previousRegenerate;
        // 한 줄을 클릭하는 것은 그 옆의 키를 누르는 것과 같은 뜻이다. 분기마다가
        // 아니라 **여기서 한 번에** 접는다. 그래야 어떤 화면도 선택지를 한쪽 손에만
        // 내주는 일이 없다.
        const ScreenAction clicked = input.leftClicked ? PointerChoice() : ScreenAction::None;

        switch (screen)
        {
        case Screen::MainMenu:
            // `--seed` 로 특정 시드를 요청하지 않았으면 매번 새 맵이다. Base Seed 는
            // 시작할 때 한 번 정해지므로, 여기서 그것을 재현하면 **한 세션의 모든 시작이
            // 같은 맵**을 돌려줬다. 그건 결정성이 아니라 시작 버튼이 고장 난 것으로 읽힌다.
            if (confirm || clicked == ScreenAction::Start)
                BeginRun(seedPinned ? seed : NextRandomSeed());
            // **프로세스를 끝내는 유일한 출구.** 다른 모든 Esc 는 한 화면 물러나므로,
            // 복도 한가운데의 오타 하나가 한 판이 아니라 일시정지 하나를 부른다.
            else if (menu || clicked == ScreenAction::Quit) quitRequested = true;
            break;
        case Screen::Generating:
            // 이 화면은 Run() 것이다. 정확히 한 프레임만 산다.
            break;
        case Screen::Playing:
            if (menu) screen = Screen::Paused;
            break;
        case Screen::Paused:
            if (confirm || clicked == ScreenAction::Resume) screen = Screen::Playing;
            else if (menu || clicked == ScreenAction::ToMenu) screen = Screen::MainMenu;
            break;
        case Screen::Dead:
        case Screen::Escaped:
            // R 은 Base Seed 를 유지한다. 그래서 원래 몇 번의 재시도가 걸렸든 같은
            // 맵이 돌아온다. N 은 새 것을 뽑는다.
            if (again || clicked == ScreenAction::Restart) BeginRun(seed);
            else if (freshSeed || clicked == ScreenAction::NewMap) BeginRun(NextRandomSeed());
            else if (menu || clicked == ScreenAction::ToMenu) screen = Screen::MainMenu;
            break;
        }
    }

    // 포인터가 올라가 있는 선택지. 없으면 None.
    ScreenAction Game::PointerChoice() const
    {
        if (screen == Screen::Playing || screen == Screen::Generating) return ScreenAction::None;
        // 프레임을 그릴 때 쓴 것과 **같은 배치**다. 그래서 포인터 아래에서 밝아진
        // 줄과 클릭이 떨어지는 줄이 구조적으로 같은 줄이다.
        const ScreenLayout layout = BuildScreenLayout(screen, MakeRunSummary(),
            renderer.Width(), renderer.Height(), balance.uiTextHeight, aimPixel);
        for (const ScreenButton& button : layout.buttons)
        {
            if (button.bounds.Contains(aimPixel)) return button.action;
        }
        return ScreenAction::None;
    }

    // 생성을 예약한다. 카드를 먼저 띄우기 위해 **맵은 여기서 안 만든다.**
    void Game::BeginRun(std::uint32_t newSeed)
    {
        pendingSeed = newSeed;
        screen = Screen::Generating;
    }

    // 지금 물려 있는 것과 절대 같지 않은 새 시드.
    std::uint32_t Game::NextRandomSeed() const
    {
        const std::uint32_t stamp = static_cast<std::uint32_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        return stamp == seed ? stamp + 0x9e3779b9u : stamp;
    }

    // F5 — 밸런스 파일을 다시 읽는다.
    //
    // 바뀐 값을 **세 종류로 나눠서** 보고한다. 지금 적용된 것, 다음 판부터인 것,
    // 다시 시작해야 하는 것. 조용히 적용하는 것과 조용히 무시하는 것은 바깥에서
    // 구별이 안 되고, 그 구별 못 함이 이 기능이 없애려던 것이다.
    void Game::ReloadBalance()
    {
        // **프로세스가 시작할 때 고르는 것과 같은 함수다.** 규칙은 "가장 가까운"이
        // 아니라 "가장 최근"이고, 그 이유는 Balance::FindConfigFile 에 적혀 있다.
        // 예전에는 그 규칙이 여기 손으로 한 번 더 적혀 있었고, 그래서 **시작 경로가
        // 옛 규칙에 남은 것을 아무도 못 봤다** — F5 를 누르기 전까지는 낡은 사본이
        // 도는 게임이었다.
        const std::filesystem::path chosen = Balance::FindConfigFile(executablePath);
        if (chosen.empty())
        {
            PushLog("GameBalance.ini 를 찾지 못했다");
            return;
        }

        const Balance fresh = Balance::Load(chosen);
        if (!fresh.problems.empty())
        {
            // 적용이 아니라 **거절**이다. 맵을 만들 수 없는 설정은 그냥 두면 다음
            // 생성에서 실패 백 번으로 발견된다.
            PushLog("설정을 적용하지 않았다: " + fresh.problems.front());
            return;
        }

        const std::vector<Balance::Difference> moved = balance.DifferencesFrom(fresh);
        if (moved.empty())
        {
            PushLog("설정을 다시 읽었다 — 바뀐 값이 없다");
            return;
        }

        // 둘이 아니라 **세 종류**다. 이 기능의 나머지 전부가 어떤 값이 어느 종류인지를
        // 정직하게 말하는 데 걸려 있다 — 조용히 적용된 변경과 조용히 무시된 변경은
        // 바깥에서 보면 똑같이 느껴지고, 그중 두 번째가 튜닝하는 사람을 실은 낡은
        // 파일이었던 버그를 찾아 헤매게 만든다.
        //
        // **다시 읽기가 닿지 못하는 것의 목록이 이미 한 번 틀렸다.** render_fps 가
        // 루프 앞에서 const 로 잡혀 있었는데 여기 있는 다른 모든 값과 함께 즉시
        // 적용이라고 알려졌고, 그래서 로그는 루프가 다시는 읽지 않는 숫자에 대해
        // "적용됐다"고 말했다. 고친 방법은 목록에 추가하는 것이 아니라 **진짜로
        // 즉시 적용되게 만드는 것**이었고, 그게 이 모든 값의 올바른 모양이다.
        // 아래 목록에 들어갈 자격은 정말로 적용할 수 없는 값에만 있다.
        static const char* const generationKeys[] = {
            "zombie_count", "extra_ammo_rounds", "start_ammo", "minimum_key_path_cost",
            "player_health" };
        // 오디오 장치를 열거나 닫는 것은 소리 나는 중일 수 있는 보이스를 부수는
        // 일이다. 아무도 두 번 튜닝하지 않는 값 하나 때문에 플레이어 몰래 할 일이 아니다.
        static const char* const restartKeys[] = { "audio_enabled" };

        const auto collect = [&moved](const char* const* keys, std::size_t count)
        {
            std::string named;
            for (const Balance::Difference& change : moved)
            {
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (change.key != keys[index]) continue;
                    named += named.empty() ? change.key : ", " + change.key;
                }
            }
            return named;
        };
        const std::string deferred = collect(generationKeys, std::size(generationKeys));
        const std::string restart = collect(restartKeys, std::size(restartKeys));

        balance = fresh;
        statusLog.Configure(balance.uiLogSeconds, balance.uiLogCooldown);
        audio.SetVolumes(balance.masterVolume, balance.sfxVolume, balance.bgmVolume);
        // 소음과 침입 압력은 BreachSystem 에 있고, 그것은 튜닝을 생성 시점에 받았다.
        // **시스템이 아니라 튜닝만 갈아 끼우면** 각 침입구에 이미 쌓인 압력이 남는다.
        // 다시 읽었다고 금 간 창문이 멀쩡해져서는 안 된다.
        breachSystem.Configure(MakeBreachTuning(balance), MakeNoiseTuning(balance));
        // 생성기도 설정을 생성 시점에 받으므로 여기서 다시 만든다. 효과는 다음
        // Reset 부터인데, "다음 판부터"라는 로그 줄이 말하는 것이 그것이다.
        generator = MapGenerator(MakeMapSettings(balance));

        PushLog("설정을 다시 읽었다 — " + std::to_string(moved.size()) + "개 값이 바뀌었다");
        if (!deferred.empty()) PushLog("다음 판부터 적용: " + deferred);
        if (!restart.empty()) PushLog("다시 시작해야 적용: " + restart);
    }

    // 결과 화면이 찍는 기록 한 묶음.
    RunSummary Game::MakeRunSummary() const
    {
        return { runSeconds, shotsFired, shotsHit, kills, seed, grid.seed, grid.seedAttempt };
    }

    // 상태 로그에 한 줄. 같은 문장이 쿨다운 중이면 StatusLog 가 버린다.
    void Game::PushLog(const std::string& text)
    {
        statusLog.Push(text);
    }

    // 세계 어딘가에서 난 소리를 낸다. **들렸으면 true.**
    //
    // 음량을 좀비가 듣는 것과 같은 음향 필드에서 뽑으므로, 로그의 "들린다"가
    // 거리에 대한 추측이 아니라 사실이다.
    bool Game::PlayWorldCue(AudioCue cue, Int2 tile)
    {
        if (!audio.IsOpen()) return false;
        const Int2 listener = ToTile(playerPosition);
        // origin 은 **언제나 플레이어의 타일**이고, PathCost 를 부르는 다른 모든 곳도
        // 그것을 쓴다. 필드는 origin 별로 캐시되므로 여기서 묻는 것은 공짜다.
        // 소리가 난 타일 기준으로 물으면 소음마다 캐시를 버리게 된다.
        const float cost = breachSystem.PathCost(listener, tile, grid);
        if (!std::isfinite(cost)) return false;
        const float reach = std::max(1.0f, balance.audioHearingCost);
        // 거리가 아니라 **비용**이다. 그게 핵심이다 — 이 게임의 중심 규칙은 벽이
        // 소리를 먹는다는 것이고, 지금까지 그 규칙은 좀비의 행동에만 드러났다.
        // 플레이어 자신의 귀를 같은 필드로 잦아들게 하면, **볼 수 없는 규칙이 들을
        // 수 있는 것**이 된다.
        const float volume = std::max(0.0f, 1.0f - cost / reach);
        if (volume <= 0.0f) return false;

        const Vec2 offset = TileCenter(tile) - playerPosition;
        // 소리의 방향과 몸이 향한 방향 사이 각의 사인 값 — 정면이나 정후방이 0,
        // 완전히 왼쪽이 -1, 오른쪽이 1. 기울지도 않고 3차원으로 움직이지도 않는
        // 청자에 대해서는 X3DAudio 도 같은 답을 낸다.
        const float relative = std::atan2(offset.y, offset.x) - playerAngle;
        audio.PlayPositional(cue, volume, std::sin(relative));
        return true;
    }

    // 상태 로그를 늙히고, 침입구가 낸 사건을 문장으로 옮긴다.
    void Game::UpdateLog(float deltaTime)
    {
        statusLog.Advance(deltaTime);

        // 지난 프레임 이후 침입구들이 한 일. 음향 필드가 이미 소리가 거기 닿았다고
        // 판정했으므로, **"들린다"가 거리에 대한 추측이 아니라 말 그대로 사실이다.**
        for (const BreachEvent& event : breachSystem.DrainEvents())
        {
            const bool door = event.kind == BreachKind::SealedDoor;
            switch (event.state)
            {
            case BreachState::Warning:
                PushLog(door ? "문이 흔들리는 소리가 들린다" : "창문이 흔들리는 소리가 들린다");
                break;
            case BreachState::Cracked:
                PushLog(door ? "문이 갈라지는 소리가 들린다" : "창문에 금이 가는 소리가 들린다");
                break;
            case BreachState::Broken:
                PushLog(door ? "문이 부숴졌다" : "창문이 깨졌다");
                PlayWorldCue(door ? AudioCue::DoorBreak : AudioCue::WindowBreak, event.position);
                break;
            default:
                break;
            }
        }
    }

    // 렌더러가 필요로 하는 것 전부를 한 묶음으로.
    //
    // 흔들림과 반동이 **여기서 한 번** 조준점에 접힌다. 그리기와 명중 판정이
    // 같은 값을 받으므로 총알이 조준선 밖으로 갈 수 없다.
    RenderState Game::MakeRenderState(const InputState&) const
    {
        RenderState state;
        state.playerPosition = playerPosition;
        state.playerAngle = playerAngle;

        // 흔들림과 반동을 **여기서 한 번** aimPixel 에 접는다. 누가 읽기 전에.
        // 그리기와 명중 판정이 둘 다 state.aimPixel 을 받으므로, 총알은 플레이어가
        // 보고 있는 조준선 말고 다른 데로 갈 수 없다. 7.x 가 숨은 산탄을 금지하는데,
        // 이것이 그 금지를 약속이 아니라 **구조**로 만드는 장치다 — 오프셋을 다르게
        // 적용할 수 있는 두 번째 자리가 존재하지 않는다.
        //
        // 기준 픽셀은 frameHeight / DefaultHeight 로 배율을 준다. 7.x 의 숫자는
        // 120x80 논리 화면에 대고 쓴 것이라, 지금 게임이 도는 프레임에서 1픽셀
        // 흔들림은 보이지도 않는다.
        const float scale =
            static_cast<float>(renderer.Height()) / static_cast<float>(RaycastRenderer::DefaultHeight);
        // 걷는 사람의 시야는 8자를 그린다 — 좌우 한 주기에 상하 두 주기다.
        // 쌓인 고정 시간 간격으로 굴러가므로 같은 입력이면 매번 같은 곡선이 나온다.
        const float swing = aimSway * scale;
        Vec2 offset{
            std::sin(aimSwayPhase * 6.0f) * swing,
            std::sin(aimSwayPhase * 12.0f) * swing * 0.5f };

        const float kick = balance.weaponRecoilDuration > 0.0f
            ? weaponRecoil / balance.weaponRecoilDuration
            : 0.0f;
        // 위로, 그리고 한쪽 옆으로. 어느 쪽인지는 난수가 아니라 흔들림 곡선에서
        // 가져온다. 그래야 같은 입력을 재현하면 같은 방향으로 튄다.
        offset.y -= kick * balance.aimKickUpPixels * scale;
        offset.x += std::sin(aimSwayPhase * 6.0f) * kick * balance.aimKickSidePixels * scale;

        state.aimOffset = offset;
        state.aimPixel = {
            std::clamp(aimPixel.x + static_cast<int>(std::lround(offset.x)), 0, renderer.Width() - 1),
            std::clamp(aimPixel.y + static_cast<int>(std::lround(offset.y)), 0, renderer.Height() - 1) };
        state.health = health;
        state.ammo = ammo;
        state.hasKey = hasKey;
        state.showDebugMap = showDebugMap;
        state.lookingBehind = lookingBehind;
        state.rollReady = rollCooldown <= 0.0f ? 1.0f : 1.0f - rollCooldown / balance.rollCooldown;
        state.stamina = balance.staminaMax > 0.0f ? stamina / balance.staminaMax : 0.0f;
        state.uiTextHeight = HudTextHeight(renderer.Height(), balance.uiTextHeight);
        // 남은 초를 총이 내려가는 프레임 비율로 접어서 넘긴다. 그래서 렌더러는
        // 시계도 밸런스 파일도 필요 없다.
        state.recoil = kick * balance.weaponRecoilKick;
        state.hitFlash = balance.hitFlashDuration > 0.0f
            ? hitFlash / balance.hitFlashDuration * balance.hitFlashStrength
            : 0.0f;
        return state;
    }

    // HUD 표시와 상태 로그를 프레임 픽셀 위의 글자 덩어리 목록으로.
    std::vector<TextSpan> Game::MakeOverlay(int frameWidth, int frameHeight) const
    {
        const HudState hud{ health, ammo, hasKey, measuredFps, lookingBehind,
            balance.uiTextHeight };
        return BuildHud(hud, statusLog, frameWidth, frameHeight);
    }

    void Game::DrawScreenCard(PixelBuffer& frame) const
    {
        TextRaster::DrawAll(frame, BuildScreenLayout(screen, MakeRunSummary(),
            frame.Width(), frame.Height(), balance.uiTextHeight, aimPixel).spans);
    }

    // 좀비 상태에서 렌더러가 쓸 부분만 뽑는다. 렌더러는 A* 경로도 타이머도 모른다.
    std::vector<RenderActor> Game::MakeRenderActors() const
    {
        std::vector<RenderActor> actors;
        actors.reserve(zombies.size());
        for (const ZombieAgent& zombie : zombies)
        {
            actors.push_back({ zombie.position, zombie.alive, zombie.alert,
                zombie.facing, zombie.stridePhase, zombie.deathProgress });
        }
        return actors;
    }

    // 디버그 탑다운의 안내 경로 — 열쇠까지, 열쇠를 얻었으면 탈출구까지.
    // developerDebug 가 아니면 빈 목록이다.
    std::vector<Int2> Game::MakeGuidePath() const
    {
        if (!developerDebug) return {};
        const Int2 target = hasKey ? grid.exit.origin : grid.keyPosition;
        return AStar::FindPath(grid, ToTile(playerPosition), target);
    }

    // 플레이어 타일에서 편 이동 비용 필드. 원점과 배치가 그대로면 재사용한다.
    const std::vector<float>& Game::TraversalField()
    {
        const Int2 origin = ToTile(playerPosition);
        if (traversalValid && traversalRevision == grid.Revision() && traversalOrigin == origin)
            return traversalField;
        traversalField = AStar::FloodCosts(grid, origin, CostField::Traversal);
        traversalOrigin = origin;
        traversalRevision = grid.Revision();
        traversalValid = true;
        return traversalField;
    }

    // 마우스가 가리키는 타일. 렌더러의 탑다운 배치를 거꾸로 탄다.
    Int2 Game::TopDownCursorTile(int frameWidth, int frameHeight) const
    {
        return RaycastRenderer::MakeTopDownLayout(frameWidth, frameHeight, grid).TileAt(aimPixel);
    }

    // 탑다운 위에 겹치는 것 전부.
    TopDownOverlay Game::MakeTopDownOverlay(int frameWidth, int frameHeight)
    {
        TopDownOverlay overlay;
        overlay.guidePath = MakeGuidePath();
        if (!developerDebug) return overlay;

        const Int2 cursor = TopDownCursorTile(frameWidth, frameHeight);
        overlay.hasCursor = grid.Contains(cursor);
        overlay.cursorTile = cursor;

        for (const ZombieAgent& zombie : zombies)
        {
            if (!ChasesPlayer(zombie)) continue;
            std::vector<Int2> remaining = RemainingPath(zombie);
            if (remaining.empty()) continue;
            overlay.chasePaths.push_back(std::move(remaining));
        }

        overlay.costScale = CostOverlayScale(balance, debugCostOverlay);
        switch (debugCostOverlay)
        {
        case CostOverlay::Traversal:
            overlay.costField = TraversalField();
            // 안내 경로와 같은 청록 계열로 시작해서 붉게 간다. 걸어서 가는 이야기다.
            overlay.costNear = { 70, 190, 150 };
            overlay.costFar = { 210, 80, 60 };
            break;
        case CostOverlay::Acoustic:
            // **새로 계산하지 않는다.** 침입구가 압력을 받을지 판정할 때 쓴 그 배열이다.
            overlay.costField = breachSystem.AcousticField(ToTile(playerPosition), grid);
            // 가까운 곳이 밝고 따뜻하다가 식는다. 소리가 잦아드는 그림이다.
            overlay.costNear = { 240, 196, 96 };
            overlay.costFar = { 62, 44, 96 };
            break;
        case CostOverlay::None:
            break;
        }
        return overlay;
    }

    // 탑다운 좌상단에 적을 숫자들.
    DebugReadout Game::MakeDebugReadout(int frameWidth, int frameHeight)
    {
        DebugReadout readout;
        const std::vector<Int2> guide = MakeGuidePath();
        readout.hasGuide = !guide.empty();
        readout.guideCost = AStar::PathCost(grid, guide);
        readout.guideTiles = static_cast<int>(guide.size());
        readout.guideToExit = hasKey;
        readout.keyCost = keyPathCost;
        readout.keyCostMinimum = balance.minimumKeyPathCost;

        for (const ZombieAgent& zombie : zombies)
        {
            if (!ChasesPlayer(zombie)) continue;
            ++readout.chasers;
            const std::vector<Int2> remaining = RemainingPath(zombie);
            if (remaining.empty()) continue;
            const float cost = RemainingCost(grid, remaining);
            if (readout.hasNearestChase && cost >= readout.nearestChaseCost) continue;
            readout.hasNearestChase = true;
            readout.nearestChaseCost = cost;
            readout.nearestChaseTiles = static_cast<int>(remaining.size());
        }

        // 커서 밑의 타일. **지도에 테두리를 두르는 쪽과 같은 함수에서 받는다** —
        // 그래서 여기 적힌 좌표가 화면에서 밝아진 칸과 언제나 같은 칸이다.
        const Int2 cursor = TopDownCursorTile(frameWidth, frameHeight);
        if (grid.Contains(cursor))
        {
            const std::vector<float>& walked = TraversalField();
            const std::vector<float>& heard =
                breachSystem.AcousticField(ToTile(playerPosition), grid);
            const std::size_t index =
                static_cast<std::size_t>(cursor.y * grid.Width() + cursor.x);
            const float unreachable = std::numeric_limits<float>::infinity();
            readout.hasCursor = true;
            readout.cursorTile = cursor;
            readout.cursorTraversal = index < walked.size() ? walked[index] : unreachable;
            readout.cursorAcoustic = index < heard.size() ? heard[index] : unreachable;
        }

        readout.overlay = debugCostOverlay;
        readout.overlayScale = CostOverlayScale(balance, debugCostOverlay);
        return readout;
    }

    // `[` `]` 로 생성 스냅샷을 넘겨보는 중인가. 마지막 칸이 LIVE 다.
    bool Game::IsInspectingGeneration() const
    {
        return developerDebug && debugTopDown && generationStep < generationTrace.size();
    }
}
