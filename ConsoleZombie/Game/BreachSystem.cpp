#include "BreachSystem.h"

#include <algorithm>
#include <limits>

namespace Zombie
{
    namespace
    {
        // [침입구 종류][압력 상태] → 그 순간 화면에 서 있는 타일.
        // 행 순서는 BreachKind, 열 순서는 BreachState 그대로다.
        constexpr Tile BreachStateTiles[2][4] = {
            /* SealedDoor */ { Tile::DoorSealed, Tile::DoorSealedWarning,
                               Tile::DoorSealedCracked, Tile::DoorBroken },
            /* Window     */ { Tile::WindowIntact, Tile::WindowWarning,
                               Tile::WindowCracked, Tile::WindowBroken }
        };

        Tile BreachTileFor(BreachKind kind, BreachState state)
        {
            return BreachStateTiles[static_cast<std::size_t>(kind)][static_cast<std::size_t>(state)];
        }
    }

    BreachSystem::BreachSystem(BreachTuning tuning, NoiseTuning noiseTuning)
        : tuning(tuning), noiseTuning(noiseTuning)
    {
        this->tuning.warningPressure = std::max(0.0f, this->tuning.warningPressure);
        this->tuning.crackPressure = std::max(this->tuning.warningPressure + 0.01f, this->tuning.crackPressure);
        this->tuning.breakPressure = std::max(this->tuning.crackPressure + 0.01f, this->tuning.breakPressure);
        this->tuning.pressureDecayPerSecond = std::max(0.0f, this->tuning.pressureDecayPerSecond);
        this->tuning.decayDelay = std::max(0.0f, this->tuning.decayDelay);
        this->noiseTuning.acousticFalloff = std::max(0.0f, this->noiseTuning.acousticFalloff);
        this->noiseTuning.pressureScale = std::max(0.0f, this->noiseTuning.pressureScale);
        this->noiseTuning.existingPressureWeight = std::max(0.0f, this->noiseTuning.existingPressureWeight);
        this->noiseTuning.maxResponders = std::max(1, this->noiseTuning.maxResponders);
    }

    // 튜닝만 갈아 끼운다. 쌓인 압력은 그대로 둔다.
    void BreachSystem::Configure(BreachTuning newTuning, NoiseTuning newNoiseTuning)
    {
        tuning = newTuning;
        noiseTuning = newNoiseTuning;
    }

    // 맵에서 침입구 후보를 뽑아 압력 추적을 시작한다. 판이 새로 시작할 때마다.
    void BreachSystem::Initialize(const Grid& grid)
    {
        // **뒤에 좀비가 나올 곳이 있는 침입구만 후보다.** 평범한 문이나 바닥과
        // 바닥을 잇는 지름길 창문은 아무리 압력을 받아도 침입을 만들어내지 못하므로,
        // 등록하면 ApplyNoise() 의 가중 선택을 묽게 만들 뿐이다.
        points.clear();
        events.clear();
        points.reserve(grid.breachSpawns.size());
        for (const BreachSpawn& spawn : grid.breachSpawns)
        {
            // 위의 표를 거꾸로 읽는다. "안정 상태의 창문은 무슨 타일인가"를 여기서
            // 세 번째로 적으면, 표를 고칠 때 이 줄이 남는다. 갓 생성된 맵의 침입구는
            // 반드시 안정 상태이므로 볼 열은 그 하나뿐이다.
            const Tile tile = grid.Get(spawn.breachPosition);
            if (tile == BreachTileFor(BreachKind::Window, BreachState::Stable))
                points.push_back({ spawn.breachPosition, BreachKind::Window });
            else if (tile == BreachTileFor(BreachKind::SealedDoor, BreachState::Stable))
                points.push_back({ spawn.breachPosition, BreachKind::SealedDoor });
        }
        acousticField.clear();
        acousticsValid = false;
    }

    // 소리가 난 지점에서 음향 비용 필드를 다시 편다.
    //
    // 같은 지점이고 배치도 안 바뀌었으면 다시 계산하지 않는다. Grid::Revision() 이
    // 배치가 움직였는지를 말해주므로, 호출부가 무효화를 기억할 필요가 없다.
    void BreachSystem::RefreshAcoustics(const Grid& grid, Int2 origin)
    {
        if (acousticsValid && acousticRevision == grid.Revision() && acousticOrigin == origin) return;
        acousticField = AStar::FloodCosts(grid, origin, CostField::Acoustic);
        acousticOrigin = origin;
        acousticRevision = grid.Revision();
        acousticsValid = true;
    }

    // 소리가 origin 에서 이 침입구까지 가며 치르는 값.
    float BreachSystem::PathCost(Int2 origin, Int2 breachPosition, const Grid& grid)
    {
        if (!grid.Contains(origin) || !grid.Contains(breachPosition))
            return std::numeric_limits<float>::infinity();
        RefreshAcoustics(grid, origin);
        const std::size_t index =
            static_cast<std::size_t>(breachPosition.y * grid.Width() + breachPosition.x);
        return index < acousticField.size() ? acousticField[index] : std::numeric_limits<float>::infinity();
    }

    // 필드 전체. PathCost 가 한 칸을 꺼내 가는 그 배열을 그대로 내준다.
    //
    // 격자 밖 origin 에서는 빈 배열이다. 0 으로 채운 배열을 돌려주면 **모든 곳에
    // 소리가 닿는 그림**이 되는데, 뜻하는 바의 정반대다.
    const std::vector<float>& BreachSystem::AcousticField(Int2 origin, const Grid& grid)
    {
        static const std::vector<float> none;
        if (!grid.Contains(origin)) return none;
        RefreshAcoustics(grid, origin);
        return acousticField;
    }

    // 시간을 흘린다. 조용한 시간이 유예를 넘긴 침입구부터 압력이 빠진다.
    //
    // 유예가 없으면 1초 간격의 발소리가 매번 상쇄되어 아무것도 안 쌓인다.
    void BreachSystem::Update(float deltaTime, Grid& grid)
    {
        for (BreachPoint& point : points)
        {
            if (!point.active) continue;
            const Tile tile = grid.Get(point.position);
            if ((point.kind == BreachKind::SealedDoor && !IsSealedDoor(tile))
                || (point.kind == BreachKind::Window && !IsSealedWindow(tile)))
            {
                point.active = false;
                continue;
            }

            point.quietTime += deltaTime;
            if (point.state != BreachState::Broken && point.quietTime >= tuning.decayDelay)
                point.pressure = std::max(0.0f, point.pressure - tuning.pressureDecayPerSecond * deltaTime);
            RefreshState(point, grid);
        }
    }

    // 한 침입구에 압력을 직접 더한다. 깨어난 좀비가 자기 침입구를 미는 경로가
    // 이것이다. **이게 없으면 감염 공간은 충분히 큰 소음을 영원히 기다린다.**
    bool BreachSystem::AddPressure(Int2 position, float amount, Grid& grid)
    {
        BreachPoint* point = Find(position);
        if (!point || !point->active || amount <= 0.0f) return false;
        point->pressure += amount;
        point->quietTime = 0.0f;
        RefreshState(*point, grid);
        return true;
    }

    // 소음 하나를 맵에 전한다. 반응한 침입구들의 위치를 돌려준다.
    //
    // 순서가 규칙이다. **사건마다 전체 반응 여부를 한 번만 판정하고**, 그 다음
    // 유효 소음으로 가중 선택해서 침입구를 고른다. 침입구마다 따로 굴리면 침입구가
    // 많을수록 맵이 예민해진다.
    //
    // 전용 난수 생성기를 쓴다. 같은 시드에 같은 입력 흐름이면 같은 반응이 나와야 한다.
    std::vector<Int2> BreachSystem::ApplyNoise(Vec2 origin, float amount, float reactionChance,
        Grid& grid, std::mt19937& random)
    {
        if (amount <= 0.0f) return {};
        const Int2 originTile = ToTile(origin);
        if (!grid.Contains(originTile)) return {};
        RefreshAcoustics(grid, originTile);

        std::vector<std::size_t> candidates;
        std::vector<double> weights;
        std::vector<float> delivered;
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            const BreachPoint& point = points[index];
            if (!point.active) continue;
            const std::size_t tileIndex =
                static_cast<std::size_t>(point.position.y * grid.Width() + point.position.x);
            if (tileIndex >= acousticField.size() || !std::isfinite(acousticField[tileIndex])) continue;

            // 벽은 소리를 끊는 것이 아니라 세금을 매긴다. 충분히 쌓이면 전달할
            // 것이 남지 않고, 그 침입구는 아무 일도 없었던 것이 된다.
            const float effective =
                std::max(0.0f, amount - acousticField[tileIndex] * noiseTuning.acousticFalloff);
            if (effective <= 0.0f) continue;

            const float pressureFactor = 1.0f + point.pressure * noiseTuning.existingPressureWeight;
            candidates.push_back(index);
            weights.push_back(static_cast<double>(pressureFactor * effective));
            delivered.push_back(effective);
        }
        if (candidates.empty()) return {};

        std::bernoulli_distribution reacts(std::clamp(reactionChance, 0.0f, 1.0f));
        if (!reacts(random)) return {};

        std::vector<Int2> selected;
        const int responderCount = std::min(noiseTuning.maxResponders, static_cast<int>(candidates.size()));
        selected.reserve(static_cast<std::size_t>(responderCount));
        for (int responder = 0; responder < responderCount; ++responder)
        {
            std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
            const std::size_t localIndex = choose(random);
            const Int2 position = points[candidates[localIndex]].position;
            const float applied = delivered[localIndex] * noiseTuning.pressureScale;
            if (AddPressure(position, applied, grid)) selected.push_back(position);
            const auto offset = static_cast<std::ptrdiff_t>(localIndex);
            candidates.erase(candidates.begin() + offset);
            weights.erase(weights.begin() + offset);
            delivered.erase(delivered.begin() + offset);
        }
        return selected;
    }

    // 쌓인 상태 변화를 넘겨주고 비운다.
    std::vector<BreachEvent> BreachSystem::DrainEvents()
    {
        std::vector<BreachEvent> drained;
        drained.swap(events);
        return drained;
    }

    // 이 침입구를 후보에서 뺀다. 이미 뚫린 곳은 더 받을 압력이 없다.
    void BreachSystem::Deactivate(Int2 position)
    {
        if (BreachPoint* point = Find(position)) point->active = false;
    }

    // 위치로 침입구를 찾는다. 개수가 셋이라 선형 탐색으로 충분하다.
    BreachPoint* BreachSystem::Find(Int2 position)
    {
        const auto found = std::find_if(points.begin(), points.end(), [position](const BreachPoint& point)
        {
            return point.position == position;
        });
        return found == points.end() ? nullptr : &*found;
    }

    // 압력을 상태로 옮기고, 바뀌었으면 타일과 사건을 갱신한다.
    //
    // Stable → Warning → Cracked → Broken 이고, **경고는 풀릴 수 있지만 균열은
    // 영구적이다.**
    void BreachSystem::RefreshState(BreachPoint& point, Grid& grid)
    {
        BreachState next = point.state;
        if (point.pressure >= tuning.breakPressure)
            next = BreachState::Broken;
        else if (point.state == BreachState::Cracked || point.pressure >= tuning.crackPressure)
            // 2026-08-27 확정: **금은 절대 아물지 않는다.** 풀릴 수 있는 것은 경고뿐이다.
            next = BreachState::Cracked;
        else if (point.pressure >= tuning.warningPressure)
            next = BreachState::Warning;
        else
            next = BreachState::Stable;

        if (next == point.state) return;
        point.state = next;
        ApplyTile(point, grid);
        events.push_back({ point.position, point.kind, point.state });
        if (point.state == BreachState::Broken) point.active = false;
    }

    // 침입구가 각 상태에서 보여주는 타일. 지금 이 표가 유일한 답이고, 초기화가
    // 하는 역방향 조회(타일 → 침입구 종류)도 같은 표를 읽는다.
    //
    // 예전에는 switch 두 벌로 각각 적혀 있었고 초기화가 "안정 상태의 창문/막힌 문은
    // 무슨 타일인가"를 세 번째로 또 적고 있었다. 셋이 어긋나면 침입구가 초기화 때
    // 인식되지 않는다 — 아무 압력도 받지 않는 조용한 창문이 하나 생기는데, 그건
    // 화면에 아무 표시도 없다.
    //
    // 막힌 문 쪽이 평범한 문 상태를 안 쓰는 이유: DoorWarning 을 쓰면
    // IsClosedDoor() 가 참이 되어 플레이어에게 E 안내가 뜬다. 6.9 가 절대 못 여는
    // 문이라고 정한 문에 말이다. Broken 만 공유하는데, 열린 것은 그냥 열린 것이다.
    void BreachSystem::ApplyTile(const BreachPoint& point, Grid& grid)
    {
        grid.Set(point.position, BreachTileFor(point.kind, point.state));
    }
}
