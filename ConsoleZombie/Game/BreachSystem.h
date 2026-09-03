#pragma once

#include <AI/AStar.h>
#include <World/Grid.h>
#include <cstdint>
#include <random>
#include <vector>

namespace Zombie
{
    enum class BreachKind
    {
        // 플레이어가 절대 못 여는 문. 압력만이 통과하고, 그래서 자기만의 타일
        // 상태가 필요하다.
        SealedDoor,
        Window
    };

    enum class BreachState
    {
        Stable,
        Warning,
        Cracked,
        Broken
    };

    struct BreachTuning
    {
        float warningPressure = 30.0f;
        float crackPressure = 60.0f;
        float breakPressure = 100.0f;
        float pressureDecayPerSecond = 3.0f;
        // 마지막 증가 뒤 이만큼은 압력이 그대로 버틴 다음에 빠지기 시작한다.
        // 그래야 1초 간격의 소음이 상쇄되지 않고 쌓인다.
        float decayDelay = 3.0f;
    };

    struct BreachPoint
    {
        Int2 position{};
        BreachKind kind = BreachKind::Window;
        BreachState state = BreachState::Stable;
        float pressure = 0.0f;
        bool active = true;
        float quietTime = 0.0f;
    };

    // 침입구가 상태를 바꾼 사건. RefreshState() 는 타일만 다시 썼기 때문에, 바깥에서
    // 금이 간 것인지 무너진 것인지 알 길이 없었다. 상태 로그가 그것을 알아야 한다.
    struct BreachEvent
    {
        Int2 position{};
        BreachKind kind = BreachKind::Window;
        BreachState state = BreachState::Stable;
    };

    struct NoiseTuning
    {
        // 유효 소음 = max(0, 원본 소음 - 음향 경로 비용 × acousticFalloff).
        float acousticFalloff = 1.5f;
        float pressureScale = 0.5f;
        float existingPressureWeight = 0.02f;
        int maxResponders = 1;
    };

    // 소음 → 압력 → 침입구 파손.
    class BreachSystem
    {
    public:
        explicit BreachSystem(BreachTuning tuning = {}, NoiseTuning noiseTuning = {});

        // 튜닝만 갈아 끼운다. **각 침입구에 이미 쌓인 압력은 건드리지 않는다.**
        // 시스템을 새로 만들면 그것이 날아가는데, 밸런스 파일을 다시 읽었다고
        // 금 간 창문이 조용히 멀쩡해져서는 안 된다.
        void Configure(BreachTuning newTuning, NoiseTuning newNoiseTuning);

        void Initialize(const Grid& grid);
        void Update(float deltaTime, Grid& grid);
        bool AddPressure(Int2 position, float amount, Grid& grid);

        // reactionChance 는 행동마다 다르다. 발소리와 총성이 같은 확률일 이유가 없다.
        std::vector<Int2> ApplyNoise(Vec2 origin, float amount, float reactionChance,
            Grid& grid, std::mt19937& random);

        void Deactivate(Int2 position);

        // 지난 호출 이후의 상태 변화를 넘겨주고 비운다. 그것을 뭐라고 말할지는
        // 받는 쪽이 정한다.
        std::vector<BreachEvent> DrainEvents();

        const std::vector<BreachPoint>& Points() const { return points; }

        // 소리가 origin 에서 breachPosition 까지 가며 치르는 음향 비용.
        // 침입구가 격자 밖이면 무한대다.
        float PathCost(Int2 origin, Int2 breachPosition, const Grid& grid);

        // origin 에서 편 음향 비용 필드 전체. 색인은 y * grid.Width() + x 다.
        //
        // 디버그 히트맵이 이것을 그린다. **다시 계산하지 않고 여기서 받아 가는 것이
        // 요점이다** — 화면에 칠해진 값이 침입구가 압력을 받을지 판정할 때 쓴 바로
        // 그 배열이어야 한다. 두 번째 사본은 게임에 대한 두 번째 의견이 된다.
        const std::vector<float>& AcousticField(Int2 origin, const Grid& grid);

    private:
        BreachPoint* Find(Int2 position);
        void RefreshState(BreachPoint& point, Grid& grid);
        static void ApplyTile(const BreachPoint& point, Grid& grid);
        void RefreshAcoustics(const Grid& grid, Int2 origin);

        BreachTuning tuning;
        NoiseTuning noiseTuning;
        std::vector<BreachPoint> points;
        std::vector<BreachEvent> events;
        // **소리가 난 지점에서** 퍼지는 다익스트라 비용 필드.
        //
        // 침입구 기준이 아니라 소리의 출처 기준이어야 한다. 비용을 치르는 것은
        // 타일에 들어가는 일이므로, 창문에서 키운 필드는 창문 자신의 비용 4를
        // 합계에서 빠뜨린다.
        //
        // 플레이어가 같은 타일에 있고 배치가 그대로면 재사용한다.
        std::vector<float> acousticField;
        Int2 acousticOrigin{ -1, -1 };
        std::uint32_t acousticRevision = 0;
        bool acousticsValid = false;
    };
}
