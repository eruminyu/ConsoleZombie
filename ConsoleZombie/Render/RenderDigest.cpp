#include "RenderDigest.h"

#include <AI/AStar.h>
#include <Platform/Terminal.h>
#include <World/MapGenerator.h>

#include <string>
#include <vector>

namespace Zombie
{
    namespace
    {
        // FNV-1a. **순서가 값에 들어간다.** 그래서 맞는 픽셀을 틀린 자리에 그리는
        // 변경도 이 값이 잡는다.
        constexpr std::uint64_t Offset = 1469598103934665603ull;
        constexpr std::uint64_t Prime = 1099511628211ull;

        void Fold(std::uint64_t& digest, std::uint8_t byte)
        {
            digest ^= byte;
            digest *= Prime;
        }

        void Fold(std::uint64_t& digest, const PixelBuffer& frame)
        {
            for (const Rgb& pixel : frame.Pixels())
            {
                Fold(digest, pixel.r);
                Fold(digest, pixel.g);
                Fold(digest, pixel.b);
            }
        }

        void Fold(std::uint64_t& digest, const std::string& text)
        {
            for (const char character : text) Fold(digest, static_cast<std::uint8_t>(character));
        }

        // 목표에서 두 타일 못 미친 자리. 실제 경로를 따라가서 잡으므로 그 타일이
        // 반드시 바닥이다. 뒤돌아보기 검사가 쓰는 것과 같은 수법이다.
        void AddApproach(std::vector<Vec2>& viewpoints, const Grid& grid, Int2 target)
        {
            const std::vector<Int2> route = AStar::FindPath(grid, grid.playerSpawn, target);
            if (route.size() >= 3) viewpoints.push_back(TileCenter(route[route.size() - 3]));
        }
    }

    std::uint64_t ComputeRenderDigest()
    {
        std::uint64_t digest = Offset;
        MapGenerator generator{ MapGenerationSettings{} };

        // 시드 하나가 아니라 넷이다. 특정 타일 배치에서만 드러나는 그리기 버그는
        // 맵 하나 뒤에 숨는다.
        for (const std::uint32_t seed : { 1u, 7u, 4242u, 20260826u })
        {
            Grid grid = generator.Generate(seed);

            std::vector<Vec2> viewpoints = { TileCenter(grid.playerSpawn) };
            AddApproach(viewpoints, grid, grid.keyPosition);
            AddApproach(viewpoints, grid, grid.exit.origin);
            if (!grid.ammoPickups.empty()) AddApproach(viewpoints, grid, grid.ammoPickups.front().position);
            // 갓 생성된 맵은 문이 전부 닫혀 있고 창문이 전부 멀쩡하다. 그래서
            // **흔적 그리기가 한 번도 안 돈다.** 각각 하나씩 열어서 거기까지 닿는다.
            if (!grid.doorways.empty())
            {
                const TilePair opened = grid.doorways.front().tiles;
                grid.Set(opened.origin, Tile::DoorOpen);
                grid.Set(opened.Second(), Tile::DoorOpen);
                AddApproach(viewpoints, grid, opened.origin);
            }
            if (!grid.breachSpawns.empty())
            {
                grid.Set(grid.breachSpawns.front().breachPosition, Tile::WindowBroken);
            }

            // 예전에는 여기 배우가 **전부 살아 있고 깨어 있었다.** 그래서 스프라이트의
            // 두 갈래가 통째로 오라클 밖에 있었다 — 눈이 없는 차분한 실루엣, 그리고
            // 시체를 건너뛰지 않고 그리게 된 뒤로는 시체 경로 전체.
            //
            // **연달아 네 번의 그리기 작업이 정확히 이 모양의 사각지대를 달고 출하됐다.**
            // 그래서 이제 변경과 함께 섞임이 들어간다.
            std::vector<RenderActor> actors;
            for (std::size_t which = 0; which < grid.zombieSpawns.size(); ++which)
            {
                RenderActor actor;
                actor.position = TileCenter(grid.zombieSpawns[which].position);
                actor.alive = (which % 4) != 1;
                actor.alert = (which % 3) != 0;
                // 돌아선 것, 보폭 중간, 반쯤 쓰러진 것 — 마스크가 실제로 읽는
                // 인자들이다. 기본값으로 두면 걸음걸이가 **한 자세로 얼어붙은 채**
                // 접힌다.
                actor.facing = static_cast<float>(which) * 0.9f;
                actor.stridePhase = static_cast<float>(which % 5) * 0.2f;
                actor.deathProgress = actor.alive
                    ? 0.0f : 0.25f * static_cast<float>(which % 5);
                actors.push_back(actor);
            }

            RaycastRenderer renderer(160, 120);
            for (std::size_t index = 0; index < viewpoints.size(); ++index)
            {
                for (int step = 0; step < 6; ++step)
                {
                    RenderState state;
                    state.playerPosition = viewpoints[index];
                    state.playerAngle = 0.19f + static_cast<float>(step) * 1.047f;
                    state.aimPixel = { 40 + step * 13, 30 + step * 9 };
                    state.health = 3 - static_cast<int>(index % 3);
                    state.ammo = static_cast<int>(index % 4);
                    state.hasKey = (index % 2) == 0;
                    state.rollReady = 0.25f * static_cast<float>(step % 4);
                    state.stamina = 0.2f * static_cast<float>(step % 5);
                    state.uiTextHeight = 9 + static_cast<int>(index % 4);
                    // 쉬는 상태, 반동 중간, 최대 반동. 이게 없으면 반동 경로가
                    // 여기서 한 번도 안 돌고 무기가 언제나 쉬는 자세로만 접힌다.
                    // 움직이는 절반이 오라클 밖에 남는다.
                    state.recoil = 0.025f * static_cast<float>(step % 3);
                    // 게임이 여기 오기 전에 aimPixel 에 접어 넣는 흔들림. 렌더러는
                    // 그것을 무기를 움직이는 데만 쓰지만, 0으로 두면 총이 한 자리에
                    // 고정된 채 접히고 흔들리는 절반이 오라클 밖으로 나간다.
                    // 반동이 빠져 있던 것과 같은 함정이다.
                    // 없음, 중간, 거의 최대. **비네트는 0에서 일찍 반환한다.**
                    // 그래서 이것을 빼면 프레임이 언제나 안 맞은 상태로만 접히고
                    // 효과 전체가 오라클 밖에 남는다. 연달아 두 번의 그리기 작업이
                    // 정확히 여기 빠졌다.
                    state.hitFlash = 0.22f * static_cast<float>(step % 3);
                    state.aimOffset = {
                        static_cast<float>(static_cast<int>(step) % 5) - 2.0f,
                        1.0f - static_cast<float>(static_cast<int>(index) % 3) };
                    // 렌더러가 가진 모든 스위치의 **양쪽 값** — 디버그 맵,
                    // 뒤돌아보기 카메라, 그리고 평범한 정면 시야.
                    state.showDebugMap = (step % 2) == 0;
                    state.lookingBehind = (step % 3) == 0;
                    Fold(digest, renderer.Render(grid, state, actors));

                    // 탑다운이 겹쳐 그리는 것 **전부**를 통과시킨다. 안내 경로만
                    // 넣던 시절에는 히트맵도 추적 경로도 오라클 밖이었을 것이고,
                    // 이 저장소는 정확히 그 모양의 사각지대를 네 번 출하했다.
                    TopDownOverlay overlay;
                    overlay.guidePath =
                        AStar::FindPath(grid, ToTile(state.playerPosition), grid.keyPosition);
                    // 좀비마다 자기 자리에서 플레이어까지. 실제 게임에서 그리는 것과
                    // 같은 모양의 목록이다 — 여러 개가 한 점으로 모인다.
                    for (std::size_t which = 0; which < grid.zombieSpawns.size(); which += 3)
                    {
                        std::vector<Int2> chase = AStar::FindPath(grid,
                            grid.zombieSpawns[which].position, ToTile(state.playerPosition));
                        if (!chase.empty()) overlay.chasePaths.push_back(std::move(chase));
                    }
                    // 히트맵 세 상태 — 꺼짐, 이동, 음향. 스케일도 같이 돌린다.
                    // **꺼짐만 접으면 칠하는 코드 전체가 오라클 밖에 남는다.**
                    const int field = step % 3;
                    if (field != 0)
                    {
                        overlay.costField = AStar::FloodCosts(grid, ToTile(state.playerPosition),
                            field == 1 ? CostField::Traversal : CostField::Acoustic);
                        overlay.costScale = field == 1 ? 40.0f : 20.0f;
                        overlay.costNear = field == 1 ? Rgb{ 70, 190, 150 } : Rgb{ 240, 196, 96 };
                        overlay.costFar = field == 1 ? Rgb{ 210, 80, 60 } : Rgb{ 62, 44, 96 };
                    }
                    // 커서 테두리도. **지도 안과 지도 밖 양쪽**을 통과시킨다 —
                    // 안쪽만 접으면 "격자 밖이면 안 그린다"는 가드가 오라클 밖이다.
                    overlay.hasCursor = (step % 2) == 0;
                    overlay.cursorTile = { 3 + step * 9, 5 + step * 7 };
                    Fold(digest, renderer.RenderTopDown(grid, state, actors, overlay, (step % 2) == 0));
                }
            }

            // 터미널 조립기도 함께. **프레임의 배수가 아닌 크기**로 부른다. 그래야
            // 샘플링 산술이 건너뛰이지 않고 실제로 돈다.
            RenderState plain;
            plain.playerPosition = viewpoints.front();
            plain.playerAngle = 0.7f;
            const PixelBuffer& frame = renderer.Render(grid, plain, actors);
            Fold(digest, TerminalSession::ComposeFrame(frame, 97, 41));
            Fold(digest, TerminalSession::ComposeFrame(frame, 200, 90, 7, 3));
        }
        return digest;
    }
}
