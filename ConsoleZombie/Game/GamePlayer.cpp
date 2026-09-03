#include "Game.h"

#include <algorithm>
#include <array>
#include <cmath>

// 플레이어가 세계에 하는 일 — 지나가고, 열고, 넘고, 쏜다.
//
// 여기 있는 것 하나하나가 **플레이어가 무엇에 닿을 수 있는가**에 대한 규칙이다.
// 좀비가 무엇을 하는가나 루프가 무엇을 언제 돌리는가와는 다른 질문이다.
namespace Zombie
{
    // 플레이어의 한 고정 시간 간격 — 회전, 구르기, 이동, 스태미나, 조준 흔들림.
    void Game::UpdatePlayer(float deltaTime, const InputState& input)
    {
        if (input.turnLeft) playerAngle -= balance.playerRotationSpeed * deltaTime;
        if (input.turnRight) playerAngle += balance.playerRotationSpeed * deltaTime;
        const bool turning = input.turnLeft || input.turnRight;
        rollCooldown = std::max(0.0f, rollCooldown - deltaTime);
        rollTimer = std::max(0.0f, rollTimer - deltaTime);
        weaponRecoil = std::max(0.0f, weaponRecoil - deltaTime);
        hitFlash = std::max(0.0f, hitFlash - deltaTime);
        // 위상은 실제 시계가 아니라 **고정 시간 간격**으로 나아간다. 그래서 흔들림이
        // 입력 이력의 함수이고 그 외의 아무것도 아니다.
        aimSwayPhase += deltaTime;
        aimSwayTarget = 0.0f;
        // 회복은 마지막으로 힘을 쓴 뒤 짧은 유예를 기다린다. 그래야 Shift 를
        // 껐다 켰다 하면서 영원히 안 지치는 일이 없다.
        staminaIdleTime += deltaTime;
        if (staminaIdleTime >= balance.staminaRegenDelay)
        {
            stamina = std::min(balance.staminaMax, stamina + balance.staminaRegen * deltaTime);
        }
        // 구르기는 스태미나를 고정량 뜯어간다. 그래야 달리기보다 빠른 이동 수단으로
        // 연타되지 않는다.
        if (input.roll && !previousRoll && rollCooldown <= 0.0f
            && stamina >= balance.staminaRollCost)
        {
            rollTimer = balance.rollDuration;
            rollCooldown = balance.rollCooldown;
            stamina = std::max(0.0f, stamina - balance.staminaRollCost);
            staminaIdleTime = 0.0f;
            EmitNoise(balance.rollNoise, balance.rollReactionChance);
        }

        const Vec2 forward{ std::cos(playerAngle), std::sin(playerAngle) };
        if (rollTimer > 0.0f)
        {
            aimSwayTarget = balance.aimSwayRollPixels;
            MovePlayer(forward * (balance.rollSpeed * deltaTime));
        }
        else
        {
            const float direction = (input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f);
            // 달리기는 자원이다. 스태미나가 떨어지면 질주가 걷기로 내려가고,
            // 그게 빈 복도에서 모든 좀비를 따돌리는 것을 막는 장치다.
            //
            // **바가 비어도 키를 누르고 있는 것 자체가 소모로 친다.** 안 그러면
            // 빈 바는 더 이상 줄지 않고, 회복 유예가 지나고, 조금 차오른 것이 같은
            // 프레임에 쓰인다 — 끝나지 않는 딸꾹질 질주다. 회복을 막는 것은
            // **달리려고 시도하는 것**이지 달리기에 성공하는 것이 아니다.
            const bool wantsSprint = input.run && direction > 0.0f;
            const bool sprinting = wantsSprint && stamina > 0.0f;
            if (wantsSprint) staminaIdleTime = 0.0f;
            if (sprinting)
            {
                stamina = std::max(0.0f, stamina - balance.staminaRunDrain * deltaTime);
            }
            const float speed = sprinting ? balance.playerRunSpeed : balance.playerWalkSpeed;
            const Vec2 positionBeforeMove = playerPosition;
            MovePlayer(forward * (direction * speed * deltaTime));
            const bool moving = direction != 0.0f
                && Length(playerPosition - positionBeforeMove) > 0.0001f;
            if (moving)
            {
                aimSwayTarget = sprinting ? balance.aimSwayRunPixels : balance.aimSwayWalkPixels;
                // 소음 사건은 고정 업데이트마다가 아니라 **보폭마다** 하나다.
                // 걷기와 달리기가 타이머를 공유하고, 다른 것은 간격과 크기뿐이다.
                footstepTimer -= deltaTime;
                if (footstepTimer <= 0.0f)
                {
                    const float noise = sprinting ? balance.runNoise : balance.walkNoise;
                    EmitNoise(noise, sprinting
                        ? balance.runReactionChance : balance.walkReactionChance);
                    // **같은 사건이다.** 그래서 플레이어가 듣는 걸음과 맵이 듣는
                    // 걸음이 다른 걸음일 수 없다.
                    //
                    // 크기는 방금 그 걸음이 낸 소음에서, 질주 소음에 대한 비율로
                    // 나온다. **새 밸런스 키가 없다** — 귀가 침입구와 같은 말을 듣고,
                    // 그게 질주를 그 위험만큼 들리게 만든다. 가운데인 이유는 이것이
                    // 플레이어 자신의 발이기 때문이고, 총성이 World 가 아니라 Ui 큐인
                    // 것과 같은 이유다.
                    audio.Play(AudioCue::Footstep,
                        noise / std::max(1.0f, balance.runNoise));
                    footstepTimer = sprinting ? balance.runNoiseInterval : balance.walkNoiseInterval;
                }
            }
            else
            {
                footstepTimer = 0.0f;
            }
        }
        // 7.x 는 회전을 걷기와 같이 묶는다. 발이 멈춰 있어도 몸을 돌리면 조준이
        // 흔들린다. **목표치를 올리기만 하므로** 질주 중 회전은 질주 수치를 유지한다.
        if (turning) aimSwayTarget = std::max(aimSwayTarget, balance.aimSwayWalkPixels);

        // **시간이 드는 쪽은 가라앉는 것이지 흔들리기 시작하는 것이 아니다.**
        // 멈춘 플레이어는 흔들림이 떨어지기를 기다려야 하고, 그 기다림이 이 메커닉
        // 전체가 기대는 거래다 — 가만히 서면 조준이 안정되고, 그동안 좀비는 계속 걷는다.
        //
        // 비율이 아니라 **고정 속도**로 떨어진다. 그래야 7.x 의 "0.2초 안에 사라진다"가
        // 말 그대로 참이 된다. 최악은 구르기이므로 그 수치에서 0까지 정확히 정착
        // 시간이 걸리고, 그보다 작은 것은 전부 더 빠르다.
        if (aimSwayTarget >= aimSway)
        {
            aimSway = aimSwayTarget;
        }
        else
        {
            const float fall = balance.aimSwaySettle > 0.0f
                ? balance.aimSwayRollPixels / balance.aimSwaySettle * deltaTime
                : aimSway;
            aimSway = std::max(aimSwayTarget, aimSway - fall);
        }

        if (input.interact && !previousInteract) TryInteract();
        if (input.leftClicked) Shoot();
    }

    // 방아쇠. 거절 조건을 먼저 보고, 총알을 판정하고, 그 다음에 소음과 반동이다.
    //
    // **순서가 전부다.** 세 가지가 서로 다른 이유로 이 순서를 요구한다.
    void Game::Shoot()
    {
        // **뒤돌아보기는 보는 것이지 겨누는 것이 아니다.** 뒤에 보이는 것을 쏠 수
        // 있으면 그 키가 공짜 즉시 180도 회전이 되고, 추격 전체가 기대는 느린 A/D
        // 회전이 통째로 무의미해진다.
        //
        // 거절을 호출부가 아니라 **여기에** 둔다. 그래야 나중에 생기는 호출자도
        // 자동으로 물려받는다.
        if (lookingBehind) return;

        if (ammo <= 0) return;
        --ammo;
        // **모든 거절을 지난 뒤에** 센다. 빈 약실에 방아쇠를 당긴 것은 사격이 아니고,
        // 안 그러면 결과 카드가 나가지도 않은 탄약을 보고한다.
        ++shotsFired;

        // 총알을 **자기 소음보다 먼저, 자기 반동보다도 먼저** 판정한다.
        //
        // 소음 쪽 규칙이 먼저 생겼다 — 사격 소음이 같은 프레임에 깨는 창문이
        // **이 총알이 맞는 것을 바꾸면 안 된다.** 반동 쪽 규칙은 조준선 반동과 함께
        // 왔고 더 날카롭다. 반동이 aimPixel 자체를 움직이기 때문이다.
        // weaponRecoil 을 이 줄 위에서 세우면 총알이 플레이어가 겨눈 자리가 아니라
        // **반동이 조준선을 던져놓은 자리**에서 판정된다. 7.x 는 총알이 그려진
        // 조준선이 있는 자리로 정확히 가기를 요구한다.
        const std::vector<RenderActor> actors = MakeRenderActors();
        const RenderState state = MakeRenderState(InputState{});
        const int hit = renderer.HitTestZombie(grid, state, actors);
        if (hit >= 0)
        {
            zombies[static_cast<std::size_t>(hit)].alive = false;
            // 지금은 한 사건에 카운터 둘이다. 총알 하나가 좀비 하나이기 때문이다.
            // 3.9 가 결과 카드에 요구하는 숫자이고, **좀비가 한 방을 견디는 날
            // 이 둘이 갈라진다.**
            ++shotsHit;
            ++kills;
        }
        else
        {
            // 권총은 유리만 깬다. 문도, 잠긴 탈출구도, 벽도 버틴다.
            const EnvironmentHit environment = renderer.HitTestEnvironment(grid, state);
            if (environment.valid && IsSealedWindow(environment.tile)) BreakWindow(environment.position);
        }

        // 모든 거절과 명중 판정을 **지난 뒤**다. 그래야 반동이 "총알이 나갔다"를
        // 뜻하고, 방금 나간 그 총알을 움직이지 못한다.
        weaponRecoil = balance.weaponRecoilDuration;
        // 플레이어가 쐈으므로 가운데에서 감쇠 없이 난다 — World 가 아니라 Ui 큐다.
        // **음향 필드는 맵이 무엇을 듣는지를 정하지, 총을 든 손이 무엇을 듣는지를
        // 정하지 않는다.**
        audio.Play(AudioCue::Gunshot);
        EmitNoise(balance.shotNoise, balance.shotReactionChance);
    }

    // 창문 하나를 깬다. 그 침입구를 압력 추적에서도 뺀다.
    void Game::BreakWindow(Int2 position)
    {
        PlayWorldCue(AudioCue::WindowBreak, position);
        grid.Set(position, Tile::WindowBroken);
        // 비활성화하면 압력 감소가 플레이어가 쏴서 깬 창문을 되살리는 일이 없고,
        // 가중 소음 선택이 다른 침입구를 고를 수 있게 된다.
        breachSystem.Deactivate(position);
    }

    // E — 문 열기, 열쇠·탄약 줍기, 창문 넘기, 탈출구 통과.
    //
    // 무엇에 닿았는지를 보고 그중 하나가 일어난다.
    void Game::TryInteract()
    {
        const Vec2 forward{ std::cos(playerAngle), std::sin(playerAngle) };
        for (float distance = 0.4f; distance <= 1.25f; distance += 0.2f)
        {
            const Int2 tile = ToTile(playerPosition + forward * distance);
            if (IsClosedDoor(grid.Get(tile)))
            {
                // 6.9 — 논리적 문 하나가 출입구의 **두 타일 모두**를 소유한다.
                // 반쪽을 열고 돌아서서 나머지 반쪽을 또 열어야 하는 것은 문이 아니다.
                OpenDoorway(tile);
                PlayWorldCue(AudioCue::DoorOpen, tile);
                // 소음이 나갈 때 문은 이미 열려 있다. 그래서 소리가 플레이어가 방금
                // 만든 틈으로 지나간다.
                EmitNoise(balance.doorNoise, balance.doorReactionChance);
                return;
            }
            if (grid.Get(tile) == Tile::WindowBroken)
            {
                // 깨진 유리는 걸어서 지나는 것이 아니라 **넘는** 것이다. 누를 때마다
                // 한 번 시도하고, 반대쪽이 막혀 있으면 그냥 제자리에 있는다.
                TryVaultWindow(tile);
                return;
            }
        }
    }

    // 이 타일이 속한 출입구의 두 타일을 함께 연다.
    void Game::OpenDoorway(Int2 tile)
    {
        breachSystem.Deactivate(tile);
        grid.Set(tile, Tile::DoorOpen);
        // 복도를 판 쪽이 기록해 뒀다면 그 짝. 뒤에 출입구가 없는 문 타일은 남은
        // 흔적일 수밖에 없으므로, 그것만 여는 것이 맞다.
        for (const Doorway& doorway : grid.doorways)
        {
            if (!doorway.tiles.Contains(tile)) continue;
            for (const Int2 half : { doorway.tiles.origin, doorway.tiles.Second() })
            {
                if (!IsClosedDoor(grid.Get(half))) continue;
                breachSystem.Deactivate(half);
                grid.Set(half, Tile::DoorOpen);
            }
            return;
        }
    }

    // 깨진 창문을 넘어 반대쪽에 내린다. 반대쪽이 막혀 있으면 false.
    bool Game::TryVaultWindow(Int2 window)
    {
        // 생성기가 창문이 벽 선 안에 있음을 보장하므로, 열린 쪽이 있는 축은 정확히
        // 하나다. 플레이어가 서 있지 않은 쪽으로 내린다.
        const Int2 playerTile = ToTile(playerPosition);
        const std::array<Int2, 2> axes = { Int2{ 1, 0 }, Int2{ 0, 1 } };
        for (const Int2 axis : axes)
        {
            const Int2 sideA{ window.x - axis.x, window.y - axis.y };
            const Int2 sideB{ window.x + axis.x, window.y + axis.y };
            Int2 destination = sideA;
            if (sideA == playerTile) destination = sideB;
            else if (sideB == playerTile) destination = sideA;
            else if (Length(TileCenter(sideB) - playerPosition) > Length(TileCenter(sideA) - playerPosition))
                destination = sideB;

            const Vec2 landing = TileCenter(destination);
            if (!CanPlayerOccupy(landing)) continue;
            playerPosition = landing;
            return true;
        }
        return false;
    }

    // 플레이어가 이 자리에 설 수 있는가. 몸통 네 모서리를 전부 본다.
    //
    // Grid::IsWalkable() 보다 엄하다. **둘이 어긋나도 되는 타일은 E 로 통과할 수
    // 있는 것뿐이다** — 닫힌 문과 깨진 창문. 새 타일을 더할 때 반드시 확인한다.
    bool Game::CanPlayerOccupy(Vec2 position) const
    {
        constexpr float radius = 0.18f;
        const Int2 points[] = {
            ToTile({ position.x - radius, position.y - radius }),
            ToTile({ position.x + radius, position.y - radius }),
            ToTile({ position.x - radius, position.y + radius }),
            ToTile({ position.x + radius, position.y + radius })
        };
        for (const Int2 point : points)
        {
            const Tile tile = grid.Get(point);
            // WindowBroken 은 좀비 길찾기에서는 통행 가능으로 남지만, 플레이어는
            // 창틀을 걸어서 지나는 대신 E 로 넘는다. **탈출구는 두 상태 모두 통행
            // 가능이다** — 잠긴 탈출구 위에 서면 그냥 아무 일도 안 일어난다.
            if (!grid.IsWalkable(point) || IsClosedDoor(tile) || tile == Tile::WindowBroken)
                return false;
        }
        return true;
    }

    // 축을 따로 밀어본다. 그래야 벽에 비스듬히 부딪혔을 때 멈추지 않고 미끄러진다.
    void Game::MovePlayer(Vec2 delta)
    {
        Vec2 candidate = playerPosition;
        candidate.x += delta.x;
        if (CanPlayerOccupy(candidate)) playerPosition.x = candidate.x;
        candidate = playerPosition;
        candidate.y += delta.y;
        if (CanPlayerOccupy(candidate)) playerPosition.y = candidate.y;
    }
}
