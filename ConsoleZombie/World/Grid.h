#pragma once

#include <Core/Types.h>
#include <cstdint>
#include <vector>

namespace Zombie
{
    // 타일 한 칸이 될 수 있는 것 전부. 규칙은 여기가 아니라 Grid.cpp 의 Rules 표에 있다.
    //
    // 값을 추가하면 그 표에도 한 줄을 더해야 하고, 안 더하면 static_assert 가
    // 빌드를 멈춘다.
    enum class Tile : std::uint8_t
    {
        Wall,
        Floor,
        DoorClosed,
        DoorWarning,
        DoorCracked,
        DoorBroken,
        DoorOpen,
        // 플레이어가 절대 못 여는 문. 침입 압력만 통한다. 그래서 자기만의 경고·균열
        // 상태가 필요하다 — 평범한 문 상태를 재사용하면 IsClosedDoor() 가 참이 되어
        // 플레이어에게 E 안내가 뜬다.
        DoorSealed,
        DoorSealedWarning,
        DoorSealedCracked,
        WindowIntact,
        WindowWarning,
        WindowCracked,
        WindowBroken,
        ExitLocked,
        ExitOpen
    };

    bool IsClosedDoor(Tile tile);
    bool IsSealedDoor(Tile tile);
    bool IsSealedWindow(Tile tile);

    // 쿼드트리 리프의 용도. 리프는 항상 열 개다 — 활성 방 일곱, 좀비가 나오는
    // 감염 공간 둘, 그리고 방이 없지만 복도는 지나갈 수 있는 예약 리프 하나.
    enum class LeafRole : std::uint8_t
    {
        Active,
        Infected,
        Reserved
    };

    struct LeafRegion
    {
        Rect region{};
        Rect room{};   // Reserved 는 비어 있다
        LeafRole role = LeafRole::Reserved;
    };

    // Room Graph 의 간선. 양 끝이 Grid::rooms 의 색인이고, rooms 에는 활성 방만
    // 들어 있다. 감염 공간은 복도가 아니라 침입구로 붙는다.
    struct RoomEdge
    {
        int a = 0;
        int b = 0;
    };

    // 인접한 두 타일을 하나의 논리적 개구부로 묶은 것. 복도 출입구와 탈출구가
    // 둘 다 정확히 2타일이라 같은 자료형을 쓴다.
    struct TilePair
    {
        Int2 origin{};        // x 가 더 작은 쪽, 같으면 y 가 더 작은 쪽
        bool spansX = true;   // 쌍이 x 축을 따라 놓였으면 true

        Int2 Second() const
        {
            return { origin.x + (spansX ? 1 : 0), origin.y + (spansX ? 0 : 1) };
        }
        bool Contains(Int2 point) const { return point == origin || point == Second(); }

        friend bool operator==(const TilePair&, const TilePair&) = default;
    };

    // 복도가 방의 벽을 뚫고 들어온 자리. 문은 타일이 아니라 Room Graph 의 간선마다
    // 고르므로, 출입구는 자기를 뚫은 간선과 자기가 들여보내는 방을 기억한다.
    struct Doorway
    {
        TilePair tiles{};
        int edge = 0;         // Grid::roomEdges 의 색인
        int room = 0;         // Grid::rooms 의 색인
    };

    // 감염 공간을 활동 구역에 잇는 침입구와, 그 감염 공간 안쪽의 대기 자리.
    // 침입구가 버티는 동안 감염 공간은 활동 구역에서 도달할 수 없고, 그래서 안에서
    // 기다리는 좀비는 그것이 열려야만 들어올 수 있다.
    struct BreachSpawn
    {
        Int2 breachPosition{};
        Int2 waitingPosition{};
    };

    // 좀비 하나가 시작하는 자리. **맵의 모든 좀비가 이 목록 하나에서 나온다.**
    // 그래야 12마리 예산을 한 곳에서 셀 수 있다.
    struct ZombieSpawn
    {
        Int2 position{};
        // 이 좀비가 자는 감염 공간의 출구 **둘 다**. 활동 구역에 그냥 있는
        // 좀비라면 -1 이다. **어느 쪽이 열려도 깨어난다** — 열린 문 옆에 서 있는
        // 좀비는 자고 있는 좀비가 아니다.
        Int2 breachPosition{ -1, -1 };
        Int2 otherBreachPosition{ -1, -1 };

        bool WaitsBehindBreach() const { return breachPosition.x >= 0; }
    };

    struct AmmoPickup
    {
        Int2 position{};
        int rounds = 1;
    };

    // 타일 저장 + 통행·시야·비용 규칙. 게임의 헌법이다.
    class Grid
    {
    public:
        Grid() = default;
        Grid(int width, int height);

        int Width() const { return width; }
        int Height() const { return height; }
        bool Contains(Int2 point) const;
        Tile Get(Int2 point) const;
        void Set(Int2 point, Tile tile);
        bool IsWalkable(Int2 point) const;
        bool IsOpaque(Int2 point) const;
        float TraversalCost(Int2 point) const;
        float AcousticCost(Int2 point) const;
        std::uint64_t StableHash() const;

        // 타일을 쓸 때마다 올라간다. 배치에 기대는 캐시(음향 필드)가 호출부의
        // 무효화를 믿는 대신 이 값을 비교한다. 한 곳만 빠뜨려도 조용히 틀린 값이 남는다.
        std::uint32_t Revision() const { return revision; }

        std::uint32_t seed = 0;
        // 몇 번째 재시도가 이 맵을 냈는가. Base Seed 가 한 번에 통과했으면 0 이다.
        // 결과 화면이 두 시드와 함께 찍는다 — 남길 만한 맵을 정확히 재현할 수 있게.
        // 메타데이터일 뿐이다. StableHash() 는 타일만 접으므로 맵 해시를 못 움직인다.
        int seedAttempt = 0;
        Int2 playerSpawn{};
        // 시작 방의 바깥쪽 벽 위 두 타일. **두 상태 모두 통행 가능하고 투명하다.**
        // 잠겼는지 여부는 바닥 색만 말한다.
        TilePair exit{};
        Int2 keyPosition{};
        int startRoom = 0;    // rooms 의 색인
        int keyRoom = 0;      // rooms 의 색인
        // 좀비가 하나도 없다고 보장된 활성 방 하나. 시작에서 Room Graph 거리 2 이상에
        // 두어서 찾아내야만 하게 한다.
        int safeRoom = -1;
        std::vector<Rect> rooms;
        std::vector<LeafRegion> leaves;
        std::vector<RoomEdge> roomEdges;
        std::vector<Doorway> doorways;
        std::vector<ZombieSpawn> zombieSpawns;
        std::vector<AmmoPickup> ammoPickups;
        std::vector<BreachSpawn> breachSpawns;
        // 일부러 1타일 폭으로 판 자리 — 침입구 접근로와 통과 창문 뒤의 벽감이다.
        // 복도는 폭 2타일을 지켜야 하는데, 이것들은 복도가 아니다. 검사의 예외 목록.
        std::vector<Int2> narrowTiles;

    private:
        int width = 0;
        int height = 0;
        std::uint32_t revision = 0;
        std::vector<Tile> tiles;
    };
}
