#include "RaycastRenderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <iterator>
#include <numeric>

namespace Zombie
{
    RaycastRenderer::RaycastRenderer(int width, int height)
        : frameWidth(std::clamp(width, MinWidth, MaxWidth)),
          frameHeight(std::clamp(height, MinHeight, MaxHeight)),
          frame(std::clamp(width, MinWidth, MaxWidth), std::clamp(height, MinHeight, MaxHeight)),
          depthBuffer(static_cast<std::size_t>(std::clamp(width, MinWidth, MaxWidth)), 1000.0f)
    {
    }

    namespace
    {
        // 화면 열 하나의 카메라 평면 오프셋 — 왼쪽 끝이 -0.5, 오른쪽 끝이 +0.5.
        //
        // 세 패스가 이것을 통과해 레이를 쏜다 — 벽, 개구부 흔적, 탈출구 바닥 —
        // 그리고 셋이 **정확히** 같아야 한다. 예전에는 DrawExitZone() 이 주석으로
        // 그렇다고 말했다. 지금은 다를 수가 없다.
        float CameraOffset(int column, int frameWidth)
        {
            return static_cast<float>(column) / static_cast<float>(frameWidth - 1) - 0.5f;
        }

        // 이만큼 떨어진 것에 빛이 얼마나 닿는가. **장면 전체에 곡선 하나**라서
        // 벽과 그 앞의 바닥이 같이 어두워진다. 바닥은 표면에 따라 최소값만 다르다 —
        // 탈출구 구역은 자기 색을 조금 더 지킨다.
        float DistanceShade(float distance, float minimum)
        {
            return std::clamp(1.25f / (0.35f + distance * 0.14f), minimum, 1.0f);
        }

        // 위에서 내려다보는 두 화면이 그릴 수 있는 타일 전부를, 한 번만 나열한 것.
        //
        // **값은 서로 조금 다르다.** 전체 탑다운 지도는 구석에 박힌 미니맵보다 밝고,
        // 두 벌은 서로에게서 유도한 것이 아니라 손으로 따로 맞췄다. 그래서 팔레트는
        // 떨어져 있고 **매핑만 공유한다.** 값까지 합치면 플레이어가 보는 것이
        // 바뀌는데, 그건 정리가 하려는 일이 아니다.
        struct TilePalette
        {
            Rgb solid;
            Rgb floor;
            Rgb doorClosed;
            Rgb doorWarning;
            Rgb doorCracked;
            Rgb doorSealed;
            Rgb doorSealedWarning;
            Rgb doorSealedCracked;
            Rgb doorBroken;
            Rgb doorOpen;
            Rgb windowIntact;
            Rgb windowWarning;
            Rgb windowCracked;
            Rgb windowBroken;
            Rgb exitLocked;
            Rgb exitOpen;
        };

        constexpr TilePalette TopDownPalette
        {
            { 18, 20, 23 }, { 74, 72, 66 }, { 156, 89, 42 }, { 225, 137, 34 },
            { 190, 48, 38 }, { 88, 98, 64 }, { 130, 140, 82 }, { 162, 116, 72 },
            { 86, 60, 43 }, { 105, 74, 51 }, { 37, 139, 174 }, { 48, 201, 211 },
            { 139, 80, 170 }, { 31, 82, 92 }, { 199, 43, 36 }, { 42, 189, 83 }
        };

        constexpr TilePalette MinimapPalette
        {
            { 12, 12, 13 }, { 69, 67, 61 }, { 145, 82, 40 }, { 218, 129, 31 },
            { 181, 45, 35 }, { 84, 94, 60 }, { 126, 136, 78 }, { 158, 112, 68 },
            { 82, 57, 41 }, { 103, 71, 48 }, { 35, 130, 160 }, { 44, 190, 198 },
            { 130, 74, 159 }, { 28, 76, 86 }, { 194, 42, 36 }, { 41, 186, 81 }
        };

        // 플레이어가 실제로 보는 화면에서 벽 한 칸이 무슨 색이고 창살을 쓰는가.
        //
        // 위의 두 팔레트가 내려다보는 화면의 것이라면 이 표는 1인칭 화면의 것이다.
        // 예전에는 이 자리만 if / else if 아홉 단이었다. 같은 파일 안에서 같은 종류의
        // 사실이 한쪽은 표, 한쪽은 사슬로 적혀 있었다.
        //
        // 막힌 문은 같은 문에 창살만 얹는다. 목재 색은 그 문이 어떤 상태인지를
        // 말하고 — 멀쩡한지, 흔들리는지, 갈라졌는지 — 창살이 "이건 안 열린다"를
        // 말한다. 색 하나로 둘 다 말하려면 색 여섯 개가 서로를 잡아먹는다.
        struct WallSkin
        {
            Rgb colour;
            bool barred;
        };

        // 통과하는 타일들은 벽으로 그려질 일이 없으므로 평범한 벽색으로 채워 둔다.
        // 예전 if 사슬의 맨 끝 기본값과 같은 값이라, 표가 사슬보다 답을 더 많이
        // 하는 것은 아니다.
        constexpr Rgb PlainWall{ 106, 111, 112 };

        constexpr WallSkin WallSkins[] = {
            /* Wall              */ { PlainWall,          false },
            /* Floor             */ { PlainWall,          false },
            /* DoorClosed        */ { { 111,  68,  38 },  false },
            /* DoorWarning       */ { { 176, 103,  31 },  false },
            /* DoorCracked       */ { { 143,  48,  35 },  false },
            /* DoorBroken        */ { PlainWall,          false },
            /* DoorOpen          */ { PlainWall,          false },
            /* DoorSealed        */ { { 111,  68,  38 },  true  },
            /* DoorSealedWarning */ { { 176, 103,  31 },  true  },
            /* DoorSealedCracked */ { { 143,  48,  35 },  true  },
            /* WindowIntact      */ { {  40, 104, 123 },  false },
            /* WindowWarning     */ { {  46, 148, 162 },  false },
            /* WindowCracked     */ { { 105,  68, 126 },  false },
            /* WindowBroken      */ { PlainWall,          false },
            /* ExitLocked        */ { PlainWall,          false },
            /* ExitOpen          */ { PlainWall,          false }
        };

        static_assert(std::size(WallSkins) == static_cast<std::size_t>(Tile::ExitOpen) + 1,
            "Tile 이 하나 늘었으면 이 표에도 한 줄 늘어야 한다");

        // 타일 하나를 팔레트에서 찾는다. 매핑은 공유하고 값은 팔레트마다 다르다.
        Rgb TileColor(Tile tile, const TilePalette& palette)
        {
            switch (tile)
            {
            case Tile::Floor: return palette.floor;
            case Tile::DoorClosed: return palette.doorClosed;
            case Tile::DoorWarning: return palette.doorWarning;
            case Tile::DoorCracked: return palette.doorCracked;
            case Tile::DoorSealed: return palette.doorSealed;
            case Tile::DoorSealedWarning: return palette.doorSealedWarning;
            case Tile::DoorSealedCracked: return palette.doorSealedCracked;
            case Tile::DoorBroken: return palette.doorBroken;
            case Tile::DoorOpen: return palette.doorOpen;
            case Tile::WindowIntact: return palette.windowIntact;
            case Tile::WindowWarning: return palette.windowWarning;
            case Tile::WindowCracked: return palette.windowCracked;
            case Tile::WindowBroken: return palette.windowBroken;
            case Tile::ExitLocked: return palette.exitLocked;
            case Tile::ExitOpen: return palette.exitOpen;
            default: return palette.solid;
            }
        }
    }

    // 프레임 크기를 바꾼다. 이미 맞으면 아무 일도 안 하므로 매 프레임 불러도 된다.
    //
    // 사용자가 창을 최대화하는 순간 해상도가 따라가는 것이 이 함수다.
    // 자동 최대화가 불가능하다는 결론의 대응책이다.
    void RaycastRenderer::Resize(int width, int height)
    {
        const int wantWidth = std::clamp(width, MinWidth, MaxWidth);
        const int wantHeight = std::clamp(height, MinHeight, MaxHeight);
        if (wantWidth == frameWidth && wantHeight == frameHeight) return;
        frameWidth = wantWidth;
        frameHeight = wantHeight;
        frame = PixelBuffer(frameWidth, frameHeight);
        depthBuffer.assign(static_cast<std::size_t>(frameWidth), 1000.0f);
    }

    // 1인칭 화면 한 장. **격자·상태·배우의 순수 함수다.**
    //
    // 그리는 순서가 규칙이다 — 하늘/바닥 → 벽 → 탈출구 → 열쇠·탄약 → 좀비 →
    // 개구부 흔적 → 무기·게이지 → 피격 → 미니맵 → 조준선.
    // **개구부 흔적이 좀비보다 뒤인 것이 특히 중요하다.** 순서를 바꾸면 좀비가
    // 창문 턱 위에 떠 보이고, 턱이 다리를 가려야 넘어야 할 높이가 읽힌다.
    const PixelBuffer& RaycastRenderer::Render(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors)
    {
        for (int y = 0; y < frameHeight; ++y)
        {
            const bool ceiling = y < frameHeight / 2;
            const float gradient = ceiling
                ? 0.38f + 0.22f * static_cast<float>(y) / (frameHeight / 2)
                : 0.45f - 0.30f * static_cast<float>(y - frameHeight / 2) / (frameHeight / 2);
            const Rgb base = ceiling ? Rgb{ 24, 27, 31 } : Rgb{ 49, 43, 39 };
            frame.FillRect(0, y, frameWidth, 1, Rgb::Scale(base, gradient + 0.45f));
        }

        for (int x = 0; x < frameWidth; ++x)
        {
            const float camera = CameraOffset(x, frameWidth);
            const float rayAngle = state.ViewAngle() + camera * FieldOfView;
            const RayHit hit = CastRay(grid, state.playerPosition, rayAngle);
            const float correctedDistance = std::max(0.05f, hit.distance * std::cos(rayAngle - state.ViewAngle()));
            depthBuffer[static_cast<std::size_t>(x)] = correctedDistance;
            const int wallHeight = std::min(frameHeight * 2, static_cast<int>(frameHeight / correctedDistance));
            const int top = frameHeight / 2 - wallHeight / 2;
            const int bottom = top + wallHeight;

            const WallSkin& skin = WallSkins[static_cast<std::size_t>(hit.tile)];
            Rgb wallColor = skin.colour;

            if (skin.barred)
            {
                // 문 위에 세로 창살 다섯. **화면 열이 아니라 타일을 가로지르는
                // 위치**로 잰다. 그래야 플레이어가 움직여도 창살이 문 위를
                // 미끄러지지 않고 문에 붙어 있는다.
                //
                // 이 렌더러가 그리는 단위가 어차피 화면 열 하나 전체이고 창살은
                // 바닥에서 천장까지 이어지므로, **창살은 공짜다** — 다른 모든 타일이
                // 하는 것과 똑같은 색 결정 한 번이다.
                const float across = hit.textureX * 5.0f;
                if (across - std::floor(across) < 0.34f) wallColor = { 58, 62, 64 };
            }

            const float distanceShade = DistanceShade(correctedDistance, 0.20f);
            const int textureColumn = static_cast<int>(hit.textureX * 8.0f);
            for (int y = std::max(0, top); y < std::min(frameHeight, bottom); ++y)
            {
                const int textureRow = wallHeight > 0 ? (y - top) * 8 / wallHeight : 0;
                float textureShade = ((textureColumn + textureRow) % 2 == 0) ? 1.0f : 0.82f;
                if (hit.side) textureShade *= 0.74f;
                frame.Set(x, y, Rgb::Scale(wallColor, distanceShade * textureShade));
            }
        }

        DrawExitZone(grid, state);
        DrawKey(grid, state);
        DrawAmmoPickups(grid, state);
        DrawSprites(grid, state, actors);
        DrawOpeningRemnants(grid, state);
        DrawWeaponAndHud(state);
        // 세계와 총 **위**, 미니맵과 조준선 **아래**. 플레이어가 맞았으니 손에 든
        // 것도 같이 붉어진다. 다만 조준선은 조준 도구이고 미니맵은 디버그 표시라,
        // 둘 중 어느 것을 가려도 피드백이 벌이 된다.
        DrawHitFlash(state.hitFlash);
        if (state.showDebugMap) DrawMinimap(grid, state, actors);

        // 뒤돌아보는 동안에는 조준선도 없다. 총과 같은 이유다 — 저 뒤에는 플레이어가
        // 맞혀도 되는, 겨눌 것이 없다.
        if (state.lookingBehind) return frame;

        const int aimX = std::clamp(state.aimPixel.x, 2, frameWidth - 3);
        const int aimY = std::clamp(state.aimPixel.y, 2, frameHeight - 3);
        const Rgb crosshair{ 224, 211, 169 };
        frame.Set(aimX - 2, aimY, crosshair);
        frame.Set(aimX + 2, aimY, crosshair);
        frame.Set(aimX, aimY - 2, crosshair);
        frame.Set(aimX, aimY + 2, crosshair);
        frame.Set(aimX, aimY - 1, crosshair);
        frame.Set(aimX, aimY, crosshair);
        return frame;
    }

    // 탑다운 지도를 프레임 가운데에 최대 정수배로 앉힌다.
    //
    // 정수배인 것은 타일 경계가 픽셀 경계와 어긋나면 격자가 물결치기 때문이다.
    // **그리기와 커서 되돌리기가 이 함수 하나를 부른다.**
    TopDownLayout RaycastRenderer::MakeTopDownLayout(int frameWidth, int frameHeight, const Grid& grid)
    {
        TopDownLayout layout;
        layout.scale = std::max(1, std::min(frameWidth / grid.Width(), frameHeight / grid.Height()));
        layout.originX = (frameWidth - grid.Width() * layout.scale) / 2;
        layout.originY = (frameHeight - grid.Height() * layout.scale) / 2;
        return layout;
    }

    // 디버그 탑다운 화면. F2 로 들어가고 F3 로 3D 와 오간다.
    //
    // 생성 스냅샷을 넘겨볼 때는 배우도 겹쳐 그리는 것도 없이 지형만 그린다.
    const PixelBuffer& RaycastRenderer::RenderTopDown(
        const Grid& grid,
        const RenderState& state,
        const std::vector<RenderActor>& actors,
        const TopDownOverlay& overlay,
        bool showLiveState)
    {
        frame.Clear({ 7, 8, 10 });
        const TopDownLayout layout = MakeTopDownLayout(frameWidth, frameHeight, grid);
        const int scale = layout.scale;
        const int originX = layout.originX;
        const int originY = layout.originY;

        // 히트맵은 타일 위에 얹는 것이 아니라 **타일 색에 섞는다.** 얹으면 문과
        // 창문이 사라져서, 비용이 왜 그 모양인지를 설명하는 바로 그 지형이 안 보인다.
        const bool heatMap = !overlay.costField.empty()
            && overlay.costField.size() >= static_cast<std::size_t>(grid.Width() * grid.Height())
            && overlay.costScale > 0.0f;
        for (int y = 0; y < grid.Height(); ++y)
        {
            for (int x = 0; x < grid.Width(); ++x)
            {
                Rgb color = TileColor(grid.Get({ x, y }), TopDownPalette);
                if (heatMap)
                {
                    const float cost = overlay.costField[static_cast<std::size_t>(y * grid.Width() + x)];
                    // 무한대와 스케일 초과는 안 칠한다. 칠하지 않은 곳이 "여기까지는
                    // 안 온다"는 뜻이고, 그 말을 하려면 색이 없어야 한다.
                    if (std::isfinite(cost) && cost <= overlay.costScale)
                    {
                        const float t = std::clamp(cost / overlay.costScale, 0.0f, 1.0f);
                        // 이름이 low/high 인 것은 취향이 아니다. windows.h 가
                        // near 와 far 를 매크로로 들고 있다.
                        const auto mix = [t](std::uint8_t low, std::uint8_t high)
                        {
                            return static_cast<float>(low) + (static_cast<float>(high)
                                - static_cast<float>(low)) * t;
                        };
                        const auto blend = [](float ramp, std::uint8_t base)
                        {
                            // 지형을 완전히 덮지 않는다. 3 대 2 면 벽·문·창문의
                            // 구분이 남으면서 비용의 결이 읽힌다.
                            return static_cast<std::uint8_t>(std::clamp(
                                ramp * 0.6f + static_cast<float>(base) * 0.4f, 0.0f, 255.0f));
                        };
                        color = { blend(mix(overlay.costNear.r, overlay.costFar.r), color.r),
                            blend(mix(overlay.costNear.g, overlay.costFar.g), color.g),
                            blend(mix(overlay.costNear.b, overlay.costFar.b), color.b) };
                    }
                }
                frame.FillRect(originX + x * scale, originY + y * scale, scale, scale, color);
            }
        }

        for (const LeafRegion& leaf : grid.leaves)
        {
            // 역할별 색. 디버그 지도에서 감염 공간이 바로 보이게.
            const Rgb partitionColor = leaf.role == LeafRole::Infected ? Rgb{ 168, 46, 62 }
                : leaf.role == LeafRole::Reserved ? Rgb{ 74, 74, 82 }
                : Rgb{ 116, 59, 143 };
            const int left = originX + leaf.region.x * scale;
            const int top = originY + leaf.region.y * scale;
            const int right = originX + leaf.region.Right() * scale - 1;
            const int bottom = originY + leaf.region.Bottom() * scale - 1;
            frame.Line(left, top, right, top, partitionColor);
            frame.Line(left, bottom, right, bottom, partitionColor);
            frame.Line(left, top, left, bottom, partitionColor);
            frame.Line(right, top, right, bottom, partitionColor);
        }

        // 추적 경로가 **먼저**다. 안내 경로가 그 위에 와야 둘이 겹친 칸에서
        // 청록색이 이긴다 — 플레이어가 가야 하는 길은 좀비 열둘 밑에 묻히면 안 된다.
        for (const std::vector<Int2>& chase : overlay.chasePaths)
        {
            for (std::size_t step = 0; step < chase.size(); ++step)
            {
                const Int2 center = layout.CenterOf(chase[step]);
                // 첫 칸은 그 좀비가 지금 향하고 있는 타일이다. 밝게 두면 경로를
                // 따라 내려가는 커서가 보이고, 그게 A* 가 다시 계산되기 전까지
                // 좀비가 무엇을 하고 있는지다.
                frame.Set(center.x, center.y, step == 0 ? Rgb{ 255, 138, 96 } : Rgb{ 150, 46, 40 });
            }
        }

        for (const Int2 point : overlay.guidePath)
        {
            frame.Set(
                originX + point.x * scale + scale / 2,
                originY + point.y * scale + scale / 2,
                { 48, 220, 226 });
        }

        const auto drawMarker = [this, originX, originY, scale](Vec2 position, Rgb color)
        {
            const Int2 tile = ToTile(position);
            frame.FillRect(
                originX + tile.x * scale,
                originY + tile.y * scale,
                scale,
                scale,
                color);
        };

        if (!(grid.keyPosition == Int2{}) && grid.Contains(grid.keyPosition))
            drawMarker(TileCenter(grid.keyPosition), { 244, 190, 43 });

        if (showLiveState)
        {
            for (const RenderActor& actor : actors)
                if (actor.alive) drawMarker(actor.position, actor.alert ? Rgb{ 231, 43, 34 } : Rgb{ 142, 165, 100 });
            drawMarker(state.playerPosition, { 238, 237, 215 });
            const int playerX = originX + static_cast<int>(state.playerPosition.x * scale);
            const int playerY = originY + static_cast<int>(state.playerPosition.y * scale);
            frame.Line(
                playerX,
                playerY,
                playerX + static_cast<int>(std::cos(state.playerAngle) * scale * 3),
                playerY + static_cast<int>(std::sin(state.playerAngle) * scale * 3),
                { 238, 237, 215 });
        }
        else
        {
            for (const ZombieSpawn& spawn : grid.zombieSpawns)
                drawMarker(TileCenter(spawn.position), { 211, 34, 29 });
            if (!(grid.playerSpawn == Int2{}))
                drawMarker(TileCenter(grid.playerSpawn), { 238, 237, 215 });
        }

        // 마지막이다. 커서는 세계의 일부가 아니라 **세계를 가리키는 것**이라
        // 무엇도 그것을 가려서는 안 된다.
        //
        // 칠하지 않고 테두리만 두르는 이유는, 가리키는 칸이 무엇인지가 가려지면
        // 안 되기 때문이다. 비용을 묻고 있는 그 타일이 문인지 창문인지 벽인지는
        // 숫자가 왜 그 값인지에 대한 답의 절반이다.
        if (overlay.hasCursor && grid.Contains(overlay.cursorTile))
        {
            const int left = originX + overlay.cursorTile.x * scale;
            const int top = originY + overlay.cursorTile.y * scale;
            const int right = left + scale - 1;
            const int bottom = top + scale - 1;
            constexpr Rgb ink{ 250, 250, 236 };
            frame.Line(left, top, right, top, ink);
            frame.Line(left, bottom, right, bottom, ink);
            frame.Line(left, top, left, bottom, ink);
            frame.Line(right, top, right, bottom, ink);
        }

        return frame;
    }

    // 조준선이 맞히는 좀비의 색인, 없으면 -1.
    //
    // **그리기와 같은 상자, 같은 마스크, 같은 가림 판정을 쓴다.** 기획 605행이고,
    // 예전에 이것들이 갈라져서 보이는데 못 쏘는 좀비가 있었다.
    int RaycastRenderer::HitTestZombie(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors) const
    {
        int bestIndex = -1;
        float bestDistance = 1000.0f;
        for (std::size_t index = 0; index < actors.size(); ++index)
        {
            // **시체는 표적이 아니다.** 총알이 그 뒤의 무엇이든 향해 계속 간다.
            // 시체를 그냥 눕혀 둘 수 있는 이유가 그것이다.
            if (!actors[index].alive) continue;
            const ZombieSprite shape = MakeZombieSprite(actors[index], state);
            if (!shape.visible) continue;
            const float distance = shape.distance;
            const float u = (state.aimPixel.x - shape.left) / shape.width;
            const float v = (state.aimPixel.y - shape.top) / shape.height;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f || !IsZombiePixel(u, v, shape)) continue;
            // 그리기가 하는 열 검사와 **글자 그대로 같은 식**이고, 그래야만 한다.
            // 예전에는 여기가 다르게 쓰인 두 번째 레이캐스트였다 — 시야 각도가 아닌
            // 플레이어 각도, 폭-1 이 아닌 폭, 깊이 버퍼가 들고 있는 수직 거리가 아닌
            // 생거리. 벽 모서리 근처에서 둘이 갈라졌고, **보이는 좀비가 못 쏘는
            // 좀비**였다.
            const int column = std::clamp(state.aimPixel.x, 0, frameWidth - 1);
            const float camera = CameraOffset(column, frameWidth);
            const RayHit wall = CastRay(grid, state.playerPosition, state.ViewAngle() + camera * FieldOfView);
            const float corrected = std::max(0.05f, wall.distance * std::cos(camera * FieldOfView));
            if (distance >= corrected + 0.1f) continue;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = static_cast<int>(index);
            }
        }
        return bestIndex;
    }

    // 조준선이 처음 닿는 환경 타일. 창문 사격 판정이 이것을 쓴다.
    EnvironmentHit RaycastRenderer::HitTestEnvironment(const Grid& grid, const RenderState& state) const
    {
        // HitTestZombie 와 같은 조준 각도. 아니면 겨눈 좀비를 빗나간 총알이
        // 엉뚱한 열 뒤의 창문을 깨는 일이 생긴다.
        const int column = std::clamp(state.aimPixel.x, 0, frameWidth - 1);
        const float camera = CameraOffset(column, frameWidth);
        const float aimAngle = state.ViewAngle() + camera * FieldOfView;
        const RayHit ray = CastRay(grid, state.playerPosition, aimAngle);
        if (!ray.hit || !grid.Contains(ray.position)) return {};
        return { true, ray.position, ray.tile, ray.distance };
    }

    // 흔적을 남겨야 하는 타일인가 — 깨진 창문, 열린 문, 부서진 문.
    bool RaycastRenderer::IsOpening(Tile tile)
    {
        // 지금은 지나갈 수 있지만 한때 단단한 무언가가 서 있던 자리.
        return tile == Tile::WindowBroken || tile == Tile::DoorOpen || tile == Tile::DoorBroken;
    }

    // DDA 레이캐스트 하나. target 이 무엇을 찾을지를 정한다.
    //
    // 격자를 칸 경계마다 건너뛰므로 반복 횟수가 지나는 타일 수에 비례한다.
    RaycastRenderer::RayHit RaycastRenderer::CastRay(const Grid& grid, Vec2 origin, float angle,
        RayTarget target) const
    {
        const Vec2 direction{ std::cos(angle), std::sin(angle) };
        Int2 map = ToTile(origin);
        const float deltaX = std::abs(direction.x) < 0.0001f ? 1.0e20f : std::abs(1.0f / direction.x);
        const float deltaY = std::abs(direction.y) < 0.0001f ? 1.0e20f : std::abs(1.0f / direction.y);
        const int stepX = direction.x < 0.0f ? -1 : 1;
        const int stepY = direction.y < 0.0f ? -1 : 1;
        float sideX = direction.x < 0.0f ? (origin.x - map.x) * deltaX : (map.x + 1.0f - origin.x) * deltaX;
        float sideY = direction.y < 0.0f ? (origin.y - map.y) * deltaY : (map.y + 1.0f - origin.y) * deltaY;
        bool side = false;

        for (int iteration = 0; iteration < 128; ++iteration)
        {
            if (sideX < sideY)
            {
                sideX += deltaX;
                map.x += stepX;
                side = false;
            }
            else
            {
                sideY += deltaY;
                map.y += stepY;
                side = true;
            }
            const bool blocked = !grid.Contains(map) || grid.IsOpaque(map);
            // 개구부를 찾는 탐색이 벽에 먼저 닿았으면 보고할 것이 없다.
            if (blocked && target == RayTarget::Opening) return {};
            if (!blocked && (target == RayTarget::Blocking || !IsOpening(grid.Get(map)))) continue;

            const float distance = side
                ? (map.y - origin.y + (1 - stepY) * 0.5f) / direction.y
                : (map.x - origin.x + (1 - stepX) * 0.5f) / direction.x;
            const float hitCoordinate = side ? origin.x + distance * direction.x : origin.y + distance * direction.y;
            return { std::abs(distance), hitCoordinate - std::floor(hitCoordinate), grid.Get(map), side, map, true };
        }
        return {};
    }

    // 각도를 -π..π 로 접는다.
    float RaycastRenderer::NormalizeAngle(float angle)
    {
        constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
        while (angle > std::numbers::pi_v<float>) angle -= twoPi;
        while (angle < -std::numbers::pi_v<float>) angle += twoPi;
        return angle;
    }

    // 좀비 하나를 화면에 투영한다. **그리기와 명중 판정이 둘 다 여기서 받는다.**
    //
    // 상자를 만드는 곳이 하나뿐이라는 것이 이 함수의 존재 이유다.
    ZombieSprite RaycastRenderer::MakeZombieSprite(const RenderActor& actor, const RenderState& state) const
    {
        ZombieSprite shape;
        const Vec2 offset = actor.position - state.playerPosition;
        shape.distance = Length(offset);

        // 몸의 각도가 아니라 **카메라 각도**다. 사격이 가능한 순간에는 둘이 같으므로
        // — 뒤돌아보기가 방아쇠를 거절한다 — 그리기와 명중 판정이 하나를 같이 쓰는
        // 것은 어긋남을 만드는 것이 아니라 없애는 것이다.
        const float relativeAngle = NormalizeAngle(std::atan2(offset.y, offset.x) - state.ViewAngle());
        if (std::abs(relativeAngle) > FieldOfView * 0.65f) return shape;
        shape.visible = true;

        // 좀비 입장에서 본 보는 사람의 방향. 자기가 향한 방향과의 각의 코사인이고,
        // 정면이 +1, 정후면이 -1, 옆이 0 이다.
        const float toViewer = std::atan2(-offset.y, -offset.x);
        const float delta = NormalizeAngle(actor.facing - toViewer);
        shape.towards = std::cos(delta);
        // 실루엣은 이쪽을 보든 등을 돌리든 같다. 그 둘을 가르는 것은 **눈뿐이다.**
        // 그래서 모양은 절댓값을 받는다.
        shape.profile = std::abs(shape.towards);
        shape.stride = actor.stridePhase;
        shape.collapse = std::clamp(actor.deathProgress, 0.0f, 1.0f);

        // 이 작업 전과 **똑같다.** 상자는 그대로이고 모양 변화는 전부 그 안에서
        // 일어난다. 상자를 움직였으면 명중 판정도 같이 움직였을 것이고,
        // 시체는 쏘기 쉬워져야 할 것이 아니다.
        shape.height = static_cast<float>(frameHeight) / std::max(0.1f, shape.distance);
        shape.width = shape.height * 0.56f;
        shape.left = (relativeAngle / FieldOfView + 0.5f) * frameWidth - shape.width * 0.5f;
        shape.top = frameHeight * 0.5f - shape.height * 0.52f;
        return shape;
    }

    // 상자 안의 (u, v) 가 좀비 픽셀인가. u, v 는 0..1 이다.
    //
    // **그려지는 모든 픽셀은 쏠 수 있어야 하고 모든 틈은 쏠 수 없어야 한다.**
    // 그리기와 명중 판정이 이 함수 하나를 같이 부르는 것이 그것을 보장한다.
    //
    // 픽셀 아트 자산이 오면 이 함수 하나를 파일 마스크로 갈아끼우면 된다.
    bool RaycastRenderer::IsZombiePixel(float u, float v, const ZombieSprite& shape)
    {
        const float fold = std::clamp(shape.collapse, 0.0f, 1.0f);
        float uu = u;
        float vv = v;
        if (fold > 0.0f)
        {
            // **같은 상자 안에서** 접힌다. 길고 가늘던 것이 바닥의 낮고 넓은
            // 더미가 된다. 상자를 안 건드리므로 그리기와 명중 판정이 그것이 어디
            // 있는지에 대해 다른 결론을 낼 수 없다.
            const float squash = 1.0f - 0.70f * fold;
            const float spread = 1.0f + 1.55f * fold;
            vv = (v - (1.0f - squash)) / squash;
            uu = 0.5f + (u - 0.5f) / spread;
            if (vv < 0.0f || vv > 1.0f || uu < 0.0f || uu > 1.0f) return false;
        }

        const float profile = std::clamp(shape.profile, 0.0f, 1.0f);
        // 팔다리는 몸의 선을 따라 흔들린다. 그래서 흔들림이 옆에서 가장 넓고
        // 정면에서는 거의 없다. **이건 근사가 아니다** — 똑바로 걸어오는 사람은
        // 실제로 보폭이 거의 안 보인다. 복도를 따라 다가오던 좀비가 미끄러지는
        // 것처럼 읽혔던 이유가 정확히 그것이다.
        const float swing = std::sin(shape.stride * 6.28318531f) * (1.0f - fold);
        const float legSwing = (0.045f + 0.075f * (1.0f - profile)) * swing;
        // 몸은 발을 디딜 때마다 올라가므로 이것은 보폭의 두 배 주기로 돈다.
        const float bob = std::cos(shape.stride * 12.5663706f) * 0.012f * (1.0f - fold);

        const float half = 0.11f + 0.14f * profile;    // torso half width
        const float reach = 0.20f + 0.22f * profile;   // how far the arms get out

        const float headX = (uu - 0.5f) / (0.62f + 0.38f * profile);
        const float headY = vv - (0.19f + bob);
        if (headX * headX + headY * headY < 0.030f) return true;

        if (vv >= 0.32f + bob && vv <= 0.68f + bob
            && uu >= 0.5f - half && uu <= 0.5f + half) return true;

        // 앞으로 뻗은 채 다리와 반대로 흔들린다. 정지 화면으로는 보여줄 수 없는
        // 비틀걸음의 절반이 이것이다.
        if (vv >= 0.38f + bob && vv <= 0.70f + bob)
        {
            const float taper = (vv - (0.38f + bob)) * 0.25f;
            if (uu <= 0.5f - half * 0.9f && uu >= 0.5f - reach + taper - legSwing) return true;
            if (uu >= 0.5f + half * 0.9f && uu <= 0.5f + reach - taper - legSwing) return true;
        }

        if (vv > 0.65f + bob)
        {
            const float legHalf = half * 0.42f;
            if (std::abs(uu - (0.5f - half * 0.5f + legSwing)) <= legHalf) return true;
            if (std::abs(uu - (0.5f + half * 0.5f - legSwing)) <= legHalf) return true;
        }
        return false;
    }

    // 눈 픽셀인가. 모양이 아니라 색이므로 명중 판정은 이것을 안 본다.
    bool RaycastRenderer::IsZombieEyePixel(float u, float v, const ZombieSprite& shape)
    {
        // 바닥에 누운 몸은 얼굴이 바닥을 향하고, 등을 돌린 몸은 눈이 머리 반대편에
        // 있다. 예전에는 둘 다 상관없이 붉게 빛났고, 그건 **세계가 모르는 것을
        // 플레이어에게 알려주는 것**이었다.
        if (shape.collapse > 0.0f || shape.towards <= 0.0f) return false;
        const float bob = std::cos(shape.stride * 12.5663706f) * 0.012f;
        if (v < 0.12f + bob || v >= 0.22f + bob) return false;
        // 머리가 돌아갈수록 두 눈이 모인다. **절댓값을 쓴다.** 그래야 얼굴이 이쪽에
        // 있는지를 정하는 것이 위의 가드 하나뿐이 된다. 여기서 부호 있는 값을 썼더니
        // 가드가 **지워도 화면이 안 바뀌는 죽은 코드**가 됐고, 그건 틀린 이유로
        // 통과하는 검사다.
        const float spread = 0.07f * std::abs(shape.towards);
        return std::abs(std::abs(u - 0.5f) - spread) < 0.035f;
    }

    // 좀비 전부를 먼 것부터 그린다. 깊이 버퍼로 벽 뒤의 것을 가린다.
    void RaycastRenderer::DrawSprites(const Grid&, const RenderState& state, const std::vector<RenderActor>& actors)
    {
        std::vector<std::size_t> order(actors.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&actors, &state](std::size_t lhs, std::size_t rhs)
        {
            return Length(actors[lhs].position - state.playerPosition) > Length(actors[rhs].position - state.playerPosition);
        });

        for (const std::size_t index : order)
        {
            const RenderActor& actor = actors[index];
            // 죽은 것도 그린다. 맵 전체에 탄약이 몇 발뿐이니 **한 발을 어디에
            // 썼는지는 바닥에 남겨둘 값어치가 있다.** 예전에는 이미 치운 방이
            // 한 번도 안 들어간 방과 똑같이 보였다.
            const ZombieSprite shape = MakeZombieSprite(actor, state);
            if (!shape.visible) continue;
            // 상자를 명중 판정과 **같은 부동소수점 좌표로** 걷는다. 여기서만
            // 픽셀로 잘라내면 두 마스크가 셀의 몇 분의 일만큼 어긋나고, 실루엣
            // 가장자리에서 그것은 **화면에 뻔히 보이는 좀비**에 대한 명중과 빗나감의
            // 차이다.
            const int left = static_cast<int>(std::floor(shape.left));
            const int top = static_cast<int>(std::floor(shape.top));
            const int right = static_cast<int>(std::ceil(shape.left + shape.width));
            const int bottom = static_cast<int>(std::ceil(shape.top
                + std::min(static_cast<float>(frameHeight * 2), shape.height)));
            for (int x = std::max(0, left); x < std::min(frameWidth, right); ++x)
            {
                if (shape.distance >= depthBuffer[static_cast<std::size_t>(x)] + 0.1f) continue;
                const float u = (static_cast<float>(x) - shape.left) / shape.width;
                if (u < 0.0f || u > 1.0f) continue;
                for (int y = std::max(0, top); y < std::min(frameHeight, bottom); ++y)
                {
                    const float v = (static_cast<float>(y) - shape.top) / shape.height;
                    if (v < 0.0f || v > 1.0f) continue;
                    if (!IsZombiePixel(u, v, shape)) continue;
                    Rgb color = v < 0.35f ? Rgb{ 111, 133, 82 } : Rgb{ 73, 79, 68 };
                    // 바닥의 몸은 회색으로 빠진다. 여전히 몸으로 읽히되,
                    // **아직 오고 있는 것으로 오해받으면 안 된다.**
                    if (shape.collapse > 0.0f)
                    {
                        color = Rgb::Scale(color, 1.0f - 0.45f * shape.collapse);
                    }
                    else if (actor.alert && IsZombieEyePixel(u, v, shape))
                    {
                        color = { 215, 42, 31 };
                    }
                    frame.Set(x, y, Rgb::Scale(color, std::clamp(1.2f / (0.4f + shape.distance * 0.12f), 0.35f, 1.0f)));
                }
            }
        }
    }

    // 깨진 창문의 턱과 열린 문의 문틀.
    //
    // **DrawSprites() 다음에 그린다.** 턱이 좀비 다리를 가려야 넘어야 할 높이가 읽힌다.
    void RaycastRenderer::DrawOpeningRemnants(const Grid& grid, const RenderState& state)
    {
        // 이 패스가 없으면 깨진 창문, 열린 문, 빈 출입구가 **픽셀 단위로 똑같다.**
        // 남은 흔적의 모양이 어느 것을 E 로 넘어야 하고 어느 것을 그냥 걸어 지나면
        // 되는지를 말해준다.
        for (int x = 0; x < frameWidth; ++x)
        {
            const float camera = CameraOffset(x, frameWidth);
            const float rayAngle = state.ViewAngle() + camera * FieldOfView;
            const RayHit opening = CastRay(grid, state.playerPosition, rayAngle, RayTarget::Opening);
            if (!opening.hit) continue;

            const float corrected = std::max(0.05f, opening.distance * std::cos(camera * FieldOfView));
            if (corrected >= depthBuffer[static_cast<std::size_t>(x)]) continue;

            const float height = frameHeight / corrected;
            const float top = frameHeight * 0.5f - height * 0.5f;
            const float bottom = top + height;

            // 창문은 허리 높이 턱을 남기고 출입구는 납작한 문지방만 남긴다.
            // **실루엣만으로** 이 구멍을 넘어야 하는지가 읽힌다.
            const bool window = opening.tile == Tile::WindowBroken;
            const Rgb color = window ? Rgb{ 46, 92, 104 } : Rgb{ 86, 60, 43 };
            const float sillTop = bottom - height * (window ? 0.40f : 0.06f);
            const float lintelBottom = top + height * (window ? 0.12f : 0.18f);

            float shade = DistanceShade(corrected, 0.20f);
            if (opening.side) shade *= 0.74f;
            const Rgb body = Rgb::Scale(color, shade);
            const Rgb lip = Rgb::Scale(color, std::min(1.0f, shade * 1.4f));

            for (int y = std::max(0, static_cast<int>(top)); y < std::min(frameHeight, static_cast<int>(bottom)); ++y)
            {
                const float row = static_cast<float>(y);
                if (row >= sillTop) frame.Set(x, y, row < sillTop + 1.0f ? lip : body);
                else if (row < lintelBottom) frame.Set(x, y, body);
            }
        }
    }

    // 탈출구를 바닥 색 구역으로 칠한다. 벽으로 세우지 않는다.
    void RaycastRenderer::DrawExitZone(const Grid& grid, const RenderState& state)
    {
        if (!grid.Contains(grid.exit.origin)) return;

        // 탈출구는 통행을 막지 않으므로 벽으로 서는 대신 **바닥에 칠한다** —
        // 잠겨 있으면 빨강, 열쇠를 손에 넣으면 초록.
        const Rgb zoneColor = state.hasKey ? Rgb{ 64, 202, 96 } : Rgb{ 201, 52, 44 };
        const float horizon = frameHeight * 0.5f;

        for (int x = 0; x < frameWidth; ++x)
        {
            const float camera = CameraOffset(x, frameWidth);
            const float fisheye = std::cos(camera * FieldOfView);
            if (fisheye < 0.0001f) continue;
            const float rayAngle = state.ViewAngle() + camera * FieldOfView;
            const Vec2 direction{ std::cos(rayAngle), std::sin(rayAngle) };
            const float wallDistance = depthBuffer[static_cast<std::size_t>(x)];

            for (int y = frameHeight / 2 + 1; y < frameHeight; ++y)
            {
                // 벽 투영의 역이다 — 이 픽셀이 보여주는 바닥이 얼마나 먼가.
                // 벽은 y 를 지평선 + 지평선 / 수직거리 에 놓는다.
                const float perpendicular = horizon / (static_cast<float>(y) - horizon);
                if (perpendicular >= wallDistance) continue;
                const Vec2 point = state.playerPosition + direction * (perpendicular / fisheye);
                if (!grid.exit.Contains(ToTile(point))) continue;
                const float shade = DistanceShade(perpendicular, 0.25f);
                frame.Set(x, y, Rgb::Scale(zoneColor, shade));
            }
        }
    }

    // 세계의 한 점이 화면 어느 열에 떨어지는가. 열쇠와 탄약이 같이 쓴다.
    RaycastRenderer::Billboard RaycastRenderer::ProjectBillboard(
        const RenderState& state, Vec2 world) const
    {
        const Vec2 offset = world - state.playerPosition;
        const float distance = Length(offset);
        const float relativeAngle = NormalizeAngle(std::atan2(offset.y, offset.x) - state.ViewAngle());
        // 표식 위에 올라서면 방향이 무의미해지므로 최소 거리를 둔다. 시야각의
        // 10분의 6을 넘어가면 어차피 화면 밖이다.
        if (std::abs(relativeAngle) > FieldOfView * 0.6f || distance < 0.2f) return {};
        const int centerX = static_cast<int>((relativeAngle / FieldOfView + 0.5f) * frameWidth);
        if (centerX < 0 || centerX >= frameWidth) return {};
        // 그 열의 벽이 결정한다. 벽 뒤의 표식은 안 보인다.
        if (distance >= depthBuffer[static_cast<std::size_t>(centerX)]) return {};
        return { true, centerX, distance };
    }

    // 바닥에 선 열쇠 표식. 이미 주웠으면 안 그린다.
    void RaycastRenderer::DrawKey(const Grid& grid, const RenderState& state)
    {
        if (state.hasKey) return;
        const Billboard marker = ProjectBillboard(state, TileCenter(grid.keyPosition));
        if (!marker.visible) return;
        const int centerX = marker.centerX;
        const float distance = marker.distance;
        const int size = std::clamp(static_cast<int>(12.0f / distance), 2, 12);
        const int centerY = frameHeight / 2 + static_cast<int>(frameHeight / distance * 0.34f);
        const Rgb gold{ 235, 180, 44 };
        for (int y = -size; y <= size; ++y)
            for (int x = -size; x <= size; ++x)
                if ((x * x + y * y <= size * size && x * x + y * y >= size * size / 3) || (x >= 0 && std::abs(y) <= 1))
                    frame.Set(centerX + x, centerY + y, gold);
    }

    // 바닥에 놓인 탄약 뭉치들.
    void RaycastRenderer::DrawAmmoPickups(const Grid& grid, const RenderState& state)
    {
        // 열쇠와 같은 투영이지만 상자가 더 납작하고 놋쇠 색이 더 차갑다. 그래야
        // 한눈에 갈린다 — 하나는 판을 끝내고 다른 하나는 판을 쉽게 만들 뿐이다.
        for (const AmmoPickup& pickup : grid.ammoPickups)
        {
            const Billboard marker = ProjectBillboard(state, TileCenter(pickup.position));
            if (!marker.visible) continue;
            const int centerX = marker.centerX;
            const float distance = marker.distance;
            const int size = std::clamp(static_cast<int>(9.0f / distance), 2, 9);
            const int centerY = frameHeight / 2 + static_cast<int>(frameHeight / distance * 0.40f);
            const Rgb brass{ 186, 148, 78 };
            for (int y = -size / 2; y <= size / 2; ++y)
            {
                for (int x = -size; x <= size; ++x) frame.Set(centerX + x, centerY + y, brass);
            }
        }
    }

    namespace
    {
        // 권총. **평면 스프라이트로 한 번 그린 다음 통째로 돌린다.**
        //
        // 첫 시도는 축에 정렬된 사각형들을 조립했는데, 그건 손에 든 물건이 아니라
        // 옆에서 본 스티커였다. 두 번째는 같은 사각형들을 대각선을 따라 놓았고
        // **부품들이 갈라졌다** — 붙어 있어야 할 부분에 이음매가 생겼다. 조각마다
        // 따로 픽셀 격자에 반올림됐기 때문이다. 자기 좌표계에서 모양을 한 번 그리고
        // 완성된 것을 회전시키는 것이 온전하게 유지하는 방법이고, 방아쇠울이 회전을
        // 견디는 유일한 버전이다.
        //
        // 왼쪽이 총구, 오른쪽이 카메라다. 손잡이가 내려가면서 오른쪽으로 기울어
        // 있어서 **기울기가 그림 안에 이미 있고** 회전은 그것을 세우기만 하면 된다.
        constexpr const char* WeaponSprite[] = {
            "..........s..............s....",
            ".LLLLLLLLLLLLLLLLLLLLLLLLLLL..",
            "oMSSSSSSSSSSSSSSSSSSSSSSScScS.",
            "oMSSSSSSSSSSSSSSSSSSSSSSScScS.",
            ".MSSSSSSSSSSSSSSSSSSSSSSScScS.",
            ".MDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
            "..FFFFFFFFFFFFFFFFFFFFFFFFFFF.",
            "..FFFFFFFFFFFFFFFFFFFFFFFFFFF.",
            ".......FFFFFFF....FFFFFFFFFFF.",
            "......ttt....tt...FFGGGGGGGG..",
            "......t.t....tt....GGGGGGGGG..",
            "......tttttttt.....gGGGGGGGG..",
            "....................GGGGGGGGg.",
            "....................gGGGGGGGG.",
            ".....................GGGGGGGGg",
            ".....................HHHHHHHHH",
            ".....................hHHHHHHHH",
            "......................HHHHHHHH",
            "......................hhHHHHHh",
            "......................HHHHHHHH"
        };
        constexpr int WeaponSpriteRows = static_cast<int>(std::size(WeaponSprite));
        constexpr int WeaponSpriteColumns = 30;
        // 이만큼 위로, 왼쪽으로 돌린다. 그래야 총구가 장면을 가로지르는 대신
        // 장면 **안쪽**을 향한다. 이보다 얕으면 옆으로 누운 총으로 읽힌다.
        // 이 정도면 총열이 화면 가운데를 향하는데, 거기가 조준선이 있고 플레이어가
        // 보고 있는 곳이다.
        constexpr float WeaponTilt = 0.6981f;   // 40 degrees

        // 무기 스프라이트의 문자 하나를 색으로. 공백이면 false 라 안 그린다.
        bool WeaponInk(char cell, Rgb& colour)
        {
            switch (cell)
            {
            case 'S': colour = { 84, 89, 93 }; return true;    // slide
            case 'L': colour = { 132, 138, 142 }; return true; // its lit top edge
            case 'D': colour = { 52, 56, 60 }; return true;    // under the slide
            case 'c': colour = { 30, 32, 35 }; return true;    // cocking serrations
            case 'M': colour = { 40, 42, 45 }; return true;    // muzzle block
            case 'o': colour = { 12, 12, 14 }; return true;    // the bore, the darkest
            case 's': colour = { 176, 182, 186 }; return true; // sights
            // 프레임과 방아쇠울만 나머지 총이 쓰는 순수한 어둠에서 한 단계 띄운다.
            // 이 둘은 바닥을 배경으로 앉는데 바닥도 어둡고, 이 크기에서 배경과 같은
            // 실루엣은 실루엣이 아니다.
            // **"권총"이라고 말하는 부분이 방아쇠울이므로**, 대비에서 살아남아야 하는
            // 부분도 그것이다.
            case 'F': colour = { 52, 55, 60 }; return true;    // frame
            case 't': colour = { 68, 72, 78 }; return true;    // trigger guard
            case 'G': colour = { 74, 55, 44 }; return true;    // grip
            case 'g': colour = { 44, 32, 26 }; return true;
            case 'H': colour = { 150, 112, 88 }; return true;  // hand
            case 'h': colour = { 112, 82, 64 }; return true;
            default: return false;
            }
        }
    }

    // 손에 든 권총. 평면 스프라이트를 통째로 회전시켜 그린다.
    void RaycastRenderer::DrawWeapon(float recoil, Vec2 sway)
    {
        // 모든 치수가 **프레임에서 뽑은 단위 하나**에서 나온다. 그래야 무기가
        // 어느 창에서나 화면의 같은 비율을 차지한다. 예전에는 고정 픽셀이었고,
        // 그래서 해상도를 올리면 조용히 작아졌다 — 202x118 프레임에서 353x200 으로
        // 가면서 화면상 크기의 40%를 잃었다.
        const float unit = static_cast<float>(std::max(2, frameHeight / 25));
        const float scale = unit * 0.30f;
        const float kick = static_cast<float>(frameHeight) * recoil;
        // 오른쪽 아래, 그리고 손이 아래 가장자리 밖으로 나갈 만큼 낮게 — 손에 든
        // 물건이 그렇듯이. **총은 조준선과 같은 흔들림을 탄다.** 프레임에 못 박힌
        // 총 위로 조준선만 흔들리면 서로 상관없는 두 물건으로 읽힌다.
        // 가운데보다 오른쪽이다. 정중앙에 두면 조준선 밑에 놓여서 플레이어가 겨누는
        // 데 쓰는 바로 그 부분을 가린다.
        const float baseX = static_cast<float>(frameWidth) * 0.68f + sway.x;
        const float baseY = static_cast<float>(frameHeight) + unit * 0.2f + kick + sway.y;
        // 스프라이트 안에서 기준점이 앉는 자리 — 손바닥 아래쪽이다.
        constexpr float originX = WeaponSpriteColumns * 0.80f;
        const float originY = static_cast<float>(WeaponSpriteRows) - 1.0f;

        const float turnCos = std::cos(WeaponTilt);
        const float turnSin = std::sin(WeaponTilt);

        // 스프라이트를 걸으며 앞으로 칠하는 대신, **화면을 걸으며 픽셀마다
        // 스프라이트의 어느 부분인지를 되묻는다.** 앞으로 칠하면 원본이 회전한 뒤에
        // 구멍이 생긴다. 이웃한 원본 칸이 더 이상 이웃한 화면 픽셀에 떨어지지 않기
        // 때문이다.
        float left = static_cast<float>(frameWidth);
        float right = 0.0f;
        float top = static_cast<float>(frameHeight);
        float bottom = 0.0f;
        for (int corner = 0; corner < 4; ++corner)
        {
            const float cornerX = (corner & 1) ? static_cast<float>(WeaponSpriteColumns) : 0.0f;
            const float cornerY = (corner & 2) ? static_cast<float>(WeaponSpriteRows) : 0.0f;
            const float dx = (cornerX - originX) * scale;
            const float dy = (cornerY - originY) * scale;
            const float x = baseX + dx * turnCos - dy * turnSin;
            const float y = baseY + dx * turnSin + dy * turnCos;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }

        const int firstX = std::max(0, static_cast<int>(left) - 1);
        const int lastX = std::min(frameWidth - 1, static_cast<int>(right) + 1);
        const int firstY = std::max(0, static_cast<int>(top) - 1);
        const int lastY = std::min(frameHeight - 1, static_cast<int>(bottom) + 1);
        for (int y = firstY; y <= lastY; ++y)
        {
            for (int x = firstX; x <= lastX; ++x)
            {
                const float dx = static_cast<float>(x) - baseX;
                const float dy = static_cast<float>(y) - baseY;
                // 위 회전의 전치다. **이 쌍을 거꾸로 써도 총은 멀쩡히 다 그려진다.**
                // 다만 바닥을 겨눈 총이 된다 — 스프라이트가 각도의 음수만큼 돌기
                // 때문이다.
                const int column = static_cast<int>((dx * turnCos + dy * turnSin) / scale + originX);
                const int row = static_cast<int>((-dx * turnSin + dy * turnCos) / scale + originY);
                if (row < 0 || row >= WeaponSpriteRows) continue;
                if (column < 0 || column >= WeaponSpriteColumns) continue;
                Rgb colour{};
                if (WeaponInk(WeaponSprite[row][column], colour)) frame.Set(x, y, colour);
            }
        }
    }

    // 피격 시 화면 가장자리를 붉히는 비네트. amount 가 0 이면 바로 반환한다.
    void RaycastRenderer::DrawHitFlash(float amount)
    {
        // 아무 일도 없었으면 아무것도 안 한다. 아래 루프는 프레임의 모든 픽셀을
        // 건드리고, **이 게임은 필요 없는 일을 매 프레임 하는 값을 이미 한 번 치렀다.**
        if (amount <= 0.0f) return;

        constexpr Rgb blood{ 150, 22, 18 };
        const float centreX = static_cast<float>(frameWidth) * 0.5f;
        const float centreY = static_cast<float>(frameHeight) * 0.5f;
        for (int y = 0; y < frameHeight; ++y)
        {
            const float ny = (static_cast<float>(y) - centreY) / std::max(1.0f, centreY);
            for (int x = 0; x < frameWidth; ++x)
            {
                const float nx = (static_cast<float>(x) - centreX) / std::max(1.0f, centreX);
                // 제곱근을 안 쓴다. nx² + ny² 이 이미 가장자리로 갈수록 가파르게
                // 오르는데 그게 비네트가 원하는 모양이고, 절반으로 나누면 모서리가
                // 1이 된다. 근을 씌우면 선형이 될 뿐이고 7만 픽셀에 호출 하나씩을
                // 얹는다.
                const float weight = std::clamp((nx * nx + ny * ny) * 0.5f, 0.0f, 1.0f);
                const float alpha = std::clamp(amount * weight, 0.0f, 1.0f);
                if (alpha <= 0.0f) continue;
                const Rgb source = frame.Get(x, y);
                frame.Set(x, y, {
                    static_cast<std::uint8_t>(source.r + (blood.r - source.r) * alpha),
                    static_cast<std::uint8_t>(source.g + (blood.g - source.g) * alpha),
                    static_cast<std::uint8_t>(source.b + (blood.b - source.b) * alpha) });
            }
        }
    }

    // 무기와 게이지 둘(구르기·스태미나). 글자 HUD 는 여기가 아니라 Hud.cpp 다.
    void RaycastRenderer::DrawWeaponAndHud(const RenderState& state)
    {
        // 뒤돌아보는 동안에는 총을 안 그린다. 몸은 여전히 앞을 향하고 사격은
        // 거절되므로, 화면의 무기는 **플레이어가 할 수 없는 것을 약속하는 것**이 된다.
        // 총이 없다는 사실 자체가 이 모드의 설명 전부다.
        if (!state.lookingBehind)
        {
            DrawWeapon(state.recoil, state.aimOffset);
        }

        // 체력·탄약·열쇠는 글자다. 여기 남는 것은 게이지 둘인데,
        // **연속적인 값은 막대를 원하기 때문이다.**
        //
        // 체력 표시 아래에 걸리는데 그 표시의 높이가 이제 프레임을 따라가므로,
        // 오프셋을 고정 2픽셀이 아니라 상태에서 받는다.
        const int gaugeTop = 4 + std::max(0, state.uiTextHeight);
        const int gaugeWidth = std::max(12, frameWidth / 12);
        const int gaugeHeight = std::max(2, frameHeight / 90);
        frame.FillRect(3, gaugeTop, gaugeWidth, gaugeHeight, { 43, 43, 43 });
        frame.FillRect(3, gaugeTop,
            static_cast<int>(gaugeWidth * std::clamp(state.rollReady, 0.0f, 1.0f)),
            gaugeHeight, { 55, 154, 181 });
        // 구르기 게이지 아래에 스태미나. **구르기가 그것을 쓰므로** 붙어 있어야 한다.
        // 바닥나 갈수록 호박색이 된다.
        const int staminaTop = gaugeTop + gaugeHeight + 1;
        frame.FillRect(3, staminaTop, gaugeWidth, gaugeHeight, { 43, 43, 43 });
        const float staminaLeft = std::clamp(state.stamina, 0.0f, 1.0f);
        const Rgb staminaColor = staminaLeft < 0.34f ? Rgb{ 196, 122, 40 } : Rgb{ 92, 166, 86 };
        frame.FillRect(3, staminaTop, static_cast<int>(gaugeWidth * staminaLeft), gaugeHeight, staminaColor);
    }

    // 구석의 작은 지도. F1 로 켠다.
    void RaycastRenderer::DrawMinimap(const Grid& grid, const RenderState& state, const std::vector<RenderActor>& actors)
    {
        const int originX = 2;
        const int originY = 7;
        for (int y = 0; y < grid.Height(); ++y)
        {
            for (int x = 0; x < grid.Width(); ++x)
            {
                const Rgb color = TileColor(grid.Get({ x, y }), MinimapPalette);
                frame.Set(originX + x, originY + y, color);
            }
        }
        for (const LeafRegion& leafEntry : grid.leaves)
        {
            const Rect& leaf = leafEntry.region;
            const Rgb edge = leafEntry.role == LeafRole::Infected ? Rgb{ 122, 34, 46 }
                : leafEntry.role == LeafRole::Reserved ? Rgb{ 56, 56, 62 }
                : Rgb{ 72, 45, 92 };
            frame.Line(originX + leaf.x, originY + leaf.y, originX + leaf.Right() - 1, originY + leaf.y, edge);
            frame.Line(originX + leaf.x, originY + leaf.y, originX + leaf.x, originY + leaf.Bottom() - 1, edge);
        }
        for (const RenderActor& actor : actors)
            if (actor.alive) frame.Set(originX + ToTile(actor.position).x, originY + ToTile(actor.position).y, { 211, 34, 29 });
        frame.Set(originX + grid.keyPosition.x, originY + grid.keyPosition.y, { 235, 180, 44 });
        for (const AmmoPickup& pickup : grid.ammoPickups)
            frame.Set(originX + pickup.position.x, originY + pickup.position.y, { 186, 148, 78 });
        const Int2 player = ToTile(state.playerPosition);
        frame.FillRect(originX + player.x - 1, originY + player.y - 1, 3, 3, { 224, 224, 205 });
        const int lookX = originX + player.x + static_cast<int>(std::round(std::cos(state.playerAngle) * 3.0f));
        const int lookY = originY + player.y + static_cast<int>(std::round(std::sin(state.playerAngle) * 3.0f));
        frame.Line(originX + player.x, originY + player.y, lookX, lookY, { 224, 224, 205 });
    }
}
