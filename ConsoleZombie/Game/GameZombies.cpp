#include "Game.h"

#include <AI/AStar.h>

#include <algorithm>
#include <cmath>

// 좀비와 그들이 듣는 소리.
//
// 한 파일에 둔 이유는 **하나의 시스템이기 때문이다.** 소음이 흥미로운 것은 무언가가
// 그것을 듣기 때문이고, 듣는 것은 좀비 아니면 침입구이며, 둘이 같은 음향 필드를 쓴다.
namespace Zombie
{
    namespace
    {
        // RaycastRenderer 도 자기 것을 갖고 있다. 좀비가 필요로 하는 각도 감싸기는
        // 이것 하나뿐이고, 그걸 렌더러에서 가져다 쓰면 AI 가 그리기에 묶인다.
        // 각도를 -π..π 로 접는다. 방향을 서서히 돌릴 때 지름길로 가게 해준다.
        float WrapAngle(float angle)
        {
            constexpr float twoPi = 6.28318531f;
            while (angle > 3.14159265f) angle -= twoPi;
            while (angle < -3.14159265f) angle += twoPi;
            return angle;
        }

        // 좀비마다 다른 오프셋. 그래야 총성 하나에 깨어난 무리가 남은 판 내내
        // 합창하지 않는다.
        //
        // **난수가 아니라 깨어난 타일에서 읽는다.** 세계 상태 어디든 난수원이
        // 하나 들어가는 순간 렌더 다이제스트가 오라클이기를 그만둔다.
        // 좀비마다 다른 울음 시작 오프셋. 난수가 아니라 타일에서 나온다.
        float GrowlStagger(Int2 tile, float interval)
        {
            return static_cast<float>((tile.x * 7 + tile.y * 13) % 8) / 8.0f * interval;
        }
    }

    // 좀비를 깨우는 **유일한** 길.
    //
    // 손으로 alert = true 를 세우면 그 경로만 조용해진다. 실제로 네 경로 중 셋이
    // 그랬고, 플레이로만 드러났다. 새 경로를 만들면 여기를 지나게 한다.
    void Game::WakeZombie(ZombieAgent& zombie)
    {
        if (zombie.alert) return;
        zombie.alert = true;
        zombie.repathTimer = 0.0f;
        // 알아챘다. 그 즉시 소리를 내고, **플레이어가 실제로 들을 수 있었을 때만**
        // 로그에 적는다. 맵 반대편에서 깨어난 좀비는 아무도 듣지 못한 것이다.
        if (PlayWorldCue(AudioCue::ZombieGrowl, ToTile(zombie.position)))
        {
            PushLog("좀비가 그르렁거리는 소리가 들린다");
        }
        zombie.growlTimer = balance.zombieGrowlInterval
            + GrowlStagger(ToTile(zombie.position), balance.zombieGrowlInterval);
    }

    // 좀비 몸통의 네 모서리가 모두 설 수 있는 자리인가. 닫힌 문은 A* 에서는
    // 비용을 치르고 들어갈 수 있지만, 실제 몸은 문을 연 다음 프레임에 지나간다.
    // 깨진 창문은 플레이어와 달리 좀비에게는 그대로 통행 가능이다.
    bool Game::CanZombieOccupy(Vec2 position) const
    {
        const Int2 points[] = {
            ToTile({ position.x - ZombieCollisionRadius, position.y - ZombieCollisionRadius }),
            ToTile({ position.x + ZombieCollisionRadius, position.y - ZombieCollisionRadius }),
            ToTile({ position.x - ZombieCollisionRadius, position.y + ZombieCollisionRadius }),
            ToTile({ position.x + ZombieCollisionRadius, position.y + ZombieCollisionRadius })
        };
        for (const Int2 point : points)
        {
            const Tile tile = grid.Get(point);
            if (!grid.IsWalkable(point) || IsClosedDoor(tile)) return false;
        }
        return true;
    }

    // A* 의 목표는 플레이어 타일 하나이므로 여러 좀비의 경로 중심선은 필연적으로
    // 겹친다. 동적 장애물을 A* 에 섞는 대신, 이동이 끝난 위치에서 살아 있는 몸끼리만
    // 밀어낸다. 그러면 문 앞에서는 줄을 서고 넓은 곳에서는 옆으로 퍼진다.
    //
    // 좀비 수가 열둘이라 전수 쌍 검사는 한 패스에 66쌍뿐이다. 고정된 순서와 고정된
    // 방향만 써 같은 입력은 여전히 같은 결과를 낸다.
    void Game::SeparateZombies()
    {
        constexpr int relaxationPasses = 12;
        constexpr float contactPadding = 0.0001f;
        const float minimumDistance = ZombieCollisionRadius * 2.0f;
        const float minimumDistanceSquared = minimumDistance * minimumDistance;

        std::vector<Vec2> beforePositions;
        beforePositions.reserve(zombies.size());
        for (const ZombieAgent& zombie : zombies) beforePositions.push_back(zombie.position);

        for (int pass = 0; pass < relaxationPasses; ++pass)
        {
            bool movedAny = false;
            for (std::size_t firstIndex = 0; firstIndex < zombies.size(); ++firstIndex)
            {
                ZombieAgent& first = zombies[firstIndex];
                if (!first.alive) continue;

                for (std::size_t secondIndex = firstIndex + 1; secondIndex < zombies.size(); ++secondIndex)
                {
                    ZombieAgent& second = zombies[secondIndex];
                    if (!second.alive) continue;

                    const Vec2 difference = second.position - first.position;
                    const float distanceSquared = difference.x * difference.x
                        + difference.y * difference.y;
                    if (distanceSquared >= minimumDistanceSquared) continue;

                    float distance = std::sqrt(distanceSquared);
                    Vec2 normal;
                    if (distance > 0.0001f)
                    {
                        normal = difference * (1.0f / distance);
                    }
                    else
                    {
                        // 정확히 한 점이면 두 위치만으로는 방향을 만들 수 없다. 좀비
                        // 색인에서 축을 골라 난수 없이 항상 같은 쪽으로 갈라 놓는다.
                        switch ((firstIndex * 3 + secondIndex) % 4)
                        {
                        case 0: normal = { 1.0f, 0.0f }; break;
                        case 1: normal = { 0.0f, 1.0f }; break;
                        case 2: normal = { -1.0f, 0.0f }; break;
                        default: normal = { 0.0f, -1.0f }; break;
                        }
                        distance = 0.0f;
                    }

                    const float correction = minimumDistance - distance + contactPadding;
                    const Vec2 half = normal * (correction * 0.5f);
                    const Vec2 firstHalf = first.position + half * -1.0f;
                    const Vec2 secondHalf = second.position + half;
                    // 아직 자는 몸과 밀폐 포켓에서 기다리는 몸은 제자리를 지킨다.
                    // 추적자가 다가오면 움직이는 쪽이 간격을 전부 부담한다.
                    const bool firstMayMove = first.alert && !first.waitingBehindBreach;
                    const bool secondMayMove = second.alert && !second.waitingBehindBreach;
                    const bool firstCanMoveHalf = firstMayMove && CanZombieOccupy(firstHalf);
                    const bool secondCanMoveHalf = secondMayMove && CanZombieOccupy(secondHalf);

                    if (firstCanMoveHalf && secondCanMoveHalf)
                    {
                        first.position = firstHalf;
                        second.position = secondHalf;
                        movedAny = true;
                        continue;
                    }

                    // 벽 쪽 몸이 움직일 수 없으면 반대쪽 몸이 간격 전부를 부담한다.
                    // 전부 움직일 자리도 없으면 가능한 절반만 옮겨 다음 패스가 잇는다.
                    const Vec2 full = normal * correction;
                    const Vec2 firstFull = first.position + full * -1.0f;
                    const Vec2 secondFull = second.position + full;
                    if (firstCanMoveHalf && CanZombieOccupy(firstFull))
                    {
                        first.position = firstFull;
                        movedAny = true;
                    }
                    else if (secondCanMoveHalf && CanZombieOccupy(secondFull))
                    {
                        second.position = secondFull;
                        movedAny = true;
                    }
                    else if (firstCanMoveHalf)
                    {
                        first.position = firstHalf;
                        movedAny = true;
                    }
                    else if (secondCanMoveHalf)
                    {
                        second.position = secondHalf;
                        movedAny = true;
                    }
                }
            }
            if (!movedAny) break;
        }

        // 밀려난 거리도 실제 이동이다. 보폭이 시계가 아니라 몸이 움직인 거리라는
        // 기존 규칙을 그대로 지킨다.
        const float stride = std::max(0.05f, balance.zombieStrideTiles);
        for (std::size_t index = 0; index < zombies.size(); ++index)
        {
            ZombieAgent& zombie = zombies[index];
            if (!zombie.alive) continue;
            zombie.stridePhase += Length(zombie.position - beforePositions[index]) / stride;
            if (zombie.stridePhase >= 1.0f) zombie.stridePhase -= std::floor(zombie.stridePhase);
        }
    }

    // 좀비 전부의 한 고정 시간 간격 — 죽음, 울음, 대기, 인지, 경로, 이동, 공격.
    void Game::UpdateZombies(float deltaTime)
    {
        for (ZombieAgent& zombie : zombies)
        {
            if (!zombie.alive)
            {
                // 쓰러지는 중. 나머지와 마찬가지로 고정 시간 간격으로 돈다.
                // 그래야 같은 입력을 재현하면 시체가 같은 모양으로 눕는다.
                zombie.deathProgress = balance.zombieDeathDuration > 0.0f
                    ? std::min(1.0f, zombie.deathProgress + deltaTime / balance.zombieDeathDuration)
                    : 1.0f;
                continue;
            }
            // **밀폐 포켓 검사보다 앞이다.** 그래야 아직 갇혀 있는 좀비도 계속 운다.
            // 그중 얼마가 플레이어에게 닿는지는 음향 필드가 정하고, 막힌 문은 3을
            // 매긴다 — 죽인 것이 아니라 먹먹하게 만든 것이다.
            // **벽 뒤에서 뭔가 움직이는 것을 듣는 것**이 요점이다.
            if (zombie.alert)
            {
                zombie.growlTimer -= deltaTime;
                if (zombie.growlTimer <= 0.0f)
                {
                    // 소리만. 로그 줄은 **플레이어를 알아채는 순간**의 것이고,
                    // 몇 초마다 반복하면 다른 모든 것을 화면 밖으로 밀어낸다.
                    PlayWorldCue(AudioCue::ZombieGrowl, ToTile(zombie.position));
                    zombie.growlTimer = balance.zombieGrowlInterval;
                }
            }

            if (zombie.waitingBehindBreach)
            {
                // 감염 공간 안에서 자고 있다. 자기가 묶인 침입구가 실제로 열리기
                // 전까지 인지도, 이동도, 공격도 없다.
                //
                // 타일 이름을 대는 대신 **통행 가능한지를 묻는다.** 창문 침입구는
                // WindowBroken 으로 끝나고 막힌 문은 DoorBroken 으로 끝나는데,
                // WindowBroken 만 검사했더니 막힌 문 뒤의 좀비들이 영영 자고 있었다.
                const bool otherOpen = zombie.otherBreachPosition.x >= 0
                    && grid.IsWalkable(zombie.otherBreachPosition);
                if (!grid.IsWalkable(zombie.breachPosition) && !otherOpen)
                {
                    // 뭔가 들었고 나가고 싶다. 안에서 할 수 있는 일이 침입구를 미는
                    // 것뿐이고, **이게 없으면 감염 공간은 스스로 길을 뚫을 만큼 큰
                    // 소음을 영원히 기다린다.**
                    if (zombie.alert)
                    {
                        const Int2 pressureTarget = zombie.selectedBreachPosition.x >= 0
                            ? zombie.selectedBreachPosition : zombie.breachPosition;
                        breachSystem.AddPressure(pressureTarget,
                            balance.breachZombiePressurePerSecond * deltaTime, grid);
                    }
                    continue;
                }
                zombie.waitingBehindBreach = false;
                WakeZombie(zombie);
            }
            const float playerDistance = Length(playerPosition - zombie.position);
            // 플레이어를 **보는** 것도 듣는 것과 똑같이 알아채는 방법이다.
            // 예전에는 이쪽만 소리가 안 났다.
            if (playerDistance <= balance.zombieAwarenessRange) WakeZombie(zombie);
            zombie.repathTimer -= deltaTime;
            zombie.attackTimer -= deltaTime;
            if (!zombie.alert) continue;

            if (zombie.repathTimer <= 0.0f)
            {
                zombie.path = AStar::FindPath(grid, ToTile(zombie.position), ToTile(playerPosition));
                zombie.pathIndex = zombie.path.size() > 1 ? 1 : 0;
                zombie.repathTimer = balance.zombieRepathInterval;
            }
            if (zombie.pathIndex < zombie.path.size())
            {
                const Int2 nextTile = zombie.path[zombie.pathIndex];
                if (IsClosedDoor(grid.Get(nextTile)))
                {
                    // 플레이어와 좀비가 논리적 문을 여는 규칙을 공유한다. 직접 한
                    // 타일만 바꾸면 2칸짜리 출입구의 나머지 반쪽이 벽처럼 남는다.
                    OpenDoorway(nextTile);
                    // 좀비가 문을 밀어 여는 것은 플레이어가 문을 여는 것과 **같은
                    // 사건**이고, 일어난 자리에서 들린다. 이 분기 **안**이지 뒤가
                    // 아니다 — 밖에 두면 걸음마다 소리가 난다.
                    PlayWorldCue(AudioCue::DoorOpen, nextTile);
                    zombie.repathTimer = 0.0f;
                    continue;
                }
                const Vec2 target = TileCenter(nextTile);
                const Vec2 offset = target - zombie.position;
                const float distance = Length(offset);
                if (distance < 0.08f)
                {
                    ++zombie.pathIndex;
                }
                else
                {
                    const Vec2 step = Normalize(offset) * (balance.zombieSpeed * deltaTime);
                    zombie.position += step;
                    // 시간이 아니라 **거리**다. 문에 막힌 좀비는 제자리걸음을 하지
                    // 않고 발을 그대로 둔다.
                    const float stride = std::max(0.05f, balance.zombieStrideTiles);
                    zombie.stridePhase += Length(step) / stride;
                    if (zombie.stridePhase >= 1.0f) zombie.stridePhase -= std::floor(zombie.stridePhase);
                    // 스냅이 아니라 서서히, 그리고 튜닝 값이 아니라 상수로.
                    // 이건 4방향 길찾기가 **제자리에서 홱 도는 몸**으로 보이는 것을
                    // 막는 장치이지 감으로 맞출 값이 아니다.
                    const float turn = WrapAngle(std::atan2(offset.y, offset.x) - zombie.facing);
                    zombie.facing = WrapAngle(zombie.facing + turn * std::min(1.0f, deltaTime * 6.0f));
                }
            }
            if (playerDistance < 0.58f && zombie.attackTimer <= 0.0f)
            {
                if (rollTimer <= 0.0f && !developerDebug)
                {
                    --health;
                    // 하트는 구석에 있고 플레이어는 조준선을 보고 있다. 이게 없으면
                    // 맞았다는 것을 나중에야 — 기억보다 짧아진 막대를 보고 — 알아챈다.
                    hitFlash = balance.hitFlashDuration;
                }
                zombie.attackTimer = balance.zombieAttackInterval;
            }
        }
        SeparateZombies();
    }

    // 이 소리가 **실제로 닿는** 좀비를 깨운다. 거리가 아니라 음향 경로 비용으로 잰다.
    void Game::AlertZombiesWithinEarshot(float amount)
    {
        // 예전에는 **사격 한 번이 맵의 모든 좀비를 깨웠다.** 그래서 한 발에 두 방
        // 건너에서 좀비가 몰려왔다. 지금은 침입구가 이미 쓰던 것과 같은 규칙으로,
        // 음향 필드가 들린다고 말하는 만큼만 듣는다 — 유효 소음은 원본에서 경로 비용
        // 곱하기 감쇠를 뺀 값이고, 벽은 타일당 6을 매긴다.
        //
        // 감염 공간에 갇힌 좀비는 들은 것으로 행동할 수는 없지만 **자기 침입구를 밀기
        // 시작한다.** UpdateZombies 를 보라.
        const Int2 origin = ToTile(playerPosition);
        for (ZombieAgent& zombie : zombies)
        {
            if (!zombie.alive || zombie.alert) continue;
            const float cost = breachSystem.PathCost(origin, ToTile(zombie.position), grid);
            if (!std::isfinite(cost)) continue;
            if (amount - cost * balance.noiseAcousticFalloff <= 0.0f) continue;
            WakeZombie(zombie);
        }
    }

    // 소음 하나를 세계에 낸다. 좀비와 침입구가 각자의 규칙으로 받는다.
    void Game::EmitNoise(float amount, float reactionChance)
    {
        // 사건 하나에 듣는 쪽이 둘. 좀비는 그냥 듣고, 침입구는 **행동별 확률 판정이
        // "맵이 반응한다"고 할 때만** 압력을 받는다.
        AlertZombiesWithinEarshot(amount);
        const std::vector<Int2> responders = breachSystem.ApplyNoise(
            playerPosition, amount, reactionChance, grid, noiseRandom);
        for (const Int2 responder : responders)
        {
            StirBreachZombie(responder);
            PushLog(IsSealedDoor(grid.Get(responder))
                ? "문을 두드리는 소리가 들린다" : "창문을 두드리는 소리가 들린다");
            PlayWorldCue(AudioCue::BreachKnock, responder);
        }
    }

    // 이 침입구 뒤의 좀비를 깨운다. 아직 못 나오지만 듣기 시작한다.
    void Game::StirBreachZombie(Int2 breachPosition)
    {
        for (ZombieAgent& zombie : zombies)
        {
            if (!zombie.alive || !zombie.hasBreachLink) continue;
            const bool linked = zombie.breachPosition == breachPosition
                || zombie.otherBreachPosition == breachPosition;
            if (!linked) continue;
            // 생성기가 묶어 준 두 침입구 중 실제로 처음 반응한 쪽을 현재 목표로
            // 고정한다. 그러면 대기 분기의 기존 압박·해제 규칙도 같은 좌표를 쓴다.
            if (zombie.selectedBreachPosition.x < 0)
                zombie.selectedBreachPosition = breachPosition;
            // 소음이 이 침입구까지 닿았으므로 그 뒤의 좀비는 듣고 있다. 창문이
            // 무너지기 전까지 나올 수는 없지만, 무너진 뒤에 경로 재계산 간격만큼
            // 멍하니 서 있지는 않는다.
            WakeZombie(zombie);
        }
    }
}
