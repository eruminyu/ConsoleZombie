#include "MapGenerator.h"

#include <AI/AStar.h>
#include <World/MapGeneratorDetail.h>

#include <algorithm>
#include <string>
#include <vector>

// 완성된 맵의 최종 검증과, 그 검증이 기대는 타일 정보 되읽기.
//
// **이것은 생성이 아니다.** 난수도 안 건드리고 타일도 안 쓴다. 완성된 맵을 읽고
// 왜 받아들일 수 없는지를 말할 뿐이다. 생성기 안에 두었더니 판정하는 264줄이
// 판정 대상을 만드는 패스들 사이에 끼어 있었다.
//
// 둘 다 MapGenerator 의 static 멤버로 남는다. Validate() 는 GenerateOnce() 가
// 통과해야 하는 관문이고, 자체 테스트는 같은 ExtractRoomAdjacency() 를 부르되
// **비교는 자기 것으로 한다.** 일부러다 — 추출은 공유하되 판정을 공유하지 않는
// 것이 검증기 자신의 버그를 테스트가 잡게 해주는 장치다.
namespace Zombie
{
    using MapDetail::GraphDistances;
    using MapDetail::Orthogonal;

    std::vector<RoomEdge> MapGenerator::ExtractRoomAdjacency(const Grid& grid)
    {
        // 방 바닥이 아니면서 통행 가능한 것 전부 — 출입구, 복도, 침입구 접근로,
        // 통과 창문 뒤의 벽감. 막힌 침입구는 통행 불가라서 그 뒤의 타일들이 자기들만의
        // 덩어리로 떨어지고 감염 공간에만 닿는다. **우리가 원하는 답이 정확히 그것이다.**
        const int width = grid.Width();
        std::vector<int> roomOf(static_cast<std::size_t>(width * grid.Height()), -1);
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            const Rect& room = grid.rooms[index];
            for (int y = room.y; y < room.Bottom(); ++y)
            {
                for (int x = room.x; x < room.Right(); ++x)
                    roomOf[static_cast<std::size_t>(y * width + x)] = static_cast<int>(index);
            }
        }

        std::vector<bool> visited(static_cast<std::size_t>(width * grid.Height()), false);
        std::vector<RoomEdge> edges;
        for (int startY = 0; startY < grid.Height(); ++startY)
        {
            for (int startX = 0; startX < width; ++startX)
            {
                const Int2 seed{ startX, startY };
                const std::size_t seedIndex = static_cast<std::size_t>(startY * width + startX);
                if (visited[seedIndex] || !grid.IsWalkable(seed) || roomOf[seedIndex] >= 0) continue;

                std::vector<int> touched;
                std::vector<Int2> frontier{ seed };
                visited[seedIndex] = true;
                while (!frontier.empty())
                {
                    const Int2 current = frontier.back();
                    frontier.pop_back();
                    for (const Int2 step : Orthogonal)
                    {
                        const Int2 next{ current.x + step.x, current.y + step.y };
                        if (!grid.Contains(next) || !grid.IsWalkable(next)) continue;
                        const std::size_t nextIndex =
                            static_cast<std::size_t>(next.y * width + next.x);
                        if (roomOf[nextIndex] >= 0)
                        {
                            if (std::find(touched.begin(), touched.end(), roomOf[nextIndex]) == touched.end())
                                touched.push_back(roomOf[nextIndex]);
                            continue;
                        }
                        if (visited[nextIndex]) continue;
                        visited[nextIndex] = true;
                        frontier.push_back(next);
                    }
                }

                std::sort(touched.begin(), touched.end());
                for (std::size_t a = 0; a < touched.size(); ++a)
                {
                    for (std::size_t b = a + 1; b < touched.size(); ++b)
                        edges.push_back({ touched[a], touched[b] });
                }
            }
        }
        std::sort(edges.begin(), edges.end(), [](const RoomEdge& lhs, const RoomEdge& rhs)
        {
            if (lhs.a != rhs.a) return lhs.a < rhs.a;
            return lhs.b < rhs.b;
        });
        edges.erase(std::unique(edges.begin(), edges.end(), [](const RoomEdge& lhs, const RoomEdge& rhs)
        {
            return lhs.a == rhs.a && lhs.b == rhs.b;
        }), edges.end());
        return edges;
    }

    namespace
    {
        // Room Graph 를 그래프로서 본다 — 존재하는가, 간선이 온전한가, 한 방에
        // 몰리지 않았는가, 개수가 계획과 맞는가, 그리고 간선마다 복도가 생겼는가.
        std::string CheckRoomGraph(const Grid& grid, const MapGenerationSettings& settings)
        {
            if (grid.rooms.empty()) return "no active rooms";

            std::vector<int> degree(grid.rooms.size(), 0);
            for (const RoomEdge& edge : grid.roomEdges)
            {
                if (edge.a == edge.b) return "self edge in the Room Graph";
                if (edge.a < 0 || edge.b < 0
                    || edge.a >= static_cast<int>(grid.rooms.size())
                    || edge.b >= static_cast<int>(grid.rooms.size()))
                {
                    return "Room Graph edge points outside the active rooms";
                }
                ++degree[static_cast<std::size_t>(edge.a)];
                ++degree[static_cast<std::size_t>(edge.b)];
            }

            // Kruskal 은 차수를 스스로 제한하지 않는다. 별 모양 트리가 나오면 방
            // 하나가 3을 훌쩍 넘길 수 있고, 기획은 그것을 무효 시드라고 한다.
            if (std::any_of(degree.begin(), degree.end(),
                [&settings](int value) { return value > settings.maxRoomDegree; }))
            {
                return "a room exceeds the maximum Room Graph degree";
            }
            if (static_cast<int>(grid.roomEdges.size())
                != static_cast<int>(grid.rooms.size()) - 1 + settings.extraRoomEdges)
            {
                return "Room Graph edge count is wrong";
            }

            // 간선 하나가 양쪽 방에 출입구를 하나씩 낸다. 목록이 짧다는 것이 곧
            // 복도 탐색이 실패했다는 신호라, 별도 플래그가 필요 없다.
            if (grid.doorways.size() != grid.roomEdges.size() * 2)
            {
                return "an edge has no two-tile-wide corridor route";
            }
            return {};
        }

        // 한 판이 시작하고 끝나는 자리. 열쇠는 **서로 다른 두 자로 재서** 멀어야
        // 한다 — 그래프 홉 수와 A* 비용. 하나만 보면 배치로 속일 수 있다.
        std::string CheckStartExitKey(const Grid& grid, const MapGenerationSettings& settings)
        {
            if (grid.startRoom < 0 || grid.startRoom >= static_cast<int>(grid.rooms.size())
                || grid.keyRoom < 0 || grid.keyRoom >= static_cast<int>(grid.rooms.size()))
            {
                return "start or key room is missing";
            }
            if (grid.Get(grid.exit.origin) != Tile::ExitLocked
                || grid.Get(grid.exit.Second()) != Tile::ExitLocked)
            {
                return "the exit is not two locked tiles";
            }
            const std::vector<int> graphDistance =
                GraphDistances(grid.roomEdges, static_cast<int>(grid.rooms.size()), grid.startRoom);
            if (graphDistance[static_cast<std::size_t>(grid.keyRoom)] < 3)
            {
                return "the key room is fewer than three Room Graph edges from the start";
            }
            const auto keyPath = AStar::FindPath(grid, grid.exit.origin, grid.keyPosition);
            if (keyPath.empty() || AStar::PathCost(grid, keyPath) < settings.minimumKeyPathCost)
            {
                return "the key is closer than the minimum path cost";
            }
            return {};
        }

        // 6.9 와 6.10 을 예산이 아니라 **개수**로 확인한다 — 문, 침입구, 통과 창문
        // 하나, 좀비 열둘, 빈 방 하나, 그리고 설정이 요구한 만큼의 추가 탄약.
        //
        // **탄약 총량은 여기서 안 본다.** 예전에는 상수 8이 여기 적혀 있었고, 그것이
        // 튜닝된 ini 를 통째로 못 굴러가게 만들었다 — 2026-08-29 에 extra_ammo_rounds
        // 가 5가 되자 예산 6에 걸려 **모든 시드가 거절됐고**, 화면에 나온 말은 원인에서
        // 세 단계 떨어진 "맵 생성 실패"였다. 총량은 설계 규칙이지 맵의 구조 불변식이
        // 아니므로 숫자를 읽는 곳(Balance.cpp 의 CheckCombinations)에서 한 번 본다.
        std::string CheckOpeningsAndBudgets(const Grid& grid, const MapGenerationSettings& settings)
        {
            // 6.9 의 개수 — 문 넷, 감염 공간마다 침입구 하나씩이되 종류는 하나씩
            // 갈라서, 그리고 맵 전체에 통과 창문 하나.
            int doorTiles = 0;
            int sealedDoors = 0;
            int intactWindows = 0;
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Tile tile = grid.Get({ x, y });
                    if (tile == Tile::DoorClosed) ++doorTiles;
                    else if (tile == Tile::DoorSealed) ++sealedDoors;
                    else if (tile == Tile::WindowIntact) ++intactWindows;
                }
            }
            if (doorTiles != settings.doorCount * 2) return "the map does not hold exactly four plain doors";
            if (grid.breachSpawns.size() != 3)
            {
                return "one infected space must open two ways and the other one";
            }
            if (sealedDoors != 2) return "the breaches are not two sealed doors and one window";
            if (intactWindows != 1 + settings.traversalWindowCount)
            {
                return "the map does not hold one breach window and one traversal window";
            }

            if (static_cast<int>(grid.zombieSpawns.size()) != settings.zombieCount)
            {
                return "the zombie budget is not met";
            }
            if (grid.safeRoom < 0 || grid.safeRoom >= static_cast<int>(grid.rooms.size()))
            {
                return "no room was left empty of zombies";
            }
            int extraRounds = 0;
            for (const AmmoPickup& pickup : grid.ammoPickups) extraRounds += pickup.rounds;
            if (grid.ammoPickups.size() != 2 || extraRounds != settings.extraAmmoRounds)
            {
                // 문구에 숫자를 안 적는다. 예전에는 설정값과 비교하면서 "세 발"이라고
                // 말했고, 설정이 움직이는 순간 실패 문구가 거짓말이 됐다.
                return "the extra ammunition is not two bundles totalling the configured rounds";
            }
            // 여기 남는 것은 **생성기 자신이 틀릴 수 있는 부분 하나뿐이다** —
            // 시킨 대로 놓았는가. 그 설정이 게임으로서 말이 되는가는 설정을 읽는
            // 곳이 본다.
            return {};
        }

        // 리프가 루트를 **정확히** 덮어야 한다. 겹치면 방 둘이 땅을 공유하게 되고,
        // 틈이 나면 아무도 도달할 수 없는 맵 조각이 남는다.
        std::string CheckLeafTiling(const Grid& grid, const MapGenerationSettings& settings)
        {
            // 6.11 — 넓이 합과 겹침을 둘 다 본다. 넓이만 맞으면 겹친 만큼의 틈이
            // 상쇄되어 통과해버린다.
            long long leafArea = 0;
            for (std::size_t first = 0; first < grid.leaves.size(); ++first)
            {
                const Rect& lhs = grid.leaves[first].region;
                leafArea += static_cast<long long>(lhs.width) * lhs.height;
                for (std::size_t second = first + 1; second < grid.leaves.size(); ++second)
                {
                    const Rect& rhs = grid.leaves[second].region;
                    if (lhs.x < rhs.Right() && rhs.x < lhs.Right()
                        && lhs.y < rhs.Bottom() && rhs.y < lhs.Bottom())
                    {
                        return "two quadtree leaves overlap";
                    }
                }
            }
            if (leafArea != static_cast<long long>(settings.width - 2) * (settings.height - 2))
            {
                return "the quadtree leaves do not tile the root exactly";
            }
            return {};
        }

        // 연결돼 있는가, 고리가 둘인가, 그리고 **6.11 의 머리 항목** — 완성된 타일이
        // 잇는 방 쌍이 계획이 잇겠다고 한 방 쌍과 정확히 같은가.
        std::string CheckGraphAgreement(const Grid& grid, const MapGenerationSettings& settings)
        {
            // 연결돼 있고, 트리 위에 독립 고리가 정확히 둘 있어야 한다.
            const std::vector<int> reach =
                GraphDistances(grid.roomEdges, static_cast<int>(grid.rooms.size()), 0);
            if (std::any_of(reach.begin(), reach.end(), [](int value) { return value < 0; }))
            {
                return "the Room Graph is not connected";
            }
            const int independentLoops = static_cast<int>(grid.roomEdges.size())
                - static_cast<int>(grid.rooms.size()) + 1;
            if (independentLoops != settings.extraRoomEdges) return "the Room Graph has the wrong number of loops";

            // 계획을 믿지 않고 완성된 타일을 되읽는 쪽. 4단계의 느슨함을 드러낸 검사다.
            std::vector<RoomEdge> planned = grid.roomEdges;
            for (RoomEdge& edge : planned)
            {
                if (edge.a > edge.b) std::swap(edge.a, edge.b);
            }
            std::sort(planned.begin(), planned.end(), [](const RoomEdge& lhs, const RoomEdge& rhs)
            {
                if (lhs.a != rhs.a) return lhs.a < rhs.a;
                return lhs.b < rhs.b;
            });
            const std::vector<RoomEdge> actual = MapGenerator::ExtractRoomAdjacency(grid);
            if (actual.size() != planned.size()
                || !std::equal(actual.begin(), actual.end(), planned.begin(),
                    [](const RoomEdge& lhs, const RoomEdge& rhs) { return lhs.a == rhs.a && lhs.b == rhs.b; }))
            {
                return "the tiles join rooms the Room Graph never planned";
            }
            return {};
        }

        // 필수 동선이 총알이나 아직 없는 열쇠 없이 걸어질 수 있는가, 그리고 모든
        // 좀비가 움직일 수 있는가 — 자는 좀비는 자기 침입구가 어딘가로 이어져야 하고,
        // 돌아다니는 좀비는 자기 방을 나올 수 있어야 한다.
        //
        // 설정에서 아무것도 안 받는 유일한 검사다. 맵 자신에게 "여기 있는 것들이
        // 아직 움직일 수 있느냐"를 묻는다.
        std::string CheckEverythingCanMove(const Grid& grid, const MapGenerationSettings&)
        {
            // 필수 동선의 어느 것도 총알이나 아직 없는 열쇠를 요구해서는 안 된다.
            // A* 가 이미 막힌 타일을 거절하므로 이건 이중 안전장치다 — 비용은
            // 통행 가능이라고 말하는데 뜻은 아닌 타일이 나중에 생길 경우를 위한 것.
            std::vector<Int2> required = AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition);
            const std::vector<Int2> toExit = AStar::FindPath(grid, grid.keyPosition, grid.exit.origin);
            if (required.empty() || toExit.empty()) return "the required run is broken";
            required.insert(required.end(), toExit.begin(), toExit.end());
            for (const Int2 tile : required)
            {
                const Tile value = grid.Get(tile);
                const bool allowed = value == Tile::Floor || value == Tile::DoorClosed
                    || value == Tile::DoorOpen || value == Tile::ExitLocked || value == Tile::ExitOpen;
                if (!allowed) return "the required run crosses a tile the player cannot simply walk";
            }

            // 모든 좀비가 움직일 수 있어야 한다. 자는 좀비는 자기 침입구가 어딘가로
            // 이어져야 하고, 돌아다니는 좀비는 자기 방을 나올 수 있어야 한다.
            for (const ZombieSpawn& spawn : grid.zombieSpawns)
            {
                if (spawn.WaitsBehindBreach())
                {
                    Grid opened = grid;
                    opened.Set(spawn.breachPosition, Tile::DoorBroken);
                    if (AStar::FindPath(opened, spawn.position, opened.playerSpawn).empty())
                    {
                        return "a sleeping zombie cannot reach the play area once its breach opens";
                    }
                    continue;
                }
                if (AStar::FindPath(grid, spawn.position, grid.playerSpawn).empty())
                {
                    return "a loose zombie cannot reach the player";
                }
            }
            return {};
        }
    }

    // 이 맵이 왜 받아들일 수 없는지, 또는 유효하면 빈 문자열.
    //
    // GenerateOnce() 가 통과해야 하는 관문이다. 여기서 거절된 시드는 고쳐지지 않고
    // 버려진다.
    std::string MapGenerator::Validate(const Grid& grid, const MapGenerationSettings& settings)
    {
        // **이 순서이고, 순서가 중요하다.** 싼 구조 검사가 A* 를 도는 검사보다 앞에
        // 오고, 폐기된 시드에 기록되는 사유는 **먼저 걸린 것**이다. 순서를 바꾸면
        // 유효한 맵은 그대로여도 폐기 사유 분포가 달라진다.
        using Check = std::string (*)(const Grid&, const MapGenerationSettings&);
        constexpr Check checks[] = {
            CheckRoomGraph,
            CheckStartExitKey,
            CheckOpeningsAndBudgets,
            CheckLeafTiling,
            CheckGraphAgreement,
            CheckEverythingCanMove,
        };
        for (const Check check : checks)
        {
            std::string reason = check(grid, settings);
            if (!reason.empty()) return reason;
        }
        return {};
    }
}
