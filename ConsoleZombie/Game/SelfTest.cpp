#include "Game.h"

#include <AI/AStar.h>
#include <Platform/TextRaster.h>
#include <Platform/WavFile.h>
#include <Render/RenderDigest.h>
#include <World/MapGenerator.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// 게임이 자기 자신에 대해 증명하도록 요구받을 수 있는 것 전부를, 게임 자체에서
// 떼어 놓았다. 이것이 한때 Game.cpp 의 2,898줄 중 2,077줄이었다 — **플레이어가
// 어떻게 움직이는지를 찾는 사람이 이 프로젝트가 여태 주장한 모든 불변식을 스크롤로
// 지나가야 했다.**
//
// 두 함수는 Game 의 멤버로 남는다. **일부러 private 상태 안으로 들어가기 때문이다.**
// 공개 표면만 볼 수 있는 테스트는 여기서 실제로 망가졌던 것들을 검사할 수 없다.
namespace Zombie
{
    // 자체 테스트를 static 검사 한 덩어리로 묶은 것. 그래서 검사 중 어느 것도
    // 게임의 헤더에 선언될 필요가 없다. Game 이 이것을 friend 로 부르고,
    // 장치의 전부가 그 한 줄이다.
    struct SelfTestAccess
    {
        // 리프 열 개와 세 역할, 그래프로서의 Room Graph, 방 크기, 그리고 깊이 1
        // 사분면마다 활성 방이 하나씩 있는가.
        static int CheckSeedShape(std::uint32_t seed, const Grid& grid,
            const std::vector<MapGenerationSnapshot>& trace)
        {
            if (trace.size() != 9)
            {
                std::cerr << "FAIL seed " << seed << ": expected nine generation snapshots\n";
                return 1;
            }
            // 리프 열 개가 활성 7 · 감염 2 · 예약 1 로 갈린다. grid.rooms 에는
            // 활성만 들어 있다 — 그것이 Room Graph 의 노드 집합이기 때문이다.
            if (grid.leaves.size() != 10 || grid.rooms.size() != 7)
            {
                std::cerr << "FAIL seed " << seed << ": expected ten quadtree leaves and seven active rooms\n";
                return 1;
            }
            int activeLeaves = 0;
            int infectedLeaves = 0;
            int reservedLeaves = 0;
            std::array<int, 4> quadrantActive{};
            for (const LeafRegion& leaf : grid.leaves)
            {
                if (leaf.role == LeafRole::Active) ++activeLeaves;
                else if (leaf.role == LeafRole::Infected) ++infectedLeaves;
                else ++reservedLeaves;
                if (leaf.role != LeafRole::Active) continue;
                // 이 리프가 어느 깊이 1 사분면에 앉아 있는가.
                const std::size_t quadrant =
                    static_cast<std::size_t>(leaf.region.x * 2 / std::max(1, grid.Width()))
                    + static_cast<std::size_t>(leaf.region.y * 2 / std::max(1, grid.Height())) * 2;
                if (quadrant < quadrantActive.size()) ++quadrantActive[quadrant];
            }
            if (activeLeaves != 7 || infectedLeaves != 2 || reservedLeaves != 1)
            {
                std::cerr << "FAIL seed " << seed
                    << ": leaf roles must be seven active, two infected and one reserved\n";
                return 1;
            }
            // Room Graph — 활성 방 일곱을 잇는 신장 트리에 고리 몇 개.
            const int expectedEdges = 6 + 2;
            if (static_cast<int>(grid.roomEdges.size()) != expectedEdges)
            {
                std::cerr << "FAIL seed " << seed << ": Room Graph must hold six tree edges and two extras\n";
                return 1;
            }
            std::array<int, 7> degree{};
            for (const RoomEdge& edge : grid.roomEdges)
            {
                if (edge.a == edge.b)
                {
                    std::cerr << "FAIL seed " << seed << ": Room Graph must not contain a self edge\n";
                    return 1;
                }
                if (edge.a < 0 || edge.b < 0
                    || edge.a >= static_cast<int>(grid.rooms.size())
                    || edge.b >= static_cast<int>(grid.rooms.size()))
                {
                    std::cerr << "FAIL seed " << seed << ": Room Graph edge points outside the active rooms\n";
                    return 1;
                }
                ++degree[static_cast<std::size_t>(edge.a)];
                ++degree[static_cast<std::size_t>(edge.b)];
            }
            for (std::size_t first = 0; first < grid.roomEdges.size(); ++first)
            {
                for (std::size_t second = first + 1; second < grid.roomEdges.size(); ++second)
                {
                    const RoomEdge& left = grid.roomEdges[first];
                    const RoomEdge& right = grid.roomEdges[second];
                    if ((left.a == right.a && left.b == right.b)
                        || (left.a == right.b && left.b == right.a))
                    {
                        std::cerr << "FAIL seed " << seed << ": Room Graph must not repeat an edge\n";
                        return 1;
                    }
                }
            }
            if (std::any_of(degree.begin(), degree.end(),
                [](int value) { return value > 3; }))
            {
                std::cerr << "FAIL seed " << seed << ": no room may exceed a Room Graph degree of three\n";
                return 1;
            }

            // 방 크기는 리프가 얼마나 크든 고정된 범위이고, RoomRegion 안에 앉으므로
            // 이웃한 방이 벽을 공유하지 않는다.
            for (const LeafRegion& leaf : grid.leaves)
            {
                if (leaf.role == LeafRole::Reserved)
                {
                    if (leaf.room.width != 0 || leaf.room.height != 0)
                    {
                        std::cerr << "FAIL seed " << seed << ": a reserved leaf must not hold a room\n";
                        return 1;
                    }
                    continue;
                }
                const Rect& room = leaf.room;
                if (room.width < 6 || room.width > 10 || room.height < 6 || room.height > 10)
                {
                    std::cerr << "FAIL seed " << seed << ": room floor must stay within 6x6 to 10x10\n";
                    return 1;
                }
                if (std::abs(room.width - room.height) > 3)
                {
                    std::cerr << "FAIL seed " << seed << ": room width and height may differ by at most 3\n";
                    return 1;
                }
                if (room.x < leaf.region.x + 1 || room.y < leaf.region.y + 1
                    || room.Right() > leaf.region.Right() - 1
                    || room.Bottom() > leaf.region.Bottom() - 1)
                {
                    std::cerr << "FAIL seed " << seed << ": room must stay inside its RoomRegion\n";
                    return 1;
                }
            }

            // 깊이 1 사분면마다 활성 방이 있어야 한다. 아니면 맵의 한 구석이
            // Room Graph 로 도달할 수 있는 것이 아무것도 없는 채로 남는다.
            if (std::any_of(quadrantActive.begin(), quadrantActive.end(),
                [](int count) { return count == 0; }))
            {
                std::cerr << "FAIL seed " << seed << ": each depth-1 quadrant needs an active room\n";
                return 1;
            }
            return 0;
        }

        // 간선당 출입구 둘, 단단한 바깥 링, 폭 2가 유지돼야 하는 곳에서 유지되는가,
        // 그리고 감염 공간이 **침입구로만** 닿을 수 있는가.
        static int CheckSeedCorridors(std::uint32_t seed, const Grid& grid)
        {
            // Room Graph 간선 하나가 양쪽 방에 2타일 출입구를 하나씩 낸다.
            // **목록이 짧다는 것이 곧 복도 탐색이 길을 못 찾았다는 보고**라,
            // 개수가 가장 먼저 붙들어야 할 것이다.
            if (grid.doorways.size() != grid.roomEdges.size() * 2)
            {
                std::cerr << "FAIL seed " << seed
                    << ": every Room Graph edge needs a doorway in each of its two rooms\n";
                return 1;
            }
            for (const Doorway& doorway : grid.doorways)
            {
                if (doorway.edge < 0 || doorway.edge >= static_cast<int>(grid.roomEdges.size())
                    || doorway.room < 0 || doorway.room >= static_cast<int>(grid.rooms.size())
                    || !grid.IsWalkable(doorway.tiles.origin)
                    || !grid.IsWalkable(doorway.tiles.Second()))
                {
                    std::cerr << "FAIL seed " << seed
                        << ": a doorway must be two walkable tiles belonging to a real edge\n";
                    return 1;
                }
            }

            // 예약된 바깥 링이 복도가 맵을 벗어나는 것을 막는다.
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    if (x != 0 && y != 0 && x != grid.Width() - 1 && y != grid.Height() - 1) continue;
                    if (grid.Get({ x, y }) == Tile::Wall) continue;
                    std::cerr << "FAIL seed " << seed << ": the map border must stay solid wall\n";
                    return 1;
                }
            }

            // 어디서나 폭 2 — 통행 가능한 타일 하나하나가 통행 가능한 2×2 블록에
            // 속한다. **유일한 예외가 침입구 접근로와 통과 창문의 벽감**인데,
            // 그것들은 복도가 아니라 기어 다니는 굴이고 생성기가 판 자리를 하나씩
            // 기록해 둔다.
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Int2 tile{ x, y };
                    if (!grid.IsWalkable(tile)) continue;
                    if (std::find(grid.narrowTiles.begin(), grid.narrowTiles.end(), tile)
                        != grid.narrowTiles.end())
                    {
                        continue;
                    }
                    bool thick = false;
                    for (int cornerY = -1; cornerY <= 0 && !thick; ++cornerY)
                    {
                        for (int cornerX = -1; cornerX <= 0 && !thick; ++cornerX)
                        {
                            thick = grid.IsWalkable({ x + cornerX, y + cornerY })
                                && grid.IsWalkable({ x + cornerX + 1, y + cornerY })
                                && grid.IsWalkable({ x + cornerX, y + cornerY + 1 })
                                && grid.IsWalkable({ x + cornerX + 1, y + cornerY + 1 });
                        }
                    }
                    if (thick) continue;
                    std::cerr << "FAIL seed " << seed << ": a corridor pinched below two tiles wide\n";
                    return 1;
                }
            }

            // 감염 공간은 침입구로만 열린다. 복도가 거기 스치기만 해도
            // **그 안의 좀비가 필수 동선 위로 쏟아진다.**
            for (const LeafRegion& leaf : grid.leaves)
            {
                if (leaf.role != LeafRole::Infected) continue;
                if (AStar::FindPath(grid, grid.playerSpawn, leaf.room.Center()).empty()) continue;
                std::cerr << "FAIL seed " << seed
                    << ": an infected space must stay sealed off from the play area\n";
                return 1;
            }
            return 0;
        }

        // 평범한 문 넷, 감염 공간마다 종류가 갈린 침입구 둘, 그중 정확히 하나만
        // 양방향, 통과 창문 하나, 그리고 **실제로 뒤에 뭔가 기다리는 곳에서만**
        // 압력이 쌓이는가.
        //
        // 같은 시드가 같은 맵을 두 번 만드는지도 여기서 본다. 두 번째 사본을
        // 비교할 값어치가 처음 생기는 자리가 여기이기 때문이다.
        static int CheckSeedOpenings(std::uint32_t seed, const Grid& grid, const Grid& repeated)
        {
            // 문이 복도 출입구에 걸리게 됐으므로, 출입구를 잃으면 닫힌 문이 하나도
            // 없는 맵이 되고 **상호작용 검사들이 조용히 속이 빈다.**
            bool anyClosedDoor = false;
            for (int y = 0; y < grid.Height() && !anyClosedDoor; ++y)
            {
                for (int x = 0; x < grid.Width() && !anyClosedDoor; ++x)
                {
                    anyClosedDoor = IsClosedDoor(grid.Get({ x, y }));
                }
            }
            if (!anyClosedDoor)
            {
                std::cerr << "FAIL seed " << seed << ": a map with no closed door has nothing to open with E\n";
                return 1;
            }
            if (grid.StableHash() != repeated.StableHash() || !(grid.keyPosition == repeated.keyPosition))
            {
                std::cerr << "FAIL seed " << seed << ": generation is not deterministic\n";
                return 1;
            }
            if (grid.breachSpawns.empty())
            {
                std::cerr << "FAIL seed " << seed << ": no breach connection into an infected space\n";
                return 1;
            }
            // 감염 공간마다 침입구 둘, 서로 다른 면에, 막힌 문 하나와 창문 하나씩.
            // 플레이 테스트 2026-08-27 이 6.9 의 침입구 하나를 대체했다 —
            // 들어오는 길이 하나면 감염 공간이 구석 주머니처럼 느껴졌다.
            if (grid.breachSpawns.size() != 3)
            {
                std::cerr << "FAIL seed " << seed
                    << ": one infected space must open two ways and the other one\n";
                return 1;
            }
            int sealedDoorBreaches = 0;
            int windowBreaches = 0;
            for (const BreachSpawn& spawn : grid.breachSpawns)
            {
                const Tile breach = grid.Get(spawn.breachPosition);
                if (breach == Tile::DoorSealed) ++sealedDoorBreaches;
                else if (breach == Tile::WindowIntact) ++windowBreaches;
                else
                {
                    std::cerr << "FAIL seed " << seed
                        << ": a breach must start as a sealed door or an intact window\n";
                    return 1;
                }
                const bool insideInfected = std::any_of(grid.leaves.begin(), grid.leaves.end(),
                    [&spawn](const LeafRegion& leaf)
                    {
                        return leaf.role == LeafRole::Infected && leaf.room.Contains(spawn.waitingPosition);
                    });
                if (!insideInfected || !grid.IsWalkable(spawn.waitingPosition))
                {
                    std::cerr << "FAIL seed " << seed
                        << ": a breach must lead into the floor of an infected space\n";
                    return 1;
                }
                if (!AStar::FindPath(grid, grid.playerSpawn, spawn.waitingPosition).empty())
                {
                    std::cerr << "FAIL seed " << seed << ": a sealed breach must isolate its infected space\n";
                    return 1;
                }
                Grid opened = grid;
                opened.Set(spawn.breachPosition, Tile::DoorBroken);
                if (AStar::FindPath(opened, spawn.waitingPosition, opened.playerSpawn).empty())
                {
                    std::cerr << "FAIL seed " << seed << ": a broken breach must open the intrusion route\n";
                    return 1;
                }
            }
            if (sealedDoorBreaches != 2 || windowBreaches != 1)
            {
                std::cerr << "FAIL seed " << seed
                    << ": the three breaches must be two sealed doors and one window\n";
                return 1;
            }
            // 감염 공간 중 **정확히 하나만** 양방향이고, 그것은 맵 경계에 닿지 않는
            // 리프의 것이다. 경계에 닿는 리프는 두 면이 아무것도 없는 바깥을 향하므로
            // 거기 두 번째 침입구를 뚫으면 나올 곳이 없다.
            int twoWaySpaces = 0;
            for (const LeafRegion& leaf : grid.leaves)
            {
                if (leaf.role != LeafRole::Infected) continue;
                std::vector<Int2> mine;
                int sealedHere = 0;
                for (const BreachSpawn& spawn : grid.breachSpawns)
                {
                    if (!leaf.room.Contains(spawn.waitingPosition)) continue;
                    mine.push_back(spawn.breachPosition);
                    if (grid.Get(spawn.breachPosition) == Tile::DoorSealed) ++sealedHere;
                }
                const bool reachesBorder = leaf.region.x == 1 || leaf.region.y == 1
                    || leaf.region.Right() == grid.Width() - 1
                    || leaf.region.Bottom() == grid.Height() - 1;
                if (mine.size() != (reachesBorder ? 1u : 2u))
                {
                    std::cerr << "FAIL seed " << seed
                        << ": only an interior infected space may open two ways\n";
                    return 1;
                }
                if (mine.size() == 2)
                {
                    ++twoWaySpaces;
                    if (sealedHere != 1 || (mine[0].x == mine[1].x && mine[0].y == mine[1].y))
                    {
                        std::cerr << "FAIL seed " << seed
                            << ": the two-way space needs one sealed door and one window on different tiles\n";
                        return 1;
                    }
                }
                else if (sealedHere != 1)
                {
                    std::cerr << "FAIL seed " << seed
                        << ": a one-way infected space must open as a sealed door\n";
                    return 1;
                }
            }
            if (twoWaySpaces != 1)
            {
                std::cerr << "FAIL seed " << seed << ": exactly one infected space opens two ways\n";
                return 1;
            }

            // 서로 다른 간선 넷에 평범한 문 넷, 각각 한쪽 끝에만, 한 방에 셋은
            // 절대 안 된다. **막힌 문은 이 넷에 포함되지 않고 절대 열리지 않는다.**
            std::vector<int> doorEdges;
            std::vector<int> doorsInRoom(grid.rooms.size(), 0);
            int doorTiles = 0;
            int sealedDoorTiles = 0;
            int intactWindowTiles = 0;
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Tile tile = grid.Get({ x, y });
                    if (tile == Tile::DoorClosed) ++doorTiles;
                    else if (tile == Tile::DoorSealed) ++sealedDoorTiles;
                    else if (tile == Tile::WindowIntact) ++intactWindowTiles;
                }
            }
            for (const Doorway& doorway : grid.doorways)
            {
                if (grid.Get(doorway.tiles.origin) != Tile::DoorClosed) continue;
                doorEdges.push_back(doorway.edge);
                ++doorsInRoom[static_cast<std::size_t>(doorway.room)];
            }
            std::sort(doorEdges.begin(), doorEdges.end());
            if (doorTiles != 8 || doorEdges.size() != 4
                || std::adjacent_find(doorEdges.begin(), doorEdges.end()) != doorEdges.end())
            {
                std::cerr << "FAIL seed " << seed
                    << ": four plain doors must sit on four distinct edges, one end each\n";
                return 1;
            }
            if (std::any_of(doorsInRoom.begin(), doorsInRoom.end(), [](int count) { return count > 2; }))
            {
                std::cerr << "FAIL seed " << seed << ": no room may carry more than two plain doors\n";
                return 1;
            }
            if (sealedDoorTiles != 2 || intactWindowTiles != 2)
            {
                std::cerr << "FAIL seed " << seed
                    << ": the map needs two sealed doors, one breach window and one traversal window\n";
                return 1;
            }
            // 통과 창문은 **어느 침입구도 소유하지 않은** 멀쩡한 창문이다.
            // 존재해야 하고, 침입구로 등록되어서는 절대 안 된다.
            int traversalWindows = 0;
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Int2 tile{ x, y };
                    if (grid.Get(tile) != Tile::WindowIntact) continue;
                    const bool owned = std::any_of(grid.breachSpawns.begin(), grid.breachSpawns.end(),
                        [tile](const BreachSpawn& spawn) { return spawn.breachPosition == tile; });
                    if (!owned) ++traversalWindows;
                }
            }
            if (traversalWindows != 1)
            {
                std::cerr << "FAIL seed " << seed << ": exactly one traversal window, owned by no breach\n";
                return 1;
            }
            // 막힌 문은 걷는 모든 것에게 벽이고 E 안내를 내주지 않는다.
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Int2 tile{ x, y };
                    if (!IsSealedDoor(grid.Get(tile))) continue;
                    if (!IsClosedDoor(grid.Get(tile)) && !grid.IsWalkable(tile) && grid.IsOpaque(tile)) continue;
                    std::cerr << "FAIL seed " << seed
                        << ": a sealed door must block movement and sight and never open with E\n";
                    return 1;
                }
            }
            // **뒤에 감염 공간이 있는 침입구만** 압력을 모을 수 있다. 통과 창문과
            // 평범한 문 넷은 침입을 만들어내지 못하므로, 등록하면 ApplyNoise 의
            // 가중 선택을 묽게 만들 뿐이다.
            BreachSystem candidateTest;
            candidateTest.Initialize(grid);
            if (candidateTest.Points().size() != grid.breachSpawns.size())
            {
                std::cerr << "FAIL seed " << seed << ": breach candidates must match the generated intrusion windows\n";
                return 1;
            }
            for (const BreachPoint& point : candidateTest.Points())
            {
                const bool linked = std::any_of(grid.breachSpawns.begin(), grid.breachSpawns.end(),
                    [point](const BreachSpawn& spawn) { return spawn.breachPosition == point.position; });
                if (!linked || (point.kind != BreachKind::Window && point.kind != BreachKind::SealedDoor))
                {
                    std::cerr << "FAIL seed " << seed << ": only intrusion windows may collect breach pressure\n";
                    return 1;
                }
            }
            return 0;
        }

        // 맵이 무엇을 담고 있고 놀 수 있는가 — 좀비 분포, 추가 탄약, 타일이 계획과
        // 맞는가, 리프가 루트를 덮는가, 총알 없이 걸어지는가, 탈출구와 열쇠가 어디에
        // 있는가.
        //
        // 넷이 아니라 한 함수인 이유는 **계산을 공유하기 때문이다.** 시작 방에서의
        // 홉 거리와 탈출구→열쇠 경로를, 앞뒤의 검사들이 각각 읽는다.
        static int CheckSeedContents(std::uint32_t seed, const Grid& grid,
            const MapGenerationSettings& mapSettings, Game& collisionProbe)
        {
            // 목록 하나에서 나오는 좀비 열둘. 감염 공간마다 셋씩 자고 나머지 여섯이
            // 방들에 흩어진다.
            if (grid.zombieSpawns.size() != 12)
            {
                std::cerr << "FAIL seed " << seed << ": the map must hold exactly twelve zombies\n";
                return 1;
            }
            // 시작 방에서의 그래프 거리(간선 수). 좀비 분포와 열쇠 깊이가 둘 다
            // 이 단위로 적혀 있다.
            std::vector<int> graphDistance(grid.rooms.size(), -1);
            graphDistance[static_cast<std::size_t>(grid.startRoom)] = 0;
            for (std::vector<int> frontier{ grid.startRoom }; !frontier.empty(); )
            {
                std::vector<int> next;
                for (const int node : frontier)
                {
                    for (const RoomEdge& edge : grid.roomEdges)
                    {
                        const int other = edge.a == node ? edge.b : (edge.b == node ? edge.a : -1);
                        if (other < 0 || graphDistance[static_cast<std::size_t>(other)] >= 0) continue;
                        graphDistance[static_cast<std::size_t>(other)] =
                            graphDistance[static_cast<std::size_t>(node)] + 1;
                        next.push_back(other);
                    }
                }
                frontier.swap(next);
            }

            const auto requiredRun = AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition);
            const auto onRequiredRun = [&requiredRun](const Rect& room)
            {
                return std::any_of(requiredRun.begin(), requiredRun.end(),
                    [&room](Int2 tile) { return room.Contains(tile); });
            };

            int infectedZombies = 0;
            std::vector<int> looseRooms;
            for (const ZombieSpawn& spawn : grid.zombieSpawns)
            {
                if (!grid.IsWalkable(spawn.position))
                {
                    std::cerr << "FAIL seed " << seed << ": a zombie may not start inside a wall or a door\n";
                    return 1;
                }
                if (spawn.WaitsBehindBreach())
                {
                    const bool sealedIn = std::any_of(grid.leaves.begin(), grid.leaves.end(),
                        [&spawn](const LeafRegion& leaf)
                        {
                            return leaf.role == LeafRole::Infected && leaf.room.Contains(spawn.position);
                        });
                    if (!sealedIn)
                    {
                        std::cerr << "FAIL seed " << seed
                            << ": a zombie linked to a breach must wait inside an infected space\n";
                        return 1;
                    }
                    ++infectedZombies;
                    continue;
                }
                // 흩어진 좀비는 방 안에 서지 복도나 출입구에는 서지 않고, 플레이어에게
                // 도달할 수 있어야 한다. 아니면 그냥 배경이다.
                //
                // **필수 동선 위에 서도 된다.** 6.10 이 그것을 기대하고, 구르기로
                // 지나갈 수 있기만을 요구한다. 방 안쪽 1타일 여백이 그것을 보장한다.
                const auto room = std::find_if(grid.rooms.begin(), grid.rooms.end(),
                    [&spawn](const Rect& entry) { return entry.Contains(spawn.position); });
                if (room == grid.rooms.end()
                    || AStar::FindPath(grid, spawn.position, grid.playerSpawn).empty())
                {
                    std::cerr << "FAIL seed " << seed
                        << ": a loose zombie must stand in a room and be able to reach the player\n";
                    return 1;
                }
                looseRooms.push_back(static_cast<int>(std::distance(grid.rooms.begin(), room)));
            }
            // 감염 공간당 셋, 밖에 여섯, 한 방에 최대 둘.
            std::sort(looseRooms.begin(), looseRooms.end());
            bool crowded = false;
            for (std::size_t index = 2; index < looseRooms.size(); ++index)
            {
                if (looseRooms[index] == looseRooms[index - 2]) crowded = true;
            }
            if (infectedZombies != 6 || looseRooms.size() != 6 || crowded)
            {
                std::cerr << "FAIL seed " << seed
                    << ": three zombies per infected space and six loose ones, at most two to a room\n";
                return 1;
            }
            // 시작 방은 절대 좀비를 향해 열리지 않고, 방 하나는 **일부러 비워** 둔다.
            // Room Graph 거리 2 이상이라 찾아내야만 한다.
            if (std::find(looseRooms.begin(), looseRooms.end(), grid.startRoom) != looseRooms.end())
            {
                std::cerr << "FAIL seed " << seed << ": the start room must hold no zombies\n";
                return 1;
            }
            if (grid.safeRoom < 0 || grid.safeRoom >= static_cast<int>(grid.rooms.size())
                || graphDistance[static_cast<std::size_t>(grid.safeRoom)] < 2
                || std::find(looseRooms.begin(), looseRooms.end(), grid.safeRoom) != looseRooms.end())
            {
                std::cerr << "FAIL seed " << seed
                    << ": one room two or more edges out must be left empty of zombies\n";
                return 1;
            }
            // 나머지 활성 방은 하나 아니면 둘을 갖는다.
            for (std::size_t index = 0; index < grid.rooms.size(); ++index)
            {
                const int room = static_cast<int>(index);
                if (room == grid.startRoom || room == grid.safeRoom) continue;
                const auto count = std::count(looseRooms.begin(), looseRooms.end(), room);
                if (count >= 1 && count <= 2) continue;
                std::cerr << "FAIL seed " << seed
                    << ": every room but the start and the empty one holds one or two zombies\n";
                return 1;
            }

            // 서로 2타일. 그래야 좀비가 다른 좀비 안에서 시작하지 않고,
            // **자기가 기다리는 침입구에 이미 서 있지도 않다.**
            for (std::size_t first = 0; first < grid.zombieSpawns.size(); ++first)
            {
                for (std::size_t second = first + 1; second < grid.zombieSpawns.size(); ++second)
                {
                    const Int2 a = grid.zombieSpawns[first].position;
                    const Int2 b = grid.zombieSpawns[second].position;
                    if ((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) >= 4) continue;
                    std::cerr << "FAIL seed " << seed << ": zombies must start at least two tiles apart\n";
                    return 1;
                }
                for (const BreachSpawn& breach : grid.breachSpawns)
                {
                    const Int2 a = grid.zombieSpawns[first].position;
                    const int dx = a.x - breach.waitingPosition.x;
                    const int dy = a.y - breach.waitingPosition.y;
                    if (dx * dx + dy * dy >= 4) continue;
                    std::cerr << "FAIL seed " << seed << ": no zombie may block its own breach\n";
                    return 1;
                }
            }

            // 추가 탄약이 두 뭉치로 — 1발과 나머지 — **필수 동선이 지나지 않는 서로
            // 다른 방에.** 줍는 것은 언제나 선택이다.
            int extraRounds = 0;
            std::vector<int> ammoRooms;
            for (const AmmoPickup& pickup : grid.ammoPickups)
            {
                extraRounds += pickup.rounds;
                const auto room = std::find_if(grid.rooms.begin(), grid.rooms.end(),
                    [&pickup](const Rect& entry) { return entry.Contains(pickup.position); });
                if (room == grid.rooms.end() || onRequiredRun(*room)
                    || !grid.IsWalkable(pickup.position) || pickup.position == grid.keyPosition)
                {
                    std::cerr << "FAIL seed " << seed
                        << ": ammunition must lie on open floor in a room off the required run\n";
                    return 1;
                }
                ammoRooms.push_back(static_cast<int>(std::distance(grid.rooms.begin(), room)));
            }
            std::sort(ammoRooms.begin(), ammoRooms.end());
            // **여기 적은 3 이 아니라 설정값에 대고** 비교한다. 이 개수가 네 곳에
            // 살고 있었고 — 이 줄, 검증기, 생성기의 구조체 기본값, ini — 그중 하나만
            // 올린 것이 2026-08-29 에 맵 생성을 멈춘 원인이었다.
            // 넷 중 둘은 사라졌고, 이 검사는 설정값을 단일 기준으로 삼는다.
            if (grid.ammoPickups.size() != 2 || extraRounds != mapSettings.extraAmmoRounds
                || std::adjacent_find(ammoRooms.begin(), ammoRooms.end()) != ammoRooms.end())
            {
                std::cerr << "FAIL seed " << seed
                    << ": the extra ammunition must be two bundles totalling "
                    << mapSettings.extraAmmoRounds << " rounds in different rooms\n";
                return 1;
            }

            // 6.11 의 머리 항목을, Validate() 와 **독립적으로** 여기서 확인한다 —
            // 완성된 타일이 실제로 잇는 방들이 Room Graph 가 잇겠다고 계획한 방들과
            // 정확히 같아야 한다. 계획이 아니라 맵에서 되읽는다.
            //
            // **이것은 Validate() 가 하는 일의 반복이고, 그래야만 한다.** 둘은 추출을
            // 공유하고 비교는 공유하지 않는다. 여기서 Validate() 를 부르면 검증기의
            // 버그가 자기 검사를 통과한다. **클론 검출기가 이 둘을 가리켜도 합치지 마라.**
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
                std::cerr << "FAIL seed " << seed
                    << ": the tiles join rooms the Room Graph never planned\n";
                return 1;
            }

            // 리프가 루트를 정확히 덮어야 한다 — 겹침도 틈도 없이.
            long long leafArea = 0;
            for (std::size_t first = 0; first < grid.leaves.size(); ++first)
            {
                const Rect& lhs = grid.leaves[first].region;
                leafArea += static_cast<long long>(lhs.width) * lhs.height;
                for (std::size_t second = first + 1; second < grid.leaves.size(); ++second)
                {
                    const Rect& rhs = grid.leaves[second].region;
                    if (lhs.x >= rhs.Right() || rhs.x >= lhs.Right()
                        || lhs.y >= rhs.Bottom() || rhs.y >= lhs.Bottom())
                    {
                        continue;
                    }
                    std::cerr << "FAIL seed " << seed << ": two quadtree leaves overlap\n";
                    return 1;
                }
            }
            if (leafArea != static_cast<long long>(grid.Width() - 2) * (grid.Height() - 2))
            {
                std::cerr << "FAIL seed " << seed << ": the quadtree leaves must tile the root exactly\n";
                return 1;
            }

            // 연결돼 있고, 신장 트리 위에 독립 고리가 정확히 둘.
            if (static_cast<int>(grid.roomEdges.size()) - static_cast<int>(grid.rooms.size()) + 1 != 2)
            {
                std::cerr << "FAIL seed " << seed << ": the Room Graph needs exactly two independent loops\n";
                return 1;
            }

            // 필수 동선의 어느 것도 총알이나 밀어붙이기를 요구해서는 안 된다.
            std::vector<Int2> requiredTiles = AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition);
            const std::vector<Int2> keyToExit = AStar::FindPath(grid, grid.keyPosition, grid.exit.origin);
            requiredTiles.insert(requiredTiles.end(), keyToExit.begin(), keyToExit.end());
            for (const Int2 tile : requiredTiles)
            {
                const Tile value = grid.Get(tile);
                if (value == Tile::Floor || value == Tile::DoorClosed || value == Tile::DoorOpen
                    || value == Tile::ExitLocked || value == Tile::ExitOpen)
                {
                    continue;
                }
                std::cerr << "FAIL seed " << seed
                    << ": the required run must never cross a tile a bullet has to open\n";
                return 1;
            }

            // 권총이 갖고 시작하는 것 + 바닥에 있는 것. 6.10 이 한 판 전체를 좀비
            // 열둘보다 한참 아래로 묶으므로 **모두 죽이는 것은 선택지에 없다.**
            //
            // 상한을 여기 적지 않고 **설정에서 읽는다.** 예전에는 이 줄에 6, 검증기에
            // 6, 위 검사에 3, 구조체 기본값 둘에 3 이 있었고, 그중 하나를 옮긴 것이
            // 2026-08-29 에 맵 생성을 완전히 멈춘 원인이었다.
            const int budget = mapSettings.startAmmo + mapSettings.extraAmmoRounds;
            int totalRounds = mapSettings.startAmmo;
            for (const AmmoPickup& pickup : grid.ammoPickups) totalRounds += pickup.rounds;
            if (totalRounds != budget)
            {
                std::cerr << "FAIL seed " << seed << ": the whole map must hold exactly "
                    << budget << " rounds\n";
                return 1;
            }

            // A* 와 플레이어 충돌 모델은 **E 상호작용으로 지나갈 수 있는 곳에서만**
            // 어긋나도 된다. 그 외의 경우는 플레이어를 가두는 것이고, 그런 맵도
            // 모든 경로 검사는 여전히 "연결됨"이라고 보고한다.
            collisionProbe.grid = grid;
            for (int y = 0; y < grid.Height(); ++y)
            {
                for (int x = 0; x < grid.Width(); ++x)
                {
                    const Int2 point{ x, y };
                    if (!grid.IsWalkable(point)) continue;
                    if (collisionProbe.CanPlayerOccupy(TileCenter(point))) continue;
                    const Tile blocking = grid.Get(point);
                    if (IsClosedDoor(blocking) || blocking == Tile::WindowBroken) continue;
                    std::cerr << "FAIL seed " << seed
                        << ": a tile A* walks through blocks the player with no way to interact\n";
                    return 1;
                }
            }

            // 필수 동선은 절대 총알을 요구하면 안 된다. A* 가 안 깨진 창문을 넘기를
            // 거절하므로, 여기서 찾은 경로는 곧 **맨손으로 걸을 수 있는 경로**다.
            if (AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition).empty()
                || AStar::FindPath(grid, grid.keyPosition, grid.exit.origin).empty())
            {
                std::cerr << "FAIL seed " << seed
                    << ": start to key to exit must hold without breaking a window\n";
                return 1;
            }
            const auto keyPath = AStar::FindPath(grid, grid.exit.origin, grid.keyPosition);
            if (keyPath.empty())
            {
                std::cerr << "FAIL seed " << seed << ": key is unreachable\n";
                return 1;
            }
            for (const Rect& room : grid.rooms)
            {
                if (!AStar::FindPath(grid, grid.exit.origin, room.Center()).empty()) continue;
                std::cerr << "FAIL seed " << seed << ": disconnected room\n";
                return 1;
            }

            // 탈출구는 시작 방 자신의 벽 링 위 두 타일이고, **두 상태 모두 통행
            // 가능하고 투명하다.** 15.5 가 정한 것 — 거기 벽을 두면 A* 와 플레이어
            // 충돌 모델이 어긋난다.
            const Rect& startRoom = grid.rooms[static_cast<std::size_t>(grid.startRoom)];
            const auto onStartWallRing = [&startRoom](Int2 tile)
            {
                return tile.x >= startRoom.x - 1 && tile.x <= startRoom.Right()
                    && tile.y >= startRoom.y - 1 && tile.y <= startRoom.Bottom()
                    && !startRoom.Contains(tile);
            };
            const Int2 exitOrigin = grid.exit.origin;
            const Int2 exitSecond = grid.exit.Second();
            if (!onStartWallRing(exitOrigin) || !onStartWallRing(exitSecond)
                || grid.Get(exitOrigin) != Tile::ExitLocked || grid.Get(exitSecond) != Tile::ExitLocked
                || !grid.IsWalkable(exitOrigin) || !grid.IsWalkable(exitSecond)
                || grid.IsOpaque(exitOrigin) || grid.IsOpaque(exitSecond))
            {
                std::cerr << "FAIL seed " << seed
                    << ": the exit must be two open tiles in the start room's outward wall\n";
                return 1;
            }
            if (!startRoom.Contains(grid.playerSpawn))
            {
                std::cerr << "FAIL seed " << seed << ": the player must start inside the start room\n";
                return 1;
            }
            const int spawnToExit = std::abs(grid.playerSpawn.x - exitOrigin.x)
                + std::abs(grid.playerSpawn.y - exitOrigin.y);
            if (spawnToExit < 3 || spawnToExit > 5)
            {
                std::cerr << "FAIL seed " << seed << ": the player must start three to five tiles from the exit\n";
                return 1;
            }

            if (graphDistance[static_cast<std::size_t>(grid.keyRoom)] < 3)
            {
                std::cerr << "FAIL seed " << seed
                    << ": the key room must be at least three Room Graph edges from the start\n";
                return 1;
            }
            if (!grid.rooms[static_cast<std::size_t>(grid.keyRoom)].Contains(grid.keyPosition))
            {
                std::cerr << "FAIL seed " << seed << ": the key must lie inside its own room\n";
                return 1;
            }
            if (AStar::PathCost(grid, keyPath) < mapSettings.minimumKeyPathCost)
            {
                std::cerr << "FAIL seed " << seed << ": the key sits closer than the minimum path cost\n";
                return 1;
            }
            // 모든 출입구에서 떨어져 있어야 한다. 아니면 자기 경로를 걷는 좀비가
            // 열쇠를 밟는다.
            for (const Doorway& doorway : grid.doorways)
            {
                for (const Int2 tile : { doorway.tiles.origin, doorway.tiles.Second() })
                {
                    if (std::max(std::abs(grid.keyPosition.x - tile.x),
                        std::abs(grid.keyPosition.y - tile.y)) >= 2)
                    {
                        continue;
                    }
                    std::cerr << "FAIL seed " << seed << ": the key must stay two tiles clear of every doorway\n";
                    return 1;
                }
            }
            return 0;
        }
        // 시드 64개를 기획이 이름 붙인 모든 구조 불변식에 통과시킨다.
        // **가장 먼저 돈다** — 이 뒤의 모든 것이 유효한 맵을 전제하기 때문이다.
        static int MapGeneration(const MapGenerationSettings& mapSettings,
            const MapGenerator& generator)
        {
            // 이 인스턴스에서 쓰는 것은 CanPlayerOccupy() 하나뿐이고, 격자는 아래에서
            // 시드마다 갈아 끼운다. Game 을 하나 만드는 것이 시드마다 만드는 것보다
            // 훨씬 싸다.
            Game collisionProbe(1u, Balance::Defaults());
            // 생성기가 얼마나 애썼는지를, 별도 실행이 아니라 **방금 만든 맵들에서**
            // 뽑는다. 생성 규칙이 움직일 때마다 자동으로 다시 재기 때문에,
            // **아무도 잊지 않아도 되는 숫자가 계속 참인 숫자다.**
            int discards = 0;
            int worst = 0;
            for (std::uint32_t seed = 1; seed <= 64; ++seed)
            {
                std::vector<MapGenerationSnapshot> trace;
                const Grid grid = generator.Generate(seed, &trace);
                const Grid repeated = generator.Generate(seed);
                discards += grid.seedAttempt;
                worst = std::max(worst, grid.seedAttempt);
                if (const int code = CheckSeedShape(seed, grid, trace); code != 0) return code;
                if (const int code = CheckSeedCorridors(seed, grid); code != 0) return code;
                if (const int code = CheckSeedOpenings(seed, grid, repeated); code != 0) return code;
                if (const int code = CheckSeedContents(seed, grid, mapSettings, collisionProbe); code != 0) return code;
            }

            // 제한 횟수를 다 쓴 결과는 모양이 Grid여도 게임 세계가 아니다. 마지막
            // 실패작을 반환하지 않고 원인과 함께 실패하며, 호출자가 가진 추적도
            // 성공 전에는 건드리지 않아야 한다.
            MapGenerationSettings impossible = mapSettings;
            impossible.maxSeedAttempts = 1;
            impossible.minimumKeyPathCost = std::numeric_limits<float>::max();
            MapGenerator refusing(impossible);
            std::vector<MapGenerationSnapshot> failedTrace{
                { "sentinel", Grid(3, 3) }
            };
            bool refused = false;
            try
            {
                (void)refusing.Generate(424242u, &failedTrace);
            }
            catch (const std::runtime_error& error)
            {
                const std::string message = error.what();
                refused = message.find("after 1 attempts on seed 424242") != std::string::npos
                    && message.find("\n  1x ") != std::string::npos;
            }
            if (!refused)
            {
                std::cerr << "FAIL map generation test: exhausted attempts must report and throw\n";
                return 1;
            }
            if (failedTrace.size() != 1 || failedTrace.front().label != "sentinel")
            {
                std::cerr << "FAIL map generation test: a failed generation must not publish its trace\n";
                return 1;
            }

            const std::uint32_t oldSeed = collisionProbe.seed;
            const std::uint32_t oldGridSeed = collisionProbe.grid.seed;
            const std::uint64_t oldGridHash = collisionProbe.grid.StableHash();
            const Int2 oldSpawn = collisionProbe.grid.playerSpawn;
            const std::uint32_t oldRevision = collisionProbe.grid.Revision();
            const std::size_t oldTraceSize = collisionProbe.generationTrace.size();
            collisionProbe.screen = Screen::Generating;
            MapGenerationSettings hundredFailures = impossible;
            hundredFailures.maxSeedAttempts = 100;
            collisionProbe.generator = MapGenerator(hundredFailures);
            bool resetRefused = false;
            try
            {
                collisionProbe.Reset(oldSeed + 1);
                std::cerr << "FAIL map generation test: Game::Reset accepted an invalid map\n";
                return 1;
            }
            catch (const std::runtime_error& error)
            {
                const std::string message = error.what();
                resetRefused = message.find("after 100 attempts on seed "
                    + std::to_string(oldSeed + 1)) != std::string::npos
                    && message.find("\n  ") != std::string::npos;
            }
            if (!resetRefused || collisionProbe.seed != oldSeed
                || collisionProbe.grid.seed != oldGridSeed
                || collisionProbe.grid.StableHash() != oldGridHash
                || !(collisionProbe.grid.playerSpawn == oldSpawn)
                || collisionProbe.grid.Revision() != oldRevision
                || collisionProbe.generationTrace.size() != oldTraceSize
                || collisionProbe.screen != Screen::Generating)
            {
                std::cerr << "FAIL map generation test: a failed reset must keep the current world intact\n";
                return 1;
            }
            std::cout << "Seed retries " << discards << " discarded over 64 seeds, worst "
                << worst << " of " << mapSettings.maxSeedAttempts << "\n";
            return 0;
        }

        // 압력이 임계값을 넘는가, 그리고 소음 사건 하나가 침입구 하나를 고르는가.
        //
        // 맵을 수정 가능한 참조로 받는 이유는 마지막 검사가 그것을 ApplyNoise() 에
        // 그대로 넘기기 때문이다. 그 사건은 거절되므로 실제로 쓰이는 것은 없지만,
        // 시그니처는 허용해야 한다.
        static int BreachPressure(Grid& sample)
        {
        Grid breachSample = sample;
        BreachSystem breachTest(BreachTuning{ 10.0f, 20.0f, 30.0f, 5.0f, 0.0f });
        breachTest.Initialize(breachSample);
        if (breachTest.Points().empty())
        {
            std::cerr << "FAIL breach test: no generated doors or windows\n";
            return 1;
        }
        const BreachPoint breachPoint = breachTest.Points().front();
        const bool sealedDoorBreach = breachPoint.kind == BreachKind::SealedDoor;
        const Tile warningTile = sealedDoorBreach ? Tile::DoorSealedWarning : Tile::WindowWarning;
        const Tile crackedTile = sealedDoorBreach ? Tile::DoorSealedCracked : Tile::WindowCracked;
        const Tile brokenTile = sealedDoorBreach ? Tile::DoorBroken : Tile::WindowBroken;
        breachTest.AddPressure(breachPoint.position, 10.0f, breachSample);
        if (breachSample.Get(breachPoint.position) != warningTile)
        {
            std::cerr << "FAIL breach test: warning transition\n";
            return 1;
        }
        breachTest.AddPressure(breachPoint.position, 10.0f, breachSample);
        breachTest.Update(100.0f, breachSample);
        if (breachSample.Get(breachPoint.position) != crackedTile)
        {
            std::cerr << "FAIL breach test: cracked damage must persist after pressure decay\n";
            return 1;
        }
        breachTest.AddPressure(breachPoint.position, 30.0f, breachSample);
        if (breachSample.Get(breachPoint.position) != brokenTile || breachSample.IsOpaque(breachPoint.position))
        {
            std::cerr << "FAIL breach test: broken transition must open sight\n";
            return 1;
        }

        Grid noiseSample = sample;
        BreachSystem noiseTest(
            BreachTuning{ 10.0f, 20.0f, 30.0f, 0.0f, 0.0f },
            NoiseTuning{ 1.5f, 0.5f, 0.01f, 1 });
        noiseTest.Initialize(noiseSample);
        std::mt19937 noiseRandom(20260826u);
        const std::vector<Int2> responders = noiseTest.ApplyNoise(
            TileCenter(noiseSample.playerSpawn), 1000.0f, 1.0f, noiseSample, noiseRandom);
        if (responders.size() != 1 || noiseSample.IsOpaque(responders.front()))
        {
            std::cerr << "FAIL noise test: one weighted responder must break and open sight\n";
            return 1;
        }
        BreachSystem silentTest(
            BreachTuning{ 10.0f, 20.0f, 30.0f, 0.0f, 0.0f },
            NoiseTuning{ 1.5f, 0.5f, 0.01f, 1 });
        silentTest.Initialize(sample);
        if (!silentTest.ApplyNoise(TileCenter(sample.playerSpawn), 1000.0f, 0.0f, sample, noiseRandom).empty())
        {
            std::cerr << "FAIL noise test: a rejected event must affect no breach points\n";
            return 1;
        }
            return 0;
        }

        // 창문을 쏴서 깨고, E 로 넘고, **두 종류의 침입구 뒤에 있는 좀비가**
        // 자기 길이 열릴 때 깨어나는가.
        static int BreachFlow()
        {
        // 우선순위 3 — 창문 사격, E 통과, 그리고 그 창문이 유일한 입장 경로인
        // 미리 배치된 좀비.
        Game game(20260826u, Balance::Defaults());
        // **창문 침입구를 콕 집어서.** 감염 공간 둘 중 하나는 막힌 문으로 열리는데,
        // 권총은 그쪽에 절대 닿으면 안 된다.
        const auto windowBreach = std::find_if(game.grid.breachSpawns.begin(), game.grid.breachSpawns.end(),
            [&game](const BreachSpawn& spawn)
            {
                return game.grid.Get(spawn.breachPosition) == Tile::WindowIntact;
            });
        if (windowBreach == game.grid.breachSpawns.end())
        {
            std::cerr << "FAIL breach zombie test: the sample map has no window breach to shoot\n";
            return 1;
        }
        const BreachSpawn intrusion = *windowBreach;
        const std::size_t zombieCount = game.zombies.size();
        if (zombieCount != game.grid.zombieSpawns.size())
        {
            std::cerr << "FAIL breach zombie test: Reset must create exactly the generated zombies\n";
            return 1;
        }

        std::size_t waitingIndex = game.zombies.size();
        for (std::size_t index = 0; index < game.zombies.size(); ++index)
        {
            const ZombieAgent& zombie = game.zombies[index];
            // 둘 중 어느 연결이든. 감염 공간은 나가는 길이 둘일 수 있고, 지금
            // 검사하는 창문이 그 좀비의 두 번째 길일 수 있다.
            if (!zombie.hasBreachLink) continue;
            if (zombie.breachPosition == intrusion.breachPosition
                || zombie.otherBreachPosition == intrusion.breachPosition)
            {
                waitingIndex = index;
            }
        }
        // 이제 감염 공간 하나를 여럿이 나눠 쓰므로 침입구 착지 타일에 특정 좀비가
        // 서 있지 않다. 붙들어야 할 것은 **검사 대상이 그 감염 공간 안에서 시작하고,
        // 침입구가 버티는 동안 도달할 수 없다**는 것이다.
        const auto infectedLeaf = std::find_if(game.grid.leaves.begin(), game.grid.leaves.end(),
            [&intrusion](const LeafRegion& leaf)
            {
                return leaf.role == LeafRole::Infected && leaf.room.Contains(intrusion.waitingPosition);
            });
        if (waitingIndex == game.zombies.size()
            || !game.zombies[waitingIndex].waitingBehindBreach
            || infectedLeaf == game.grid.leaves.end()
            || !infectedLeaf->room.Contains(ToTile(game.zombies[waitingIndex].position)))
        {
            std::cerr << "FAIL breach zombie test: the waiting zombie must start inside its infected space\n";
            return 1;
        }

        const Vec2 sealedPosition = game.zombies[waitingIndex].position;
        for (int step = 0; step < 180; ++step) game.UpdateZombies(1.0f / 60.0f);
        if (game.zombies.size() != zombieCount
            || Length(game.zombies[waitingIndex].position - sealedPosition) > 0.0001f
            || !game.zombies[waitingIndex].waitingBehindBreach)
        {
            std::cerr << "FAIL breach zombie test: a sealed zombie must not move, wake or multiply\n";
            return 1;
        }

        const auto aimAt = [&game](Int2 from, Int2 target)
        {
            game.playerPosition = TileCenter(from);
            const Vec2 offset = TileCenter(target) - game.playerPosition;
            game.playerAngle = std::atan2(offset.y, offset.x);
            game.aimPixel = { RaycastRenderer::DefaultWidth / 2, RaycastRenderer::DefaultHeight / 2 };
        };

        const Int2 window = intrusion.breachPosition;
        // 침입구는 벽 선 안에 있고 열린 축이 정확히 하나다. 활동 구역 쪽은 플레이어가
        // 이미 갈 수 있는 쪽이고, 반대쪽이 기어 다니는 굴이다.
        Int2 approach{ -1, -1 };
        Int2 pocket{ -1, -1 };
        for (const Int2 axis : { Int2{ 1, 0 }, Int2{ 0, 1 } })
        {
            const Int2 low{ window.x - axis.x, window.y - axis.y };
            const Int2 high{ window.x + axis.x, window.y + axis.y };
            if (!game.grid.IsWalkable(low) || !game.grid.IsWalkable(high)) continue;
            const bool lowReachable = !AStar::FindPath(game.grid, game.grid.playerSpawn, low).empty();
            approach = lowReachable ? low : high;
            pocket = lowReachable ? high : low;
        }
        if (approach.x < 0)
        {
            std::cerr << "FAIL breach zombie test: the window breach has no open axis\n";
            return 1;
        }
        if (game.grid.Get(window) != Tile::WindowIntact)
        {
            std::cerr << "FAIL shooting test: the window under test must still be intact before the shot\n";
            return 1;
        }
        aimAt(approach, window);
        game.ammo = 3;
        game.Shoot();
        if (game.grid.Get(window) != Tile::WindowBroken || game.ammo != 2)
        {
            std::cerr << "FAIL shooting test: an aimed shot must break an intact window and spend ammunition\n";
            return 1;
        }
        if (game.zombies.size() != zombieCount)
        {
            std::cerr << "FAIL shooting test: breaking a window must not create a zombie\n";
            return 1;
        }
        if (game.CanPlayerOccupy(TileCenter(window)))
        {
            std::cerr << "FAIL vault test: a broken window must still block plain player movement\n";
            return 1;
        }

        game.TryInteract();
        if (ToTile(game.playerPosition) != pocket)
        {
            std::cerr << "FAIL vault test: E must carry the player across a broken window\n";
            return 1;
        }

        // 다시 물러난다. 그래야 깨어남 검사가 **포켓 안에 서 있는 플레이어에 대한
        // 공격**이 아니라 활동 구역으로 나오는 길찾기를 재게 된다.
        game.playerPosition = TileCenter(approach);
        game.UpdateZombies(1.0f / 60.0f);
        if (game.zombies[waitingIndex].waitingBehindBreach || !game.zombies[waitingIndex].alert)
        {
            std::cerr << "FAIL breach zombie test: a broken window must release the waiting zombie\n";
            return 1;
        }
        if (AStar::FindPath(game.grid, ToTile(game.zombies[waitingIndex].position), game.grid.playerSpawn).empty())
        {
            std::cerr << "FAIL breach zombie test: the released zombie needs a path into the play space\n";
            return 1;
        }
        if (game.zombies.size() != zombieCount)
        {
            std::cerr << "FAIL breach zombie test: no zombie may be created during play\n";
            return 1;
        }

        // **다른 종류의 침입구.** 위의 모든 깨어남 검사가 창문 침입구를 썼고,
        // 그 사각지대가 진짜 버그를 출하시켰다 — 깨어남 조건이 WindowBroken 을
        // 이름으로 대고 있어서, DoorBroken 으로 열리는 막힌 문 뒤의 좀비들이
        // 영영 자고 있었다.
        Game sealedGame(20260826u, Balance::Defaults());
        const auto sealedBreach = std::find_if(
            sealedGame.grid.breachSpawns.begin(), sealedGame.grid.breachSpawns.end(),
            [&sealedGame](const BreachSpawn& spawn)
            {
                return sealedGame.grid.Get(spawn.breachPosition) == Tile::DoorSealed;
            });
        if (sealedBreach == sealedGame.grid.breachSpawns.end())
        {
            std::cerr << "FAIL sealed door test: the sample map has no sealed door breach\n";
            return 1;
        }
        std::size_t sleepers = 0;
        for (const ZombieAgent& zombie : sealedGame.zombies)
        {
            if (!zombie.waitingBehindBreach) continue;
            if (zombie.breachPosition == sealedBreach->breachPosition
                || zombie.otherBreachPosition == sealedBreach->breachPosition)
            {
                ++sleepers;
            }
        }
        // 공간당 셋, 그리고 **그 공간의 침입구 둘 다** 같은 셋을 깨운다.
        if (sleepers != 3)
        {
            std::cerr << "FAIL sealed door test: three zombies must wait behind each breach\n";
            return 1;
        }
        sealedGame.playerPosition = TileCenter(sealedGame.grid.playerSpawn);
        sealedGame.UpdateZombies(1.0f / 60.0f);
        const auto linkedToSealed = [&sealedBreach](const ZombieAgent& zombie)
        {
            return zombie.breachPosition == sealedBreach->breachPosition
                || zombie.otherBreachPosition == sealedBreach->breachPosition;
        };
        if (std::any_of(sealedGame.zombies.begin(), sealedGame.zombies.end(),
            [&linkedToSealed](const ZombieAgent& zombie)
            {
                return linkedToSealed(zombie) && !zombie.waitingBehindBreach;
            }))
        {
            std::cerr << "FAIL sealed door test: an intact sealed door must keep its zombies asleep\n";
            return 1;
        }
        sealedGame.grid.Set(sealedBreach->breachPosition, Tile::DoorBroken);
        sealedGame.UpdateZombies(1.0f / 60.0f);
        for (const ZombieAgent& zombie : sealedGame.zombies)
        {
            if (!linkedToSealed(zombie)) continue;
            if (!zombie.waitingBehindBreach && zombie.alert) continue;
            std::cerr << "FAIL sealed door test: a broken sealed door must wake every zombie behind it\n";
            return 1;
        }

        // 6.9 — 논리적 문 하나가 출입구의 두 타일 모두를 소유한다. 반쪽을 열고
        // 돌아서서 나머지 반쪽을 또 열어야 하는 것은 문이 아니다.
        Game doorwayGame(20260826u, Balance::Defaults());
        const auto closedDoorway = std::find_if(
            doorwayGame.grid.doorways.begin(), doorwayGame.grid.doorways.end(),
            [&doorwayGame](const Doorway& doorway)
            {
                return IsClosedDoor(doorwayGame.grid.Get(doorway.tiles.origin));
            });
        if (closedDoorway == doorwayGame.grid.doorways.end())
        {
            std::cerr << "FAIL doorway test: the sample map has no closed doorway\n";
            return 1;
        }
        doorwayGame.OpenDoorway(closedDoorway->tiles.origin);
        if (doorwayGame.grid.Get(closedDoorway->tiles.origin) != Tile::DoorOpen
            || doorwayGame.grid.Get(closedDoorway->tiles.Second()) != Tile::DoorOpen)
        {
            std::cerr << "FAIL doorway test: opening a door must open both of its tiles\n";
            return 1;
        }

        // helper를 직접 부르는 것만으로는 좀비 분기의 한 칸짜리 구현을 잡지 못한다.
        // 실제 경로 한 걸음을 문으로 향하게 해 같은 논리적 문 두 칸이 열리는지 본다.
        {
            Game zombieDoorGame(20260826u, Balance::Defaults());
            const auto doorway = std::find_if(zombieDoorGame.grid.doorways.begin(),
                zombieDoorGame.grid.doorways.end(), [&zombieDoorGame](const Doorway& candidate)
                {
                    return IsClosedDoor(zombieDoorGame.grid.Get(candidate.tiles.origin));
                });
            if (doorway == zombieDoorGame.grid.doorways.end())
            {
                std::cerr << "FAIL zombie doorway test: the sample map has no closed doorway\n";
                return 1;
            }
            Int2 zombieApproach{ -1, -1 };
            Int2 target = doorway->tiles.origin;
            for (const Int2 half : { doorway->tiles.origin, doorway->tiles.Second() })
            {
                for (const Int2 offset : { Int2{ 1, 0 }, Int2{ -1, 0 }, Int2{ 0, 1 }, Int2{ 0, -1 } })
                {
                    const Int2 candidate{ half.x + offset.x, half.y + offset.y };
                    if (!zombieDoorGame.grid.IsWalkable(candidate)) continue;
                    zombieApproach = candidate;
                    target = half;
                    break;
                }
                if (zombieApproach.x >= 0) break;
            }
            if (zombieApproach.x < 0)
            {
                std::cerr << "FAIL zombie doorway test: the closed doorway has no walkable approach\n";
                return 1;
            }
            zombieDoorGame.zombies.clear();
            ZombieAgent opener;
            opener.position = TileCenter(zombieApproach);
            opener.alert = true;
            opener.repathTimer = 1.0f;
            opener.path = { zombieApproach, target };
            opener.pathIndex = 1;
            zombieDoorGame.zombies.push_back(opener);
            zombieDoorGame.UpdateZombies(1.0f / 60.0f);
            if (zombieDoorGame.grid.Get(doorway->tiles.origin) != Tile::DoorOpen
                || zombieDoorGame.grid.Get(doorway->tiles.Second()) != Tile::DoorOpen)
            {
                std::cerr << "FAIL zombie doorway test: a zombie must open both doorway tiles\n";
                return 1;
            }
        }

        // 위의 사격이 이 맵을 바꿔놓지 못하도록 **별도 인스턴스**를 쓴다.
        // 권총은 탄약을 아무리 써도 문을 손상시키면 안 된다.
        Game doorGame(20260826u, Balance::Defaults());
        Int2 doorTile{ -1, -1 };
        Int2 doorApproach{};
        for (int y = 1; y < doorGame.grid.Height() - 1 && doorTile.x < 0; ++y)
        {
            for (int x = 1; x < doorGame.grid.Width() - 1 && doorTile.x < 0; ++x)
            {
                const Int2 point{ x, y };
                if (!IsClosedDoor(doorGame.grid.Get(point))) continue;
                for (const Int2 axis : { Int2{ 1, 0 }, Int2{ 0, 1 } })
                {
                    const Int2 side{ point.x - axis.x, point.y - axis.y };
                    if (doorGame.grid.Get(side) != Tile::Floor) continue;
                    doorTile = point;
                    doorApproach = side;
                    break;
                }
            }
        }
        if (doorTile.x < 0)
        {
            std::cerr << "FAIL shooting test: sample map has no reachable closed door to verify against\n";
            return 1;
        }
        doorGame.playerPosition = TileCenter(doorApproach);
        {
            const Vec2 doorOffset = TileCenter(doorTile) - doorGame.playerPosition;
            doorGame.playerAngle = std::atan2(doorOffset.y, doorOffset.x);
        }
        doorGame.aimPixel = { RaycastRenderer::DefaultWidth / 2, RaycastRenderer::DefaultHeight / 2 };
        doorGame.ammo = 3;
        const Tile doorBefore = doorGame.grid.Get(doorTile);
        doorGame.Shoot();
        if (doorGame.grid.Get(doorTile) != doorBefore || doorGame.ammo != 2)
        {
            std::cerr << "FAIL shooting test: a pistol must spend ammunition without damaging a door\n";
            return 1;
        }

        // 청각. 예전에는 **사격 한 번이 맵의 모든 좀비를 깨웠고**, 그래서 한 발에
        // 두 방 건너에서 몰려왔다. 지금은 누가 듣는지를 음향 필드가 정하므로
        // 벽이 중요하고 생거리는 중요하지 않다.
        //
        //   S . . . . . . . .   열린 복도 마흔 타일
        {
            const Balance defaults = Balance::Defaults();
            Grid hall(40, 3);
            for (int x = 0; x < 40; ++x) hall.Set({ x, 1 }, Tile::Floor);

            Game earshot(20260826u, defaults);
            earshot.grid = hall;
            earshot.breachSystem.Initialize(earshot.grid);
            earshot.playerPosition = TileCenter({ 1, 1 });
            earshot.zombies.clear();
            ZombieAgent close;
            close.position = TileCenter({ 10, 1 });
            earshot.zombies.push_back(close);
            ZombieAgent distant;
            distant.position = TileCenter({ 35, 1 });
            earshot.zombies.push_back(distant);

            // 반응 확률을 0 으로 둬서 **청각만** 재고 침입 압력이 섞이지 않게 한다.
            // 바닥 아홉 타일은 9, 서른네 타일은 34 이고, 30 - 34 × 1.5 는 0보다 한참
            // 아래다.
            earshot.EmitNoise(defaults.shotNoise, 0.0f);
            if (!earshot.zombies[0].alert)
            {
                std::cerr << "FAIL hearing test: a zombie one room away must hear a shot\n";
                return 1;
            }
            if (earshot.zombies[1].alert)
            {
                std::cerr << "FAIL hearing test: a shot must not carry across the whole map\n";
                return 1;
            }

            // **같은 열 타일인데 사이에 벽이 셋.** 벽이 하나에 6을 매기므로 거리가
            // 그대로여도 소리가 다 닳아서 도착한다 — 바닥 여섯에 벽 셋이면 24 이고,
            // 30 - 24 × 1.5 는 음수다.
            Grid walled = hall;
            walled.Set({ 5, 1 }, Tile::Wall);
            walled.Set({ 6, 1 }, Tile::Wall);
            walled.Set({ 7, 1 }, Tile::Wall);
            Game muffled(20260826u, defaults);
            muffled.grid = walled;
            muffled.breachSystem.Initialize(muffled.grid);
            muffled.playerPosition = TileCenter({ 1, 1 });
            muffled.zombies.clear();
            muffled.zombies.push_back(close);
            muffled.EmitNoise(defaults.shotNoise, 0.0f);
            if (muffled.zombies[0].alert)
            {
                std::cerr << "FAIL hearing test: walls must swallow a shot the open corridor carries\n";
                return 1;
            }
        }

        // 소리를 들은 좀비는 자기 침입구가 무너질 때까지 민다.
        // **이게 없으면 감염 공간은 충분히 큰 소음을 영원히 기다린다.**
        {
            Game secondary(20260826u, Balance::Defaults());
            const auto linked = std::find_if(secondary.zombies.begin(), secondary.zombies.end(),
                [](const ZombieAgent& zombie)
                {
                    return zombie.waitingBehindBreach && zombie.otherBreachPosition.x >= 0;
                });
            if (linked == secondary.zombies.end())
            {
                std::cerr << "FAIL secondary breach test: the sample map has no two-exit infected space\n";
                return 1;
            }
            const Int2 primary = linked->breachPosition;
            const Int2 other = linked->otherBreachPosition;
            const ZombieAgent isolated = *linked;
            secondary.zombies = { isolated };
            secondary.StirBreachZombie(other);
            // 같은 소음 사건에서 허용된 responder가 둘이어도 첫 선택이 뒤집히지 않는다.
            secondary.StirBreachZombie(primary);
            if (!secondary.zombies.front().alert
                || !(secondary.zombies.front().selectedBreachPosition == other))
            {
                std::cerr << "FAIL secondary breach test: the responding exit must be latched\n";
                return 1;
            }
            const auto pressureAt = [&secondary](Int2 position)
            {
                const auto point = std::find_if(secondary.breachSystem.Points().begin(),
                    secondary.breachSystem.Points().end(), [position](const BreachPoint& candidate)
                    {
                        return candidate.position == position;
                    });
                return point == secondary.breachSystem.Points().end() ? -1.0f : point->pressure;
            };
            const float primaryBefore = pressureAt(primary);
            const float otherBefore = pressureAt(other);
            secondary.UpdateZombies(0.5f);
            if (primaryBefore < 0.0f || otherBefore < 0.0f
                || std::abs(pressureAt(primary) - primaryBefore) > 0.0001f
                || pressureAt(other) <= otherBefore)
            {
                std::cerr << "FAIL secondary breach test: pressure must go to the exit that woke the zombie\n";
                return 1;
            }
        }
        {
            Game leaning(20260826u, Balance::Defaults());
            const auto breach = std::find_if(leaning.grid.breachSpawns.begin(),
                leaning.grid.breachSpawns.end(),
                [&leaning](const BreachSpawn& spawn)
                {
                    return leaning.grid.Get(spawn.breachPosition) == Tile::DoorSealed;
                });
            if (breach == leaning.grid.breachSpawns.end())
            {
                std::cerr << "FAIL breach pressure test: the sample map has no sealed door\n";
                return 1;
            }
            for (ZombieAgent& zombie : leaning.zombies)
            {
                if (zombie.waitingBehindBreach && zombie.breachPosition == breach->breachPosition)
                    zombie.alert = true;
            }
            // 셋이 초당 3씩, 임계값 40 에 대고 — 4.5초쯤이다. 10초면 넉넉하면서도
            // 여전히 유한하다.
            for (int step = 0; step < 600; ++step) leaning.UpdateZombies(1.0f / 60.0f);
            if (leaning.grid.Get(breach->breachPosition) != Tile::DoorBroken)
            {
                std::cerr << "FAIL breach pressure test: woken zombies must break their own way out\n";
                return 1;
            }
        }
            return 0;
        }

        // 게임이 타일에 대해 가진 규칙 전부를 여기 **다시 적고** Grid 의 답과
        // 비교한다. 이것이 헌법이다 — 무엇을 밟을 수 있고, 무엇이 시야를 막고,
        // 한 걸음이 얼마이고, 소리가 넘어가는 데 얼마를 치르는가.
        //
        // 기댓값을 Grid 가 쓰는 것에서 읽어오지 않고 **다시 적는 것이 요점이다.**
        // 답이면서 동시에 검사인 표는 자기 안의 오타를 잡을 수 없다.
        // MapValidation 과 이 파일이 그래프 비교를 각자 들고 있는 것과 같은 이유다.
        static int TileRules()
        {
            constexpr float Blocked = 1.0e9f;
            struct Expected
            {
                Tile tile;
                const char* name;
                bool walkable;
                bool opaque;
                float traversal;
                float acoustic;
                bool closedDoor;
                bool sealedDoor;
                bool sealedWindow;
            };
            // 헌법 전문을 한 쪽에. **이 목록에 없는 타일은 아무도 규칙을 정하지
            // 않은 타일이다.**
            constexpr Expected table[] = {
                { Tile::Wall,               "Wall",               false, true,  Blocked, 6.0f, false, false, false },
                { Tile::Floor,              "Floor",              true,  false, 1.0f,    1.0f, false, false, false },
                { Tile::DoorClosed,         "DoorClosed",         true,  true,  3.0f,    3.0f, true,  false, false },
                { Tile::DoorWarning,        "DoorWarning",        true,  true,  3.0f,    3.0f, true,  false, false },
                { Tile::DoorCracked,        "DoorCracked",        true,  true,  2.5f,    3.0f, true,  false, false },
                { Tile::DoorBroken,         "DoorBroken",         true,  false, 1.0f,    1.0f, false, false, false },
                { Tile::DoorOpen,           "DoorOpen",           true,  false, 1.0f,    1.0f, false, false, false },
                { Tile::DoorSealed,         "DoorSealed",         false, true,  Blocked, 3.0f, false, true,  false },
                { Tile::DoorSealedWarning,  "DoorSealedWarning",  false, true,  Blocked, 3.0f, false, true,  false },
                { Tile::DoorSealedCracked,  "DoorSealedCracked",  false, true,  Blocked, 3.0f, false, true,  false },
                { Tile::WindowIntact,       "WindowIntact",       false, true,  Blocked, 4.0f, false, false, true  },
                { Tile::WindowWarning,      "WindowWarning",      false, true,  Blocked, 4.0f, false, false, true  },
                { Tile::WindowCracked,      "WindowCracked",      false, true,  Blocked, 4.0f, false, false, true  },
                { Tile::WindowBroken,       "WindowBroken",       true,  false, 2.0f,    1.0f, false, false, false },
                { Tile::ExitLocked,         "ExitLocked",         true,  false, 1.0f,    1.0f, false, false, false },
                { Tile::ExitOpen,           "ExitOpen",           true,  false, 1.0f,    1.0f, false, false, false }
            };
            static_assert(std::size(table) == static_cast<std::size_t>(Tile::ExitOpen) + 1,
                "every Tile needs a row here, or a rule was decided for it nowhere");

            // 칸마다 타일 하나씩. 그래야 규칙이 이웃 칸에서 읽히는 일이 없다.
            Grid board(static_cast<int>(std::size(table)), 1);
            for (std::size_t index = 0; index < std::size(table); ++index)
            {
                board.Set({ static_cast<int>(index), 0 }, table[index].tile);
            }

            for (std::size_t index = 0; index < std::size(table); ++index)
            {
                const Expected& want = table[index];
                const Int2 at{ static_cast<int>(index), 0 };
                if (board.Get(at) != want.tile)
                {
                    std::cerr << "FAIL tile rules: " << want.name << " did not survive being stored\n";
                    return 1;
                }
                const auto bad = [&want](const char* rule)
                {
                    std::cerr << "FAIL tile rules: " << want.name << " disagrees on " << rule << '\n';
                    return 1;
                };
                if (board.IsWalkable(at) != want.walkable) return bad("walkable");
                if (board.IsOpaque(at) != want.opaque) return bad("opaque");
                if (board.TraversalCost(at) != want.traversal) return bad("traversal cost");
                if (board.AcousticCost(at) != want.acoustic) return bad("acoustic cost");
                if (IsClosedDoor(want.tile) != want.closedDoor) return bad("IsClosedDoor");
                if (IsSealedDoor(want.tile) != want.sealedDoor) return bad("IsSealedDoor");
                if (IsSealedWindow(want.tile) != want.sealedWindow) return bad("IsSealedWindow");

                // 규칙을 나란히 늘어놓고 나서야 보인 불변식 셋. **처음부터 참이었지만
                // switch 일곱 개에 흩어져 있어서 아무도 한눈에 볼 수 없었다.**
                //
                // 통행과 비용은 한 문장이다 — 못 지나는 타일은 정확히 막는 값을 가진
                // 타일이다. 갈라 두면 어긋날 수 있고, 비용 1.0e9f 인데 통행 가능한
                // 타일이 있으면 A* 가 그 위에서 영영 멈춘다.
                if (want.walkable != (want.traversal < Blocked)) return bad("walkable against its cost");
                // 시야를 막는 것과 소리를 깎는 것도 한 문장이다. 소리는 벽을
                // 넘어가되 값을 치른다. 빛을 막으면서 소리에 아무것도 안 매기는
                // 타일은 **소리가 그냥 통과하는 벽**이다.
                if (want.opaque != (want.acoustic > 1.0f)) return bad("opaque against its acoustic cost");
                // 문·창문 술어 셋은 서로 다른 세 상호작용을 뜻한다 — E 가 연다,
                // 압력만 연다, 총알이 연다. **둘이 동시에 참인 것은 타일이 아니라
                // 버그다.**
                if (static_cast<int>(want.closedDoor) + static_cast<int>(want.sealedDoor)
                    + static_cast<int>(want.sealedWindow) > 1)
                {
                    return bad("belonging to more than one interaction");
                }
            }

            // 밀폐 포켓이 기대는 규칙 하나를 소리 내어 말해둔다. **실수처럼
            // 읽히기 때문이다** — 막힌 문은 걷는 모든 것을 막으면서 소리에는 평범한
            // 문과 똑같은 값만 매긴다. 벽 수준으로 올리면 그 뒤의 좀비가 아무것도
            // 못 듣고, 침입구에 압력이 안 쌓이고, 그들이 한 판 내내 거기서 기다린다.
            if (Grid(1, 1).AcousticCost({ 0, 0 }) != 6.0f)
            {
                std::cerr << "FAIL tile rules: a fresh grid must be solid wall\n";
                return 1;
            }
            return 0;
        }

        // 자원으로서의 달리기, 그리고 그것을 고정량 뜯어가는 구르기.
        static int Stamina()
        {
        // 스태미나. 달리기가 자원이 아니면 플레이어가 빈 복도에서 모든 좀비를
        // 영원히 따돌린다.
        {
            const Balance defaults = Balance::Defaults();
            Grid hall(40, 3);
            for (int x = 0; x < 40; ++x) hall.Set({ x, 1 }, Tile::Floor);

            Game runner(20260826u, defaults);
            runner.grid = hall;
            runner.breachSystem.Initialize(runner.grid);
            runner.zombies.clear();
            runner.playerPosition = TileCenter({ 1, 1 });
            runner.playerAngle = 0.0f;

            InputState sprint;
            sprint.forward = true;
            sprint.run = true;
            for (int step = 0; step < 60; ++step) runner.UpdatePlayer(1.0f / 60.0f, sprint);
            if (std::abs(runner.stamina - (defaults.staminaMax - 1.0f)) > 0.05f)
            {
                std::cerr << "FAIL stamina test: a second of running must cost a second of stamina\n";
                return 1;
            }

            // 바닥까지 쓴 다음 다음 걸음을 잰다. **빈 바는 플레이어를 질주 속도가
            // 아니라 걷기 속도로 움직여야 한다.**
            for (int step = 0; step < 60 * 12; ++step) runner.UpdatePlayer(1.0f / 60.0f, sprint);
            if (runner.stamina > 0.0001f)
            {
                std::cerr << "FAIL stamina test: running must be able to empty the bar\n";
                return 1;
            }
            runner.playerPosition = TileCenter({ 1, 1 });
            const Vec2 before = runner.playerPosition;
            runner.UpdatePlayer(1.0f / 60.0f, sprint);
            const float travelled = Length(runner.playerPosition - before) * 60.0f;
            if (std::abs(travelled - defaults.playerWalkSpeed) > 0.2f)
            {
                std::cerr << "FAIL stamina test: an empty bar must drop the sprint to a walk\n";
                return 1;
            }

            // 회복은 유예를 기다린 뒤 자기 속도로 돌아온다.
            InputState still;
            for (int step = 0; step < 30; ++step) runner.UpdatePlayer(1.0f / 60.0f, still);
            if (runner.stamina > 0.0001f)
            {
                std::cerr << "FAIL stamina test: recovery must wait out the delay\n";
                return 1;
            }
            for (int step = 0; step < 90; ++step) runner.UpdatePlayer(1.0f / 60.0f, still);
            if (std::abs(runner.stamina - 1.0f) > 0.05f)
            {
                std::cerr << "FAIL stamina test: recovery must return one a second after the delay\n";
                return 1;
            }
        }

        // 구르기는 스태미나를 고정량 쓰고 자기 짧은 쿨다운을 갖는다. 그래야 달리기보다
        // 빠른 이동 수단으로 이어 붙일 수 없다.
        {
            const Balance defaults = Balance::Defaults();
            Grid hall(40, 3);
            for (int x = 0; x < 40; ++x) hall.Set({ x, 1 }, Tile::Floor);

            Game roller(20260826u, defaults);
            roller.grid = hall;
            roller.breachSystem.Initialize(roller.grid);
            roller.zombies.clear();
            roller.playerPosition = TileCenter({ 1, 1 });
            roller.playerAngle = 0.0f;

            InputState dodge;
            dodge.roll = true;
            roller.UpdatePlayer(1.0f / 60.0f, dodge);
            if (std::abs(roller.stamina - (defaults.staminaMax - defaults.staminaRollCost)) > 0.05f)
            {
                std::cerr << "FAIL roll test: a roll must cost its share of stamina\n";
                return 1;
            }
            if (std::abs(roller.rollCooldown - defaults.rollCooldown) > 0.05f)
            {
                std::cerr << "FAIL roll test: a roll must start its own cooldown\n";
                return 1;
            }

            // 바가 모자라면 구르기는 그냥 일어나지 않는다.
            roller.previousRoll = false;
            roller.rollCooldown = 0.0f;
            roller.rollTimer = 0.0f;
            roller.stamina = defaults.staminaRollCost - 0.5f;
            const float leftover = roller.stamina;
            roller.UpdatePlayer(1.0f / 60.0f, dodge);
            if (roller.rollTimer > 0.0f || roller.stamina < leftover - 0.0001f)
            {
                std::cerr << "FAIL roll test: an empty bar must refuse the roll outright\n";
                return 1;
            }
        }
            return 0;
        }

        // 출하 설정이 내장 기본값과 일치하는가, HUD 글자가 정말로 시스템 글꼴에서
        // 나오는가, 그리고 프레임이 **자기가 주장한 셀에만** 쓰는가.
        static int ConfigAndText(const std::filesystem::path& executable)
        {
        // 출하 설정과 내장 기본값의 비교. 둘은 **한 게임을 두 번 적은 것**이고
        // 실행 시점에 이기는 것은 설정 쪽이라, 둘이 어긋나면 헤더가 아무도 죽은 줄
        // 모르는 죽은 글이 된다.
        //
        // **가정이 아니다.** 값 여섯 개를 헤더에서 튜닝했는데 ini 가 옛것을 들고
        // 있었고, 실행할 때마다 조용히 옛것이 복원됐다 — **밸런스 작업 한 라운드가
        // 통째로 게임에 도달하지 못했다.** 이 검사가 잡는 것이 그것이다.
        {
            // 이기는 하나가 아니라 **검색 경로의 모든 사본**을 본다. 이기는 것만
            // 검사하면 이 테스트가 버그와 정확히 같은 방식으로 눈이 먼다 —
            // 빌드가 실행 파일 옆에 사본을 떨구고 그것이 먼저 발견되므로,
            // 저장소 설정의 변경은 읽히지도 않았다.
            const std::vector<std::filesystem::path> configPaths =
                Balance::FindConfigFiles(executable);
            if (configPaths.empty())
            {
                // 따로 떼어낸 실행 파일 사본은 비교할 대상이 없다. 조용히 통과하는
                // 대신 그렇다고 말한다. 아니면 검사가 돈 것처럼 보인다.
                std::cerr << "NOTE: no Config/GameBalance.ini found; skipped the balance agreement check\n";
            }
            const Balance builtIn = Balance::Load({});
            for (const std::filesystem::path& configPath : configPaths)
            {
                const std::vector<Balance::Difference> differences =
                    Balance::Load(configPath).DifferencesFrom(builtIn);
                if (differences.empty()) continue;
                std::cerr << "FAIL balance test: " << configPath.string()
                    << " disagrees with the built-in defaults:\n";
                // 양쪽 값을 다 찍는다. 그래야 해결이 조회가 아니라 복사 한 번이 된다.
                for (const Balance::Difference& entry : differences)
                {
                    std::cerr << "    " << entry.key << "  config " << entry.here
                        << "  built-in " << entry.there << '\n';
                }
                std::cerr << "  A config wins over the defaults at run time, so the config column"
                    " is what the game really plays.\n  Decide which side is right and change"
                    " both, then rebuild so the copy beside the executable follows.\n";
                return 1;
            }
        }

        // 글자 오버레이. 예전에는 HUD 가 터미널 셀에 진짜 문자를 넣고 터미널 글꼴이
        // 그리게 했는데, 그러면 크기가 글꼴이 말하는 대로였고 **게임에 발언권이 없었다.**
        // 지금은 시스템 글꼴에서 구운 픽셀이라 프레임을 따라 커진다.
        //
        // 붙들어야 할 것이 셋이다 — GDI 가 정말로 한글 잉크를 내는가, 결과가 캐시되는가,
        // 그리고 그린 것이 가장자리를 넘지 않고 프레임 안에 떨어지는가.
        {
            // 글꼴 폴백. 아래의 모든 것이 **경험적 주장 하나**에 기대고 있다 —
            // 설치되지 않은 글꼴에도 CreateFontW 가 성공하고, GDI 가 선택 시점에
            // 다른 것으로 대체하며, 그 대체가 GetTextFaceW 에 드러난다.
            //
            // 어떤 기기에서 그 주장이 거짓이면 후보 목록은 장식이다. **2026-08-29
            // 까지가 정확히 그랬다** — 되읽기를 아무도 비교하지 않았고, 첫 항목
            // 아래의 목록에 도달할 수 없었다.
            {
                const std::string phantom = "No Such Face 12345";
                const std::string substituted = TextRaster::ResolveFace(phantom, 12);
                if (substituted.empty())
                {
                    std::cerr << "FAIL font test: GDI must resolve a request to some face\n";
                    return 1;
                }
                if (TextRaster::FaceMatches(phantom, substituted))
                {
                    std::cerr << "FAIL font test: a face that is not installed must not"
                        " report itself as loaded; the fallback cannot work without that\n";
                    return 1;
                }

                // 비교기 자체. 대소문자는 무의미하고, 다른 글꼴은 일치가 아니며,
                // 한국어 Windows 에서는 글꼴이 한국어 이름으로 답할 수 있다.
                // **마지막 항목이 없으면 맑은 고딕을, 그것을 가장 갖고 있을 법한
                // 기기에서 거절한다.**
                if (!TextRaster::FaceMatches("Consolas", "consolas")
                    || !TextRaster::FaceMatches("Malgun Gothic", "맑은 고딕")
                    || TextRaster::FaceMatches("Malgun Gothic", "Noto Sans KR"))
                {
                    std::cerr << "FAIL font test: the face matcher must accept a Korean"
                        " name and reject a different face\n";
                    return 1;
                }

                // 그리고 HUD 가 결국 물린 것이 **정말로 존재하는 글꼴**인지.
                // 아무도 확인 안 한 또 하나의 유령이 아니라.
                TextRaster::Bake("남은 체력", 12);
                const std::string loaded = TextRaster::FaceName();
                if (loaded.empty() || !TextRaster::FaceMatches(loaded,
                        TextRaster::ResolveFace(loaded, 12)))
                {
                    std::cerr << "FAIL font test: the loaded HUD face must be one GDI"
                        " actually honours, got \"" << loaded << "\"\n";
                    return 1;
                }
                // 답이 기기마다 다르므로 찍어둔다. **"발표용 노트북에서 HUD 가
                // 안 읽힌다"는 이야기가 여기서 시작된다.**
                std::cout << "HUD font " << loaded << "\n";
            }

            // 같은 이유로 한 줄 더. 프레임이 안 나온다는 이야기는 **여기서** 시작된다.
            //
            // 고해상도 타이머가 없으면 루프의 1밀리초 대기가 실제로는 15.63ms 이고,
            // 그러면 render_fps 를 아무리 올려도 50 언저리가 천장이다. 설정 파일에는
            // 아무 잘못이 없는데 설정 파일을 의심하게 되는 종류의 고장이라, 답을
            // 사람이 재기 전에 게임이 먼저 말한다.
            //
            // **깃발이 아니라 실측을 찍는다.** 이 저장소는 손으로 적어둔 숫자가
            // 낡는 것을 이미 한 번 겪었고, 그 결론이 "계측을 코드에 둔다"였다.
            // 부탁한 1밀리초가 실제로 몇 밀리초인지가
            // 곧 이 기기의 프레임 천장이다.
            {
                const FrameSleeper sleeper;
                constexpr int rounds = 100;
                const auto start = std::chrono::steady_clock::now();
                for (int round = 0; round < rounds; ++round)
                {
                    sleeper.Sleep(std::chrono::milliseconds(1));
                }
                const float each = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - start).count() / rounds;
                // 렌더 한 장에 3.4ms 를 쓰므로 한 바퀴가 (잠 + 3.4)ms 다.
                const float ceiling = 1000.0f / (each + 3.4f);
                std::cout << "Frame sleeper " << each << "ms per 1ms request"
                    << (sleeper.IsHighResolution() ? "" : " (no high-resolution timer)")
                    << ", so about " << static_cast<int>(ceiling) << " FPS is the ceiling\n";
                if (each > 8.0f)
                {
                    // 실패는 아니다 — 게임은 돈다. 하지만 render_fps 의 위쪽 절반이
                    // 도달 불가능하다는 것은 **밸런스 파일을 의심하기 전에** 알아야 한다.
                    std::cerr << "NOTE: a 1ms sleep really takes " << each
                        << "ms here, so render_fps above " << static_cast<int>(ceiling)
                        << " cannot be reached on this machine\n";
                }
            }

            const std::string label = "남은 체력";
            const TextRaster::Mask& mask = TextRaster::Bake(label, 12);
            if (mask.width <= 0 || mask.height <= 0 || mask.coverage.empty())
            {
                std::cerr << "FAIL overlay test: the system font must rasterise Hangul\n";
                return 1;
            }
            const std::size_t ink = static_cast<std::size_t>(
                std::count(mask.coverage.begin(), mask.coverage.end(), std::uint8_t{ 255 }));
            // 네 음절과 공백 하나. **빈 출력은 경계 검사를 통과하면서 화면에
            // 아무것도 안 놓는데**, 실제로 일어난 실패가 그것이다.
            if (ink < 40 || mask.width <= mask.height)
            {
                std::cerr << "FAIL overlay test: the baked label must carry ink and run wide\n";
                return 1;
            }
            if (&TextRaster::Bake(label, 12) != &mask)
            {
                std::cerr << "FAIL overlay test: a repeated string must come from the cache\n";
                return 1;
            }
            if (TextRaster::MeasureWidth(label + label, 12) <= mask.width
                || TextRaster::MeasureWidth(label, 24) <= mask.width)
            {
                std::cerr << "FAIL overlay test: width must follow the text and the size\n";
                return 1;
            }

            PixelBuffer sheet(mask.width + 8, mask.height + 8);
            sheet.Clear({ 10, 20, 30 });
            const Rgb pen{ 200, 40, 40 };
            TextRaster::Draw(sheet, { { 4, 4 }, label, pen, 12 });
            std::size_t drawn = 0;
            for (int y = 0; y < sheet.Height(); ++y)
                for (int x = 0; x < sheet.Width(); ++x)
                    if (sheet.Get(x, y) == pen) ++drawn;
            if (drawn != ink)
            {
                std::cerr << "FAIL overlay test: every ink pixel must reach the frame\n";
                return 1;
            }
            // 테두리는 뒤에 밝은 벽이 있어도 글자가 살아남게 하려고 있다.
            // 잉크 픽셀의 이웃 중 **자기가 잉크가 아닌 것은 전부** 테두리를 져야 한다.
            //
            // 예전에는 (3,4) 와 (4,3) 을, 마스크 원점에서 한 픽셀 나간 자리를 찔렀다.
            // 그건 **첫 글리프가 마침 (0,0) 에 잉크를 갖고 있을 때만** 성립하는데,
            // 그것은 외곽선의 성질이 아니라 글꼴의 성질이다. 돋움체의 내장 비트맵은
            // 갖고 있고 Noto Sans KR 은 거기에 사이드 베어링을 남긴다.
            // **아무것도 안 망가뜨린 글꼴 교체에서 검사가 실패했다.**
            constexpr Rgb rim{ 8, 8, 10 };
            for (int y = 0; y < mask.height; ++y)
            {
                for (int x = 0; x < mask.width; ++x)
                {
                    if (!mask.Ink(x, y)) continue;
                    for (int offsetY = -1; offsetY <= 1; ++offsetY)
                    {
                        for (int offsetX = -1; offsetX <= 1; ++offsetX)
                        {
                            if (mask.Ink(x + offsetX, y + offsetY)) continue;
                            if (sheet.Get(4 + x + offsetX, 4 + y + offsetY) == rim) continue;
                            std::cerr << "FAIL overlay test: glyphs must be outlined\n";
                            return 1;
                        }
                    }
                }
            }

            PixelBuffer clean(mask.width + 8, mask.height + 8);
            clean.Clear({ 10, 20, 30 });
            PixelBuffer offscreen = clean;
            TextRaster::Draw(offscreen, { { 4, 999 }, label, pen, 12 });
            if (offscreen.Pixels() != clean.Pixels())
            {
                std::cerr << "FAIL overlay test: text off the frame must change nothing\n";
                return 1;
            }
        }

        // 프레임 조립. 큰 모니터에서 비용을 묶는 것이 출력 상한 하나뿐이므로,
        // **자기가 주장한 셀에만 정확히** 쓰고 다른 데는 안 써야 한다.
        {
            PixelBuffer plate(16, 8);
            plate.Clear({ 10, 20, 30 });
            const auto halfBlocks = [](const std::string& text)
            {
                std::size_t count = 0;
                for (std::size_t at = text.find("\xE2\x96\x80"); at != std::string::npos;
                    at = text.find("\xE2\x96\x80", at + 3))
                {
                    ++count;
                }
                return count;
            };

            const std::string full = TerminalSession::ComposeFrame(plate, 16, 4);
            if (halfBlocks(full) != 64)
            {
                std::cerr << "FAIL frame test: a full frame must fill every cell\n";
                return 1;
            }
            // 더 넓은 뷰포트 가운데의 블록으로 제한한 경우. 여백은 Present() 가
            // 한 번 칠하고, **그 뒤로 어떤 프레임에도 나타나면 안 된다.**
            const std::string limited = TerminalSession::ComposeFrame(plate, 8, 2, 4, 1);
            if (halfBlocks(limited) != 16)
            {
                std::cerr << "FAIL frame test: a limited frame must write only its own cells\n";
                return 1;
            }
            if (limited.find("\x1b[2;5H") != 0 || limited.find("\x1b[3;5H") == std::string::npos)
            {
                std::cerr << "FAIL frame test: every row must start at the frame origin\n";
                return 1;
            }
        }
            return 0;
        }

        // 플레이어는 안 돌고 카메라만 도는가, 그리고 사격이 거절되는가.
        //
        // 조준 흔들림과 사격 반동도 여기다. 7.x 는 조준선이 흔들리는 것은 허용하되
        // **숨은 산탄은 금지한다** — 총알은 그려진 조준선이 있는 자리로 정확히 간다.
        // 그것이 성립하는 조건은 둘이다. 변위가 누가 읽기 전에 **한 번만** 적용되고,
        // 그 어느 부분도 난수가 아닐 것.
        static int AimSway()
        {
            const auto drive = [](Game& game, int steps, bool moving)
            {
                InputState input;
                input.forward = moving;
                for (int step = 0; step < steps; ++step) game.Update(1.0f / 60.0f, input);
            };

            // **한 번만 적용된다.** state.aimPixel 은 생포인터 위치에 그 상태가 같이
            // 보고하는 오프셋을 더한 값이어야 한다. 아니면 둘이 갈라선 것이고 그중
            // 하나는 거짓말이다. **어디서든 두 번째로 적용하면 여기서 걸린다.**
            {
                Game game(20260826u, Balance::Defaults());
                drive(game, 30, true);
                const RenderState state = game.MakeRenderState(InputState{});
                const Int2 expected{
                    std::clamp(game.aimPixel.x + static_cast<int>(std::lround(state.aimOffset.x)),
                        0, game.renderer.Width() - 1),
                    std::clamp(game.aimPixel.y + static_cast<int>(std::lround(state.aimOffset.y)),
                        0, game.renderer.Height() - 1) };
                if (!(state.aimPixel == expected))
                {
                    std::cerr << "FAIL aim sway test: the drawn crosshair must be the pointer plus"
                        " the reported offset, applied exactly once\n";
                    return 1;
                }
                if (game.aimSway <= 0.0f)
                {
                    std::cerr << "FAIL aim sway test: walking must unsettle the aim\n";
                    return 1;
                }
            }

            // 재현 가능한가. 7.x 가 난수원을 금지하고, 이 프로젝트의 오라클들이
            // 같은 입력이 같은 프레임을 낸다는 데 기대고 있다. 같은 입력 흐름을 두 번
            // 돌리면 **픽셀 단위로** 일치해야 한다.
            {
                Game first(20260826u, Balance::Defaults());
                Game second(20260826u, Balance::Defaults());
                drive(first, 47, true);
                drive(second, 47, true);
                const RenderState a = first.MakeRenderState(InputState{});
                const RenderState b = second.MakeRenderState(InputState{});
                if (!(a.aimPixel == b.aimPixel) || std::abs(a.aimOffset.x - b.aimOffset.x) > 1.0e-5f)
                {
                    std::cerr << "FAIL aim sway test: the same inputs must give the same drift\n";
                    return 1;
                }
            }

            // 가라앉는가, 그리고 설정이 약속한 시간 안에 그러는가. 0에 도달하지 않는
            // 흔들림은 **가만히 서도 조준이 안정되지 않는다**는 뜻이고, 그 거래가
            // 이 메커닉의 존재 이유다.
            {
                Game game(20260826u, Balance::Defaults());
                const Balance tuning = Balance::Defaults();
                drive(game, 30, true);
                const int settleSteps = static_cast<int>(tuning.aimSwaySettle * 60.0f) + 2;
                drive(game, settleSteps, false);
                if (game.aimSway != 0.0f)
                {
                    std::cerr << "FAIL aim sway test: the aim must settle to nothing once the"
                        " player stops, within aim_sway_settle\n";
                    return 1;
                }
            }

            // 반동이 조준선까지 닿는가, 그리고 **총알이 실제로 나간 뒤에만** 그러는가.
            // 빈 탄창은 조준을 흔들면 안 된다.
            {
                Game game(20260826u, Balance::Defaults());
                game.ammo = 1;
                game.Shoot();
                if (game.weaponRecoil <= 0.0f)
                {
                    std::cerr << "FAIL aim sway test: firing must start the kick\n";
                    return 1;
                }
                const Int2 kicked = game.MakeRenderState(InputState{}).aimPixel;
                if (kicked.y >= game.aimPixel.y)
                {
                    std::cerr << "FAIL aim sway test: the shot kick must lift the crosshair\n";
                    return 1;
                }
                game.weaponRecoil = 0.0f;
                game.ammo = 0;
                game.Shoot();
                if (game.weaponRecoil != 0.0f)
                {
                    std::cerr << "FAIL aim sway test: a refused trigger must not kick\n";
                    return 1;
                }
            }

            // 총알이 **자기 반동보다 먼저** 판정되는가. 7.x 는 총알이 그려진 조준선이
            // 있는 자리로 가기를 원하는데 반동이 그 조준선을 움직이므로, 명중 판정
            // 위에서 반동을 시작하면 **플레이어가 겨눈 적 없는 자리**에서 판정된다.
            //
            // 평범한 반동은 2픽셀이고 좀비 스프라이트는 그보다 훨씬 넓어서,
            // **자연스러운 설정으로는 두 순서를 구별할 수 없다.** 그래서 터무니없는
            // 값을 요청한다 — 화면 절반. 올바른 순서는 조준선에 서 있는 좀비를 죽이고,
            // 틀린 순서는 조준을 천장으로 던져서 좀비가 산다.
            {
                Balance loud = Balance::Defaults();
                loud.aimKickUpPixels = 40.0f;
                Game game(20260826u, loud);
                game.ammo = 1;
                const Vec2 ahead{ std::cos(game.playerAngle), std::sin(game.playerAngle) };
                game.zombies.front().position = game.playerPosition + ahead * 2.0f;
                game.zombies.front().alive = true;
                game.zombies.front().waitingBehindBreach = false;
                game.aimPixel = { game.renderer.Width() / 2, game.renderer.Height() / 2 };
                game.Shoot();
                if (game.zombies.front().alive)
                {
                    std::cerr << "FAIL aim sway test: the shot must resolve before its own kick,"
                        " at the crosshair the player aimed with\n";
                    return 1;
                }
            }
            return 0;
        }

        // 소리. 붙들어야 할 것이 둘인데 **둘 다 "들린다"가 아니다** —
        // 파서가 실제 자산의 모양을 견뎌야 하고, 재생할 장치가 없을 때 모든 큐가
        // 무해해야 한다.
        static int Audio()
        {
            // fmt 와 data 사이에 LIST 청크가 있는 wav. **출하 자산 둘 다 정확히
            // 이렇게 생겼고**, 그래서 샘플이 최소한의 라이터가 놓을 위치보다 한참 뒤에서
            // 시작한다. 고정 위치에서 읽는 파서는 **메타데이터를 오디오로 돌려주고
            // 그렇다고 말하지도 않는다.**
            {
                const std::vector<std::uint8_t> file = {
                    'R','I','F','F', 0x38,0,0,0, 'W','A','V','E',
                    'f','m','t',' ', 16,0,0,0,
                    1,0,                    // PCM
                    2,0,                    // stereo
                    0x44,0xAC,0,0,          // 44100
                    0x10,0xB1,0x02,0,       // bytes per second
                    4,0,                    // block align
                    16,0,                   // bits
                    'L','I','S','T', 8,0,0,0, 'I','N','F','O','x','x','x','x',
                    'd','a','t','a', 8,0,0,0,
                    // 평균이 **어느 채널과도 안 같은** 프레임 둘 — 10과 20의 평균은
                    // 15, -30과 -10의 평균은 -20. 일부러 그렇게 골랐다.
                    //
                    // 이 검사의 첫 버전은 1 차이 나는 값을 썼는데, 그러면 정수 평균이
                    // 마침 왼쪽 채널에 떨어져서 **오른쪽을 그냥 버리는 다운믹스도
                    // 통과했다.**
                    0x0A,0x00, 0x14,0x00, 0xE2,0xFF, 0xF6,0xFF
                };
                const WavData clip = ParseWav(file.data(), file.size());
                if (!clip.Valid() || clip.sampleRate != 44100 || clip.channels != 2)
                {
                    std::cerr << "FAIL audio test: the parser must walk past a LIST chunk"
                        " to find fmt and data\n";
                    return 1;
                }
                if (clip.samples.size() != 4 || clip.samples[0] != 10 || clip.samples[1] != 20
                    || clip.samples[2] != -30 || clip.samples[3] != -10)
                {
                    std::cerr << "FAIL audio test: samples must come from the data chunk,"
                        " not from a fixed offset\n";
                    return 1;
                }
                // 다운믹스가 World 큐를 좌우로 배치 가능하게 만든다. 출력 행렬이
                // 원본 채널 × 출력 채널이고, **모노 원본만이 채널을 접지 않고
                // 배치되는 1x2 를 준다.**
                WavData mono = clip;
                DownmixToMono(mono);
                if (mono.channels != 1 || mono.samples.size() != 2
                    || mono.samples[0] != 15 || mono.samples[1] != -20)
                {
                    std::cerr << "FAIL audio test: a downmix must average the channels\n";
                    return 1;
                }
                // PCM 16 이 아닌 것은 오류가 아니라 조용한 큐다.
                std::vector<std::uint8_t> compressed = file;
                compressed[20] = 0x55;      // some codec that is not PCM
                if (ParseWav(compressed.data(), compressed.size()).Valid())
                {
                    std::cerr << "FAIL audio test: a format this cannot decode must not load\n";
                    return 1;
                }
            }

            // 장치를 열지 않은 채로 모든 큐를 울려본다. 자체 테스트는 Game 을 직접
            // 만들어 Shoot, EmitNoise, 줍기를 돌리므로, **여기서의 침묵이 사운드
            // 카드가 아예 없는 기기에서도 돌게 해주는 것**이다.
            {
                Game game(20260826u, Balance::Defaults());
                if (game.audio.IsOpen())
                {
                    std::cerr << "FAIL audio test: constructing a Game must not open a device\n";
                    return 1;
                }
                const int health = game.health;
                const int ammo = game.ammo;
                const std::uint64_t before = game.grid.StableHash();
                for (int index = 0; index < static_cast<int>(AudioCue::Count); ++index)
                {
                    const AudioCue cue = static_cast<AudioCue>(index);
                    game.audio.Play(cue);
                    game.audio.PlayPositional(cue, 0.5f, -0.5f);
                    game.audio.PlayMusic(cue);
                    game.PlayWorldCue(cue, game.grid.keyPosition);
                }
                game.audio.StopMusic();
                if (game.health != health || game.ammo != ammo || game.grid.StableHash() != before)
                {
                    std::cerr << "FAIL audio test: playing a cue must not touch game state\n";
                    return 1;
                }
                // 그리고 그 뒤로도 게임이 계속 돈다.
                InputState input;
                input.forward = true;
                for (int step = 0; step < 10; ++step) game.Update(1.0f / 60.0f, input);
                if (game.won || game.lost)
                {
                    std::cerr << "FAIL audio test: the game must carry on with no device\n";
                    return 1;
                }
            }
            return 0;
        }

        // 뒤돌아보기와 조준 흔들림·사격 반동.
        static int LookBehind()
        {
        // 뒤돌아보기. 전체가 기대는 규칙은 **카메라만 돌고 다른 것은 안 돈다**는
        // 것이다 — 몸은 여전히 앞을 향하고 사격은 거절된다. 둘을 만나게 하면 그 키가
        // 공짜 즉시 회전이 되는데, 느린 A/D 회전이 막으려는 것이 정확히 그것이다.
        {
            RenderState looking;
            looking.playerAngle = 0.7f;
            looking.lookingBehind = true;
            if (std::abs((looking.ViewAngle() - looking.playerAngle) - 3.14159265f) > 1.0e-4f)
            {
                std::cerr << "FAIL look-behind test: the view must turn exactly half a circle\n";
                return 1;
            }
            looking.lookingBehind = false;
            if (looking.ViewAngle() != looking.playerAngle)
            {
                std::cerr << "FAIL look-behind test: the view must follow the body when not held\n";
                return 1;
            }

            // 어떤 각도에서 뒤를 보는 것은 반대 방향을 향해 선 것과 **같은 그림**이어야
            // 한다. 이것이 그리기 함수가 조용히 playerAngle 로 돌아가는 것을 잡는
            // 검사인데, **그 함수가 그리는 것이 실제로 화면에 있어야만** 잡는다.
            //
            // 이 검사의 첫 버전은 시작 방에서 각도 하나를 비교했고, 거기엔 그릴 좀비가
            // 없어서 **playerAngle 로 되돌아간 스프라이트가 그대로 통과했다.**
            //
            // 그래서 매번 보는 방향에 배우를 심고 각도 여덟 개를 훑는다. 아래쪽 띠를
            // 빼는 이유는 무기와 조준선이야말로 뒤돌아보기가 **일부러 숨기는 것**이기
            // 때문이다.
            //
            // 띠는 무기가 **실제로** 얼마나 높든 덮어야 하는데, 그건 상수가 아니다 —
            // 스프라이트가 프레임에서 배율을 받고 그 다음에 회전하므로, 총구가 얼마나
            // 위로 올라가는지는 회전이 정한다. 120행 중 40행이면 지금 기울기가
            // 필요로 하는 34행 위로 여유가 있다.
            //
            // **이 검사는 망가진 것이 아니라 진짜 변경을 잡았다** — 총을 아래가 아니라
            // 위로 겨누게 했더니 비교 영역 안으로 올라왔고, 첫 실행에서 테스트가
            // 그렇다고 말했다.
            MapGenerator viewGenerator{ MapGenerationSettings{} };
            Grid viewGrid = viewGenerator.Generate(20260826u);
            constexpr int viewWidth = 160;
            constexpr int viewHeight = 120;
            constexpr int viewComparedRows = viewHeight - 40;
            RaycastRenderer forward(viewWidth, viewHeight);
            RaycastRenderer behind(viewWidth, viewHeight);

            // 시작 방에 서 있으면 벽과 심어놓은 배우밖에 안 돈다. 열쇠·탄약·출입구는
            // **각자의 함수가 그리므로**, 그것들에서 두 타일 못 미친 자리에서도 훑는다.
            // 실제 경로를 따라 잡은 자리라 그 타일이 반드시 바닥이다.
            std::vector<Vec2> viewpoints = { TileCenter(viewGrid.playerSpawn) };
            const auto approachTo = [&](Int2 target)
            {
                const std::vector<Int2> route =
                    AStar::FindPath(viewGrid, viewGrid.playerSpawn, target);
                if (route.size() >= 3) viewpoints.push_back(TileCenter(route[route.size() - 3]));
            };
            approachTo(viewGrid.keyPosition);
            approachTo(viewGrid.exit.origin);
            if (!viewGrid.ammoPickups.empty()) approachTo(viewGrid.ammoPickups.front().position);
            // **갓 생성된 맵에는 흔적이 하나도 없다** — 문이 전부 닫혀 있고 창문이
            // 전부 멀쩡하다. 그러면 DrawOpeningRemnants() 가 아무것도 안 그리고
            // 훑기가 그것의 퇴행을 볼 수 없다. 그래서 문 하나를 열어 프레임을 준다.
            if (!viewGrid.doorways.empty())
            {
                const TilePair opened = viewGrid.doorways.front().tiles;
                viewGrid.Set(opened.origin, Tile::DoorOpen);
                viewGrid.Set(opened.Second(), Tile::DoorOpen);
                approachTo(opened.origin);
            }

            for (int step = 0; step < 8 * static_cast<int>(viewpoints.size()); ++step)
            {
                const float bodyAngle = 0.31f + static_cast<float>(step % 8) * 0.785f;
                const float lookAngle = bodyAngle + 3.14159265f;

                RenderState ahead;
                ahead.playerPosition = viewpoints[static_cast<std::size_t>(step / 8)];
                ahead.playerAngle = lookAngle;
                // 제외된 띠 안이다. 그래야 정면 프레임이 그리는 조준선이 비교
                // 대상 행에 절대 안 떨어진다.
                ahead.aimPixel = { viewWidth / 2, viewHeight - 10 };
                RenderState back = ahead;
                back.playerAngle = bodyAngle;
                back.lookingBehind = true;

                const std::vector<RenderActor> viewActors = { {
                    { ahead.playerPosition.x + std::cos(lookAngle) * 1.5f,
                      ahead.playerPosition.y + std::sin(lookAngle) * 1.5f }, true, true } };

                const std::vector<Rgb> aheadPixels = forward.Render(viewGrid, ahead, viewActors).Pixels();
                const std::vector<Rgb> backPixels = behind.Render(viewGrid, back, viewActors).Pixels();
                const std::size_t compared = static_cast<std::size_t>(viewComparedRows) * viewWidth;
                if (!std::equal(aheadPixels.begin(), aheadPixels.begin() + compared, backPixels.begin()))
                {
                    std::cerr << "FAIL look-behind test: the view must match facing the other way\n";
                    return 1;
                }
                // 그리고 열쇠를 안 든 것과 **다른 그림**이어야 한다. 아니면 위 검사가
                // 열쇠를 통째로 무시하는 프레임에서도 통과한다.
                RenderState unturned = back;
                unturned.lookingBehind = false;
                if (behind.Render(viewGrid, unturned, viewActors).Pixels() == backPixels)
                {
                    std::cerr << "FAIL look-behind test: holding the key must change the picture\n";
                    return 1;
                }
            }

            // 뒤돌아보는 동안 사격이 거절되고, **뒤돌아보는 동안에만** 그렇다.
            Game shooter(20260826u, Balance::Defaults());
            shooter.ammo = 3;
            shooter.lookingBehind = true;
            shooter.Shoot();
            if (shooter.ammo != 3)
            {
                std::cerr << "FAIL look-behind test: looking behind must refuse the shot\n";
                return 1;
            }
            shooter.lookingBehind = false;
            shooter.Shoot();
            if (shooter.ammo != 2)
            {
                std::cerr << "FAIL look-behind test: the same shot must fire when facing forward\n";
                return 1;
            }
        }
            return 0;
        }

        // 줄이 늙어 사라지는가, 반복을 거부하는가, 침입 사건이 거기까지 닿는가.
        //
        // 그리고 3.9 의 화면 흐름. 키가 있어야 **이미 돌고 있는 루프가 아니라 시작과
        // 끝이 있는 게임**이 되므로, 전환 하나하나를 여기 이름 붙이고 각각을 그 앞
        // 상태에서 확인한다.
        //
        // Generating 은 Run() 이 몰고 간다 — 카드를 띄우고 그 밑에서 맵을 만든다.
        // 그 단계를 부르지 않고 아래에 풀어 적은 이유는 **Run() 이 터미널을 필요로
        // 하는데 이 테스트는 그러면 안 되기 때문이다.**
        static int Screens()
        {
            constexpr float step = 1.0f / 60.0f;
            InputState none{};
            InputState confirm{};
            confirm.confirm = true;
            InputState back{};
            back.menu = true;
            InputState sameMap{};
            sameMap.regenerate = true;
            InputState newMap{};
            newMap.newSeed = true;

            const auto fail = [](const char* what)
            {
                std::cerr << "FAIL screen flow test: " << what << "\n";
                return 1;
            };

            {
                // 고정된 경우 — `--seed` 다. 메뉴가 요청받은 맵을 그대로 재현해야 한다.
                Game flow(20260826u, Balance::Defaults(), true);
                flow.screen = Screen::MainMenu;

                // Enter 는 생성을 **예약**하지 수행하지 않는다. Run() 이 카드를 먼저
                // 그리고, 맵은 그 프레임이 화면에 나간 뒤에 만들어진다.
                flow.Update(step, confirm);
                if (flow.screen != Screen::Generating || flow.pendingSeed != flow.seed)
                    return fail("Enter on a pinned seed must replay it");
                flow.Reset(flow.pendingSeed);
                if (flow.screen != Screen::Playing) return fail("a reset must start playing");

                flow.Update(step, none);
                flow.Update(step, back);
                if (flow.screen != Screen::Paused) return fail("Esc while playing must pause");

                // **누르고 있는 키가 흐름을 걸어가면 안 된다.** 같은 Esc 를 누른 채로
                // 세 번 더 돌려도 일시정지 화면이 그 자리에 그대로 있어야 한다.
                for (int held = 0; held < 3; ++held) flow.Update(step, back);
                if (flow.screen != Screen::Paused) return fail("a held Esc must not step twice");

                flow.Update(step, none);
                flow.Update(step, confirm);
                if (flow.screen != Screen::Playing) return fail("Enter must resume from a pause");

                flow.Update(step, none);
                flow.Update(step, back);
                flow.Update(step, none);
                flow.Update(step, back);
                if (flow.screen != Screen::MainMenu)
                    return fail("Esc from a pause must go back to the menu");

                // 그리고 **Esc 가 프로세스를 끝내는 화면은 메뉴 하나뿐이다.**
                flow.Update(step, none);
                flow.Update(step, back);
                if (!flow.quitRequested) return fail("Esc on the menu must quit");
            }

            // 고정이 아닌 경우 — 평범한 실행 — 메뉴는 새 맵을 굴린다.
            // 한 세션의 모든 시작이 같은 맵을 돌려주는 것을 플레이어는
            // **시작 버튼이 고장 난 것**으로 읽는다.
            {
                Game rolling(20260826u, Balance::Defaults());
                rolling.screen = Screen::MainMenu;
                rolling.Update(step, confirm);
                if (rolling.screen != Screen::Generating)
                    return fail("Enter on the menu must arm a run");
                if (rolling.pendingSeed == rolling.seed)
                    return fail("Enter on the menu must roll a new map when no seed was asked for");
            }

            // 일시정지는 **세계를** 멈춘다. "플레이어를 멈춘다"가 아니다 —
            // 좀비도, 판의 시계도 같이 서 있어야 한다. 아니면 일시정지가
            // **화면을 읽는 동안 죽는 방법**이 된다.
            {
                Game frozen(20260826u, Balance::Defaults());
                InputState walking{};
                walking.forward = true;
                for (int warm = 0; warm < 120; ++warm) frozen.Update(step, walking);

                frozen.screen = Screen::Paused;
                const std::vector<ZombieAgent> before = frozen.zombies;
                const Vec2 wherePlayer = frozen.playerPosition;
                const float clock = frozen.runSeconds;
                for (int held = 0; held < 60; ++held) frozen.Update(step, walking);
                if (frozen.runSeconds != clock) return fail("a pause must not count as played time");
                const auto moved = [](Vec2 a, Vec2 b) { return a.x != b.x || a.y != b.y; };
                if (moved(frozen.playerPosition, wherePlayer))
                    return fail("a pause must not let the player move");
                for (std::size_t index = 0; index < before.size(); ++index)
                {
                    if (moved(frozen.zombies[index].position, before[index].position))
                        return fail("a pause must not let the zombies move");
                }

                // 결과 카드가 입력을 거절하는 것도 같은 이유다.
                frozen.screen = Screen::Dead;
                for (int held = 0; held < 60; ++held) frozen.Update(step, walking);
                if (moved(frozen.playerPosition, wherePlayer))
                    return fail("a finished run must not accept movement");
            }

            // 죽음과 탈출이 각자의 카드로 떨어지고, 두 카드가 같은 세 갈래를 내준다.
            // **R 이 같은 맵을 돌려줘야 하는 쪽**이다.
            {
                Game ending(20260826u, Balance::Defaults());
                ending.health = 0;
                ending.Update(step, none);
                if (ending.screen != Screen::Dead) return fail("no health must end on the dead card");

                const std::uint32_t base = ending.seed;
                const std::uint64_t map = ending.grid.StableHash();
                ending.Update(step, sameMap);
                if (ending.screen != Screen::Generating || ending.pendingSeed != base)
                    return fail("R must restart on the base seed");
                ending.Reset(ending.pendingSeed);
                if (ending.grid.StableHash() != map) return fail("R must hand back the same map");
                if (ending.runSeconds != 0.0f || ending.shotsFired != 0 || ending.kills != 0)
                    return fail("a restart must clear the record of the last run");

                ending.health = 0;
                ending.Update(step, none);
                ending.Update(step, newMap);
                if (ending.screen != Screen::Generating || ending.pendingSeed == base)
                    return fail("N must take a seed other than the base one");

                Game escaping(20260826u, Balance::Defaults());
                escaping.hasKey = true;
                escaping.playerPosition = TileCenter(escaping.grid.exit.origin);
                escaping.Update(step, none);
                if (escaping.screen != Screen::Escaped)
                    return fail("reaching the exit with the key must end on the escaped card");
                escaping.Update(step, back);
                if (escaping.screen != Screen::MainMenu)
                    return fail("Esc on a result card must go back to the menu");
            }

            // 결과 카드가 보고하는 기록. **거절된 방아쇠는 사격이 아니다** —
            // 그것을 세면 플레이어에게 나가지도 않은 총알로 빗맞혔다고 말하는 셈이다.
            {
                Game shooter(20260826u, Balance::Defaults());
                shooter.ammo = 0;
                shooter.Shoot();
                if (shooter.shotsFired != 0) return fail("an empty chamber is not a shot");

                // 열쇠로 가는 경로의 다음 타일에 좀비를 놓으면 화면 가운데를 채우므로,
                // 조준선이 반드시 그 위에 있다.
                const std::vector<Int2> route =
                    AStar::FindPath(shooter.grid, shooter.grid.playerSpawn, shooter.grid.keyPosition);
                if (route.size() < 2) return fail("the sample map has no route to the key");
                const Vec2 spot = TileCenter(route[1]);
                const Vec2 towards = spot - shooter.playerPosition;
                shooter.playerAngle = std::atan2(towards.y, towards.x);
                shooter.zombies.clear();
                ZombieAgent target;
                target.position = spot;
                shooter.zombies.push_back(target);

                shooter.ammo = 1;
                shooter.Shoot();
                if (shooter.shotsFired != 1) return fail("a round that leaves must be counted");
                if (shooter.shotsHit != 1 || shooter.kills != 1)
                    return fail("a hit must count as a hit and a kill");
                shooter.Shoot();
                if (shooter.shotsFired != 1) return fail("an empty chamber is still not a shot");
            }

            // 화면마다 뭔가를 말하고, **둘이 같은 말을 하지 않는다.** 이게 없으면
            // 카드 넷이 전부 만들어지면서 전부 비어 있을 수 있다.
            {
                const RunSummary summary{ 93.5f, 4, 2, 2, 20260826u, 20260827u, 1 };
                const Screen cards[] = { Screen::MainMenu, Screen::Generating,
                    Screen::Paused, Screen::Dead, Screen::Escaped };
                const Int2 nowhere{ -1, -1 };
                std::vector<std::string> said;
                for (const Screen card : cards)
                {
                    std::string words;
                    for (const TextSpan& span :
                        BuildScreenLayout(card, summary, 360, 200, 0, nowhere).spans)
                        words += span.text + "|";
                    if (words.empty()) return fail("every screen but Playing must say something");
                    for (const std::string& other : said)
                    {
                        if (other == words) return fail("two screens must not say the same thing");
                    }
                    said.push_back(words);
                }
                if (!BuildScreenLayout(Screen::Playing, summary, 360, 200, 0, nowhere).spans.empty())
                    return fail("Playing must draw no card at all");

                // 3.9 가 요구하는 시드 셋, 그리고 분·초로 적은 시간.
                std::string result;
                for (const TextSpan& span :
                    BuildScreenLayout(Screen::Dead, summary, 360, 200, 0, nowhere).spans)
                    result += span.text + "|";
                if (result.find("20260826") == std::string::npos
                    || result.find("20260827") == std::string::npos
                    || result.find("1:33") == std::string::npos)
                {
                    return fail("the result card must report the time and all three seeds");
                }
            }

            // 포인터. **키를 내주는 모든 화면이 마우스에게 같은 선택지를 내주고**,
            // 포인터 아래 줄이 밝아지며, **밝아진 줄이 곧 클릭이 떨어지는 줄이다.**
            {
                const RunSummary summary{ 93.5f, 4, 2, 2, 20260826u, 20260827u, 1 };
                const Int2 nowhere{ -1, -1 };
                constexpr int cardWidth = 360;
                constexpr int cardHeight = 200;

                const Screen offering[] = { Screen::MainMenu, Screen::Paused,
                    Screen::Dead, Screen::Escaped };
                for (const Screen card : offering)
                {
                    const ScreenLayout resting =
                        BuildScreenLayout(card, summary, cardWidth, cardHeight, 0, nowhere);
                    if (resting.buttons.size() < 2)
                        return fail("a screen that offers keys must offer them to the pointer too");

                    for (std::size_t index = 0; index < resting.buttons.size(); ++index)
                    {
                        const ScreenButton& button = resting.buttons[index];
                        if (button.action == ScreenAction::None)
                            return fail("a choosable line must actually do something");
                        for (std::size_t other = 0; other < index; ++other)
                        {
                            if (resting.buttons[other].action == button.action)
                                return fail("two lines on one screen must not do the same thing");
                        }

                        // 올려놓으면 **정확히 한 줄**이 밝아진다. 나머지가 그대로인지
                        // 확인하는 절반이 **카드 전체에 밝기를 적용한 경우**를 잡는데,
                        // 그건 아무것도 안 골라진 것처럼 보인다.
                        const ScreenLayout lit = BuildScreenLayout(card, summary,
                            cardWidth, cardHeight, 0, button.bounds.Center());
                        if (lit.spans.size() != resting.spans.size())
                            return fail("the pointer must not add or remove lines");
                        int changed = 0;
                        for (std::size_t span = 0; span < lit.spans.size(); ++span)
                        {
                            const Rgb before = resting.spans[span].color;
                            const Rgb after = lit.spans[span].color;
                            if (before.r != after.r || before.g != after.g || before.b != after.b)
                                ++changed;
                            if (lit.spans[span].text != resting.spans[span].text)
                                return fail("the pointer must not change what a line says");
                        }
                        if (changed != 1)
                            return fail("the pointer must light exactly the line it is on");
                    }

                    // 프레임 구석은 어느 줄 위도 아니다.
                    const ScreenLayout cold =
                        BuildScreenLayout(card, summary, cardWidth, cardHeight, 0, Int2{ 1, 1 });
                    for (std::size_t span = 0; span < cold.spans.size(); ++span)
                    {
                        const Rgb before = resting.spans[span].color;
                        const Rgb after = cold.spans[span].color;
                        if (before.r != after.r || before.g != after.g || before.b != after.b)
                            return fail("a pointer on nothing must light nothing");
                    }
                }
            }

            // **모든 카드가, 게임이 실제로 렌더링하는 모든 크기에서** 프레임 밖으로
            // 나가지 않는다.
            //
            // **이건 실제로 망가져 있던 검사다.** 결과 카드가 프레임의 고정 4분의 1
            // 지점에서 배치됐고, 그래서 출하 렌더 천장인 200행에서 마지막 줄 —
            // "Esc 메인 메뉴로", **방금 죽은 플레이어에게 가장 필요한 줄** — 이
            // 바닥에서 8픽셀 넘쳐서 잘렸다.
            //
            // **넘친 글자는 줄어드는 게 아니라 잘리므로** 그리는 쪽은 아무 말도 안 한다.
            {
                const RunSummary summary{ 3599.0f, 12, 9, 9, 3984155984u, 2691032039u, 32 };
                const Int2 nowhere{ -1, -1 };
                // 140행까지 내려간다. 그 아래에서는 **한글이 읽히는 가장 작은 em
                // 에서도** 결과 카드가 안 들어간다 — 제목, 통계 다섯, 선택지 셋이
                // em 7 에서 128행을 요구한다. 그보다 작은 프레임은 이 배치가 지킬 수
                // 없는 약속이다.
                //
                // 게임은 min(열, 360) × min(행 × 2, 200) 으로 렌더링하므로 140 은
                // 70행짜리 터미널이고, 이 카드들이 읽히는 어떤 창보다도 한참 작다.
                const int frames[][2] = {
                    { 360, 200 },   // the shipped render ceiling
                    { 545, 278 },   // a maximised QHD window before the ceiling bites
                    { 240, 160 },
                    { 200, 140 },
                };
                const Screen cards[] = { Screen::MainMenu, Screen::Generating,
                    Screen::Paused, Screen::Dead, Screen::Escaped };
                for (const int (&frame)[2] : frames)
                {
                    for (const Screen card : cards)
                    {
                        for (const TextSpan& span :
                            BuildScreenLayout(card, summary, frame[0], frame[1], 0, nowhere).spans)
                        {
                            const int spanWidth = TextRaster::MeasureWidth(span.text, span.height);
                            const bool off = span.pixel.y < 0
                                || span.pixel.y + span.height + 5 > frame[1]
                                || span.pixel.x < 0
                                || span.pixel.x + spanWidth > frame[0];
                            if (!off) continue;
                            // **어느 카드의, 어느 줄이, 어느 크기에서** 그런지를 말한다.
                            // "카드가 프레임 밖으로 나간다"만 적으면 다음 사람이
                            // 화면 다섯 개와 크기 다섯 개를 손으로 뒤지게 된다.
                            std::cerr << "  offending line: \"" << span.text << "\" at ("
                                << span.pixel.x << ',' << span.pixel.y << ") size "
                                << spanWidth << 'x' << span.height << " on a "
                                << frame[0] << 'x' << frame[1] << " frame, screen "
                                << static_cast<int>(card) << "\n";
                            return fail("a card must stay inside the frame");
                        }
                    }
                }
            }

            // 그리고 **클릭이 정말로 판을 시작한다.** 명중 판정을 직접 부르는 대신
            // 마우스 셀을 담아 Update() 로 몰아넣는다. 그래야 콘솔 마우스 이벤트에서
            // 새 맵까지의 **사슬 전체**가 검사된다.
            {
                Game clicking(20260826u, Balance::Defaults());
                clicking.screen = Screen::MainMenu;
                const ScreenLayout layout = BuildScreenLayout(Screen::MainMenu,
                    clicking.MakeRunSummary(), clicking.renderer.Width(),
                    clicking.renderer.Height(), clicking.balance.uiTextHeight, Int2{ -1, -1 });
                std::size_t start = layout.buttons.size();
                for (std::size_t index = 0; index < layout.buttons.size(); ++index)
                {
                    if (layout.buttons[index].action == ScreenAction::Start) start = index;
                }
                if (start == layout.buttons.size()) return fail("the menu must offer a start");

                // Update() 가 터미널 셀을 프레임 픽셀로 매핑한다. 아무것도 보고되지
                // 않으면 열 하나에 픽셀 하나, 셀 하나에 행 둘로 떨어지는데,
                // 여기서 뒤집어 쓰는 것이 그 매핑이다.
                const Int2 target = layout.buttons[start].bounds.Center();
                InputState click{};
                click.mouseCell = { target.x, (target.y - 1) / 2 };
                click.leftClicked = true;
                clicking.Update(1.0f / 60.0f, click);
                if (clicking.screen != Screen::Generating)
                    return fail("a click on the start line must arm a run");

                Game missing(20260826u, Balance::Defaults());
                missing.screen = Screen::MainMenu;
                InputState astray{};
                astray.mouseCell = { 1, 1 };
                astray.leftClicked = true;
                missing.Update(1.0f / 60.0f, astray);
                if (missing.screen != Screen::MainMenu)
                    return fail("a click on nothing must do nothing");
            }
            return 0;
        }

        // 좀비 스프라이트. 여기서 중요한 불변식은 6.x 의 것이다 —
        // **총알이 그려진 그 마스크에 대고 판정된다.**
        //
        // 전부 공개 렌더러를 통해서만 한다. 그래야 테스트가 구현과 같은 private
        // 도우미를 불러서 **우연히 구현에 동의해버리는 일**이 없다.
        //
        // 전체를 관통하는 수법은 **같은 화면을 두 번 그리는 것**이다 — 배우가 있는
        // 것과 없는 것 — 그리고 다른 픽셀을 취한다. 그게 정확히 좀비의 잉크이고,
        // 그 색이 무엇이든 상관없으며 팔레트를 알 필요가 없다.
        static int ZombieSprites()
        {
            const auto fail = [](const char* what)
            {
                std::cerr << "FAIL zombie sprite test: " << what << "\n";
                return 1;
            };

            constexpr int width = 160;
            constexpr int height = 120;
            MapGenerator generator{ MapGenerationSettings{} };
            const Grid grid = generator.Generate(20260826u);
            const std::vector<Int2> route =
                AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition);
            if (route.size() < 4) return fail("the sample map has no room to stand a zombie up");

            RaycastRenderer renderer(width, height);
            const Vec2 eye = TileCenter(grid.playerSpawn);
            const Vec2 spot = TileCenter(route[2]);
            const Vec2 towards = spot - eye;

            RenderState state;
            state.playerPosition = eye;
            state.playerAngle = std::atan2(towards.y, towards.x);
            // 조준선은 맨 마지막에 그려지고, **좀비가 있는 프레임과 없는 프레임 양쪽에**
            // 그려진다. 그래서 어디에 떨어지든 차이에서 상쇄되고 **실루엣의 구멍처럼
            // 읽힌다.** 구석에 세워두면 화면 가운데의 좀비 근처에 안 떨어진다.
            state.aimPixel = { 3, 3 };
            // 같은 함정의 더 큰 버전 — 무기는 두 렌더링 모두에서 프레임 아래쪽을
            // 덮는다. 120행 중 34행이 지금 기울기가 닿는 높이라, 틈 검사는 그보다
            // 한참 위에서 멈춘다.
            const int weaponTop = height - 45;

            // 이 배우가 책임지는 픽셀들과, 그것들이 사는 상자.
            const std::vector<RenderActor> nobody;
            const PixelBuffer empty = renderer.Render(grid, state, nobody);
            const auto inkOf = [&](const RenderActor& actor)
            {
                const std::vector<RenderActor> one{ actor };
                const PixelBuffer drawn = renderer.Render(grid, state, one);
                std::vector<Int2> ink;
                for (int y = 0; y < height; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        const Rgb before = empty.Get(x, y);
                        const Rgb after = drawn.Get(x, y);
                        if (before.r != after.r || before.g != after.g || before.b != after.b)
                            ink.push_back({ x, y });
                    }
                }
                return ink;
            };

            RenderActor walker;
            walker.position = spot;
            walker.alert = true;
            walker.facing = state.playerAngle + 3.14159265f;   // looking back at the player

            // 같은 배우, 같은 화면, 두 번. **스프라이트 어디든 난수원이 하나 있으면
            // 여기서 드러난다.** 그리고 그런 것이 존재하는 순간 렌더 다이제스트가
            // 오라클이기를 그만둔다.
            {
                const std::vector<RenderActor> one{ walker };
                const PixelBuffer first = renderer.Render(grid, state, one);
                const PixelBuffer second = renderer.Render(grid, state, one);
                if (!(first.Pixels() == second.Pixels()))
                    return fail("the same actor must draw the same pixels every time");
            }

            // **보폭 한 주기 전체를 훑는다.** 마스크가 걸으면서 모양이 바뀌고,
            // 어느 위상에서 열리는 구멍은 그 위상에서 못 쏘는 좀비다.
            // 자세 하나만 봤으면 나머지 넷에 대해 아무것도 증명하지 못했을 것이다.
            for (int phase = 0; phase < 5; ++phase)
            {
                walker.stridePhase = static_cast<float>(phase) * 0.2f;
                const std::vector<RenderActor> one{ walker };
                const std::vector<Int2> ink = inkOf(walker);
                if (ink.size() < 200) return fail("the zombie must actually be on screen");

                int left = width;
                int right = 0;
                int top = height;
                int bottom = 0;
                std::vector<std::uint8_t> painted(
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
                for (const Int2 pixel : ink)
                {
                    left = std::min(left, pixel.x);
                    right = std::max(right, pixel.x);
                    top = std::min(top, pixel.y);
                    bottom = std::max(bottom, pixel.y);
                    painted[static_cast<std::size_t>(pixel.y) * width + pixel.x] = 1;
                }

                // 그려진 것은 전부 쏠 수 있다.
                for (const Int2 pixel : ink)
                {
                    RenderState aimed = state;
                    aimed.aimPixel = pixel;
                    if (renderer.HitTestZombie(grid, aimed, one) != 0)
                        return fail("a pixel the zombie was drawn on must be a hit");
                }

                // 그리고 **구멍은 구멍이다.** 이 절반이 없으면 마스크를 무시하고
                // 상자 전체를 받는 구현도 통과한다. 다리 사이와 머리 옆의 틈이
                // **명중 판정이 눈과 같은 실루엣을 읽고 있다**는 것을 증명한다.
                int holes = 0;
                for (int y = top; y <= std::min(bottom, weaponTop - 1); ++y)
                {
                    for (int x = left; x <= right; ++x)
                    {
                        if (painted[static_cast<std::size_t>(y) * width + x]) continue;
                        ++holes;
                        RenderState aimed = state;
                        aimed.aimPixel = { x, y };
                        if (renderer.HitTestZombie(grid, aimed, one) == 0)
                            return fail("a gap in the silhouette must not be a hit");
                    }
                }
                if (holes < 20) return fail("the silhouette must have gaps to test at all");
            }

            // 바닥의 몸은 그려지고, 아직 서 있는 것과 **다른 모양**이며,
            // 다시 쏠 수 없다.
            {
                walker.stridePhase = 0.0f;
                const std::vector<Int2> standing = inkOf(walker);

                RenderActor corpse = walker;
                corpse.alive = false;
                corpse.deathProgress = 1.0f;
                const std::vector<Int2> fallen = inkOf(corpse);
                if (fallen.empty()) return fail("a dead zombie must still be drawn");
                if (fallen == standing) return fail("a body on the floor must not stand up");

                int lowest = 0;
                for (const Int2 pixel : standing) lowest = std::max(lowest, pixel.y);
                int highest = height;
                for (const Int2 pixel : fallen) highest = std::min(highest, pixel.y);
                if (highest <= lowest - static_cast<int>(standing.size() / 8))
                    return fail("a body must fall towards the floor");

                const std::vector<RenderActor> one{ corpse };
                for (const Int2 pixel : fallen)
                {
                    RenderState aimed = state;
                    aimed.aimPixel = pixel;
                    if (renderer.HitTestZombie(grid, aimed, one) >= 0)
                        return fail("a body on the floor must not be a target");
                }
            }

            // 눈은 머리 앞면에 있다. **배우 자신의 잉크 안에서만** 세므로, 화면의
            // 다른 붉은 것이 대신 답할 수 없다.
            {
                const auto redPixels = [&](const RenderActor& actor)
                {
                    const std::vector<RenderActor> one{ actor };
                    const PixelBuffer drawn = renderer.Render(grid, state, one);
                    int reds = 0;
                    for (const Int2 pixel : inkOf(actor))
                    {
                        const Rgb colour = drawn.Get(pixel.x, pixel.y);
                        if (colour.r > colour.g * 2 && colour.r > colour.b * 2) ++reds;
                    }
                    return reds;
                };

                RenderActor facingUs = walker;
                facingUs.facing = state.playerAngle + 3.14159265f;
                if (redPixels(facingUs) <= 0)
                    return fail("an alert zombie looking at the player must show its eyes");

                RenderActor facingAway = walker;
                facingAway.facing = state.playerAngle;
                if (redPixels(facingAway) != 0)
                    return fail("a zombie facing away must not show eyes through its head");

                RenderActor calm = facingUs;
                calm.alert = false;
                if (redPixels(calm) != 0)
                    return fail("a zombie that has not noticed the player must not glow");
            }

            // 걸음걸이는 시계가 아니라 **걸은 거리**에서 나온다. 출하 속도로 스무
            // 걸음이면 보폭 한 주기가 안 되므로 위상이 안 감기고, 두 숫자를 그냥
            // 비교할 수 있다.
            {
                Game walkTest(20260826u, Balance::Defaults());
                std::size_t chosen = walkTest.zombies.size();
                for (std::size_t index = 0; index < walkTest.zombies.size(); ++index)
                {
                    if (walkTest.zombies[index].waitingBehindBreach) continue;
                    chosen = index;
                    break;
                }
                if (chosen == walkTest.zombies.size())
                    return fail("the sample map has no loose zombie to walk");

                walkTest.zombies[chosen].alert = true;
                walkTest.zombies[chosen].repathTimer = 0.0f;
                float travelled = 0.0f;
                InputState still{};
                for (int step = 0; step < 20; ++step)
                {
                    const Vec2 before = walkTest.zombies[chosen].position;
                    walkTest.Update(1.0f / 60.0f, still);
                    travelled += Length(walkTest.zombies[chosen].position - before);
                }
                if (travelled <= 0.0f) return fail("the chosen zombie never moved");
                const float expected = travelled / walkTest.balance.zombieStrideTiles;
                if (std::abs(walkTest.zombies[chosen].stridePhase - expected) > 1.0e-3f)
                    return fail("the stride must follow the distance walked, not the clock");

                // 그리고 아무것도 못 알아챈 좀비는 발을 가만히 둔다.
                std::size_t asleep = walkTest.zombies.size();
                for (std::size_t index = 0; index < walkTest.zombies.size(); ++index)
                {
                    if (index != chosen && !walkTest.zombies[index].alert)
                    {
                        asleep = index;
                        break;
                    }
                }
                if (asleep < walkTest.zombies.size()
                    && walkTest.zombies[asleep].stridePhase != 0.0f)
                {
                    return fail("a zombie that has not moved must not be mid stride");
                }
            }
            return 0;
        }

        // 같은 플레이어 타일을 쫓아도 살아 있는 좀비 몸은 한 점으로 합쳐지지 않는다.
        // A* 를 동적 점유로 바꾸지 않았으므로, 열린 직선에서 실제 추적을 오래 돌려
        // 선두가 멈춘 뒤 뒤따르던 몸들이 줄을 이루는 순간까지 본다.
        static int ZombieSeparation()
        {
            const auto prepare = [](Game& game)
            {
                game.grid = Grid(12, 7);
                for (int y = 1; y < game.grid.Height() - 1; ++y)
                {
                    for (int x = 1; x < game.grid.Width() - 1; ++x)
                        game.grid.Set({ x, y }, Tile::Floor);
                }
                game.playerPosition = { 10.5f, 3.5f };
                game.developerDebug = true;
                game.zombies.clear();
                for (int index = 0; index < 5; ++index)
                {
                    ZombieAgent zombie;
                    zombie.position = { 1.5f + static_cast<float>(index), 3.5f };
                    zombie.alert = true;
                    zombie.growlTimer = 1000.0f;
                    game.zombies.push_back(zombie);
                }
            };

            Game crowd(20260826u, Balance::Defaults());
            Game repeated(20260826u, Balance::Defaults());
            prepare(crowd);
            prepare(repeated);
            const float minimumDistance = Game::ZombieCollisionRadius * 2.0f;
            for (int step = 0; step < 360; ++step)
            {
                crowd.UpdateZombies(1.0f / 60.0f);
                repeated.UpdateZombies(1.0f / 60.0f);
                for (std::size_t first = 0; first < crowd.zombies.size(); ++first)
                {
                    if (!crowd.CanZombieOccupy(crowd.zombies[first].position))
                    {
                        std::cerr << "FAIL zombie separation test: avoidance pushed a zombie into a wall\n";
                        return 1;
                    }
                    const Vec2 repeatDelta = crowd.zombies[first].position
                        - repeated.zombies[first].position;
                    if (Length(repeatDelta) > 0.00001f)
                    {
                        std::cerr << "FAIL zombie separation test: avoidance must be deterministic\n";
                        return 1;
                    }
                    for (std::size_t second = first + 1; second < crowd.zombies.size(); ++second)
                    {
                        if (Length(crowd.zombies[second].position - crowd.zombies[first].position)
                            < minimumDistance - 0.002f)
                        {
                            std::cerr << "FAIL zombie separation test: pursuing zombies overlapped\n";
                            return 1;
                        }
                    }
                }
            }

            // 이미 완전히 겹친 상태도 복구한다. 차이가 0인 쌍은 정규화만으로는
            // 어느 방향으로도 밀 수 없어 별도 결정론적 축이 필요하다.
            crowd.zombies.resize(2);
            crowd.zombies[0].position = { 5.5f, 3.5f };
            crowd.zombies[1].position = crowd.zombies[0].position;
            crowd.SeparateZombies();
            if (Length(crowd.zombies[1].position - crowd.zombies[0].position)
                < minimumDistance - 0.002f)
            {
                std::cerr << "FAIL zombie separation test: an exact overlap was not recovered\n";
                return 1;
            }

            // 알아채지 못한 좀비가 추적자에게 밀려 조용히 미끄러지면 안 된다.
            crowd.zombies[0].position = { 5.5f, 3.5f };
            crowd.zombies[0].alert = true;
            crowd.zombies[1].position = { 5.7f, 3.5f };
            crowd.zombies[1].alert = false;
            const Vec2 sleepingPosition = crowd.zombies[1].position;
            crowd.SeparateZombies();
            if (Length(crowd.zombies[1].position - sleepingPosition) > 0.00001f
                || Length(crowd.zombies[1].position - crowd.zombies[0].position)
                    < minimumDistance - 0.002f)
            {
                std::cerr << "FAIL zombie separation test: a sleeping zombie must stay planted\n";
                return 1;
            }
            return 0;
        }

        // 막힌 문. **옆의 문과 같은 문으로 읽혀야 한다** — 같은 목재, 같은 경고색,
        // 같은 균열색 — 그 위에 창살만 얹은 것으로. 어렵게 알아내는 두 방법이 둘 다
        // 플레이어에게 값을 물리기 때문이다. E 를 누르는 것은 그 순간을 쓰고,
        // 한 발은 맵 탄약의 6분의 1을 쓴다.
        //
        // 생성된 맵이 아니라 **여기서 만든 방** 위에 세운다. 막힌 문은 감염 공간의
        // 벽에 있고 A* 는 정의상 거기 닿을 수 없으므로, 진짜 맵에서 그것을 볼 수 있는
        // 시점을 찾는 것은 탐색이 된다. 먼 벽에 문 하나가 있는 9타일 방은 같은 그림이면서
        // 찾을 것이 없다.
        static int SealedDoors()
        {
            const auto fail = [](const char* what)
            {
                std::cerr << "FAIL sealed door test: " << what << "\n";
                return 1;
            };

            constexpr int width = 160;
            constexpr int height = 120;
            Grid room(9, 9);
            for (int y = 1; y <= 7; ++y)
            {
                for (int x = 1; x <= 7; ++x) room.Set({ x, y }, Tile::Floor);
            }
            const Int2 doorTile{ 4, 0 };

            RaycastRenderer renderer(width, height);
            RenderState state;
            state.playerPosition = TileCenter({ 4, 4 });
            state.playerAngle = -1.57079633f;      // straight at the far wall
            state.aimPixel = { 3, 3 };             // crosshair out of the way
            const std::vector<RenderActor> nobody;

            const auto frameWith = [&](Tile tile)
            {
                room.Set(doorTile, tile);
                return renderer.Render(room, state, nobody);
            };

            // 화면 가운데를 가로지르는 띠 — 천장 아래, 무기 위, 구석의 게이지에서 비켜.
            constexpr int bandTop = height / 2 - 6;
            constexpr int bandBottom = height / 2 + 6;

            const PixelBuffer blank = frameWith(Tile::Wall);

            const std::pair<Tile, Tile> pairs[] = {
                { Tile::DoorClosed, Tile::DoorSealed },
                { Tile::DoorWarning, Tile::DoorSealedWarning },
                { Tile::DoorCracked, Tile::DoorSealedCracked },
            };

            for (const auto& [plain, sealed] : pairs)
            {
                const PixelBuffer plainFrame = frameWith(plain);
                const PixelBuffer sealedFrame = frameWith(sealed);

                int onDoor = 0;
                int shared = 0;
                int barred = 0;
                for (int y = bandTop; y < bandBottom; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        // **문 자신의 픽셀** — 평범한 벽이 이렇게 칠하지 않는 것들.
                        // 화면의 나머지는 세 프레임에서 전부 같고 세는 값을 묽게
                        // 만들 뿐이다.
                        const Rgb wall = blank.Get(x, y);
                        const Rgb open = plainFrame.Get(x, y);
                        if (wall.r == open.r && wall.g == open.g && wall.b == open.b) continue;
                        ++onDoor;

                        const Rgb shut = sealedFrame.Get(x, y);
                        if (open.r == shut.r && open.g == shut.g && open.b == shut.b) ++shared;
                        else ++barred;
                    }
                }

                if (onDoor < 40) return fail("the door must actually be in view");
                // **양쪽 절반 모두, 그리고 둘 다 중요하다.** 창살만 있으면 더 이상
                // 문처럼 안 보이는 문이고, 목재만 있으면 창살이 아예 없는 것인데
                // 그게 이 변경이 대체한 상태다.
                if (shared == 0) return fail("a sealed door must keep the timber of its state");
                if (barred == 0) return fail("a sealed door must carry bars");
            }
            return 0;
        }

        // 깨어남. 예전에는 울음소리가 **소음 경로 하나에만** 달려 있어서, 그냥
        // 플레이어를 본 좀비는 조용히 왔다. 그리고 한 번 깬 뒤로는 다시 울지 않았다.
        //
        // 둘 다 여기서 확인하고, **둘 다 소리가 아니라 타이머로** 확인한다. 자체
        // 테스트는 오디오 장치 없이 돌고 아무 데도 안 가는 큐는 아무것도 증명하지
        // 않기 때문이다. **타이머는 상태이고, 상태는 보인다.**
        static int ZombieWaking()
        {
            constexpr float step = 1.0f / 60.0f;
            const auto fail = [](const char* what)
            {
                std::cerr << "FAIL zombie waking test: " << what << "\n";
                return 1;
            };

            // 플레이어를 보면 깨어나고, **WakeZombie 를 거치는 것이** 울음을 장전한다.
            // 플래그를 손으로 세우는 경로는 이 값을 0으로 남긴다.
            {
                Game seen(20260826u, Balance::Defaults());
                std::size_t loose = seen.zombies.size();
                for (std::size_t index = 0; index < seen.zombies.size(); ++index)
                {
                    if (!seen.zombies[index].waitingBehindBreach) { loose = index; break; }
                }
                if (loose == seen.zombies.size()) return fail("the sample map has no loose zombie");

                ZombieAgent& target = seen.zombies[loose];
                if (target.alert) return fail("the chosen zombie starts awake");
                // 플레이어 바로 위, 인지 범위 한참 안쪽.
                target.position = seen.playerPosition;
                seen.Update(step, InputState{});
                if (!target.alert) return fail("a zombie that can see the player must wake");
                if (target.growlTimer <= 0.0f)
                    return fail("waking must arm the growl, whichever way it woke");
                if (target.growlTimer > seen.balance.zombieGrowlInterval * 2.0f)
                    return fail("the first repeat must land within two intervals");
            }

            // 그리고 **계속 운다.** 타이머가 0까지 가서 머무는 것이 아니라 다시
            // 돌아와야 한다. 조용해진 좀비는 관심을 잃은 것으로 읽히는데,
            // 이 좀비는 절대 관심을 잃지 않는다.
            {
                Game waiting(20260826u, Balance::Defaults());
                std::size_t loose = waiting.zombies.size();
                for (std::size_t index = 0; index < waiting.zombies.size(); ++index)
                {
                    if (!waiting.zombies[index].waitingBehindBreach) { loose = index; break; }
                }
                if (loose == waiting.zombies.size()) return fail("the sample map has no loose zombie");

                waiting.WakeZombie(waiting.zombies[loose]);
                const float armed = waiting.zombies[loose].growlTimer;
                if (armed <= 0.0f) return fail("waking must arm the growl");

                int reloads = 0;
                float previous = armed;
                const float interval = waiting.balance.zombieGrowlInterval;
                for (int tick = 0; tick < 600; ++tick)      // ten seconds
                {
                    waiting.Update(step, InputState{});
                    const float now = waiting.zombies[loose].growlTimer;
                    if (!waiting.zombies[loose].alive) break;
                    if (now > previous) ++reloads;          // it fired and rearmed
                    previous = now;
                }
                if (reloads < 2)
                    return fail("an alert zombie must keep growling, not fall silent");
                if (previous > interval)
                    return fail("the timer must never be armed beyond one interval on a repeat");
            }

            // 같은 사건에 깨어난 좀비 둘이 **발을 맞추면 안 된다.** 맞으면 맵이
            // 메트로놈이 된다. 오프셋은 타일에서 읽으므로 같은 시드의 모든 실행에서
            // 같다.
            {
                Game chorus(20260826u, Balance::Defaults());
                std::vector<float> armed;
                for (ZombieAgent& zombie : chorus.zombies)
                {
                    if (zombie.waitingBehindBreach) continue;
                    chorus.WakeZombie(zombie);
                    armed.push_back(zombie.growlTimer);
                }
                if (armed.size() < 2) return fail("the sample map has too few loose zombies");
                bool differs = false;
                for (std::size_t index = 1; index < armed.size(); ++index)
                {
                    if (armed[index] != armed[0]) differs = true;
                }
                if (!differs) return fail("zombies woken together must not growl in unison");
            }
            return 0;
        }

        // 생성기는 MapGenerationSettings{} — 자기 구조체 기본값 — 에 대고 검증된다.
        // **게임은 그것을 안 쓴다.** Game 은 밸런스 파일에서 설정을 만든다.
        //
        // 그래서 튜닝된 ini 하나가 출하되는 게임을 **아무도 검사한 적 없는 생성기
        // 설정** 위로 조용히 옮겨놓을 수 있고, 그러는 동안 위의 64시드 훑기는 계속
        // 통과한다.
        //
        // **2026-08-29 에 그것이 가정이기를 그만뒀다** — ini 의 extra_ammo_rounds 가
        // 3에서 5가 됐고, 다섯 발은 필수 동선이 지나지 않는 방들에 놓여야 한다.
        // 그건 생성에 대한 진짜 제약인데 아무도 그것을 재고 있지 않았다.
        //
        // 64개가 아니라 8개 시드다. **두 번째 전체 훑기가 아니라 출하 숫자에 대한
        // 연기 테스트**다 — 규칙은 이미 덮여 있고, 여기서 묻는 것은 오직
        // "플레이어가 실제로 노는 설정이 아직 맵을 만들 수 있는가"뿐이다.
        static int ShippedConfigGenerates(const Balance& balance)
        {
            MapGenerationSettings settings;
            settings.minimumKeyPathCost = balance.minimumKeyPathCost;
            settings.zombieCount = balance.zombieCount;
            settings.extraAmmoRounds = balance.extraAmmoRounds;
            settings.startAmmo = balance.startAmmo;
            const MapGenerator generator(settings);

            for (std::uint32_t seed = 1; seed <= 8; ++seed)
            {
                const Grid grid = generator.Generate(seed);
                const std::string reason = MapGenerator::Validate(grid, settings);
                if (!reason.empty())
                {
                    std::cerr << "FAIL shipped config test: seed " << seed
                        << " does not generate with the numbers the game plays: "
                        << reason << "\n"
                        << "  extra_ammo_rounds " << settings.extraAmmoRounds
                        << ", zombie_count " << settings.zombieCount
                        << ", minimum_key_path_cost " << settings.minimumKeyPathCost << "\n"
                        << "  Config/GameBalance.ini is what the game runs on. The 64 seed"
                        " sweep uses the generator's own defaults and cannot see this.\n";
                    return 1;
                }
                // 탄약 예산이 **줍는 것을 선택으로 만드는 규칙**이고, 이 개수가
                // 직접 누르는 것이 그 규칙이다.
                int rounds = 0;
                for (const AmmoPickup& pickup : grid.ammoPickups) rounds += pickup.rounds;
                if (rounds != settings.extraAmmoRounds)
                {
                    std::cerr << "FAIL shipped config test: seed " << seed << " placed "
                        << rounds << " extra rounds, config asks for "
                        << settings.extraAmmoRounds << "\n";
                    return 1;
                }
                if (seed == 1)
                {
                    Game game(seed, balance);
                    int gameRounds = 0;
                    for (const AmmoPickup& pickup : game.grid.ammoPickups)
                        gameRounds += pickup.rounds;
                    if (game.grid.seed != grid.seed
                        || game.grid.seedAttempt != grid.seedAttempt
                        || game.grid.StableHash() != grid.StableHash()
                        || game.grid.zombieSpawns.size() != grid.zombieSpawns.size()
                        || game.zombies.size() != grid.zombieSpawns.size()
                        || gameRounds != rounds || game.ammo != balance.startAmmo)
                    {
                        std::cerr << "FAIL shipped config test: Game and the direct generator"
                            " must receive the same normalized map settings\n";
                        return 1;
                    }
                }
            }
            return 0;
        }

        // 설정을 **설정으로서** 판정한다. 셋인데, **하나하나가 2026-08-29 에 실제로
        // 잘못됐던 것**이다.
        static int BalanceSanity(const Balance& shipped)
        {
            const auto fail = [](const std::string& what)
            {
                std::cerr << "FAIL balance sanity test: " << what << "\n";
                return 1;
            };

            // 1. **게임이 실제로 돌 설정이 놀 수 있어야 한다.** 잘못된 조합이 예전에는
            //    "map generation gave up after 100 attempts on seed 1" 로 발견됐는데,
            //    그건 원인에서 세 단계 떨어진 증상의 이름이고 읽는 사람을 엉뚱한
            //    파일로 보냈다.
            if (!shipped.problems.empty())
            {
                std::string all;
                for (const std::string& problem : shipped.problems) all += "\n    " + problem;
                return fail("the shipped config is not playable:" + all
                    + "\n  Config/GameBalance.ini is what the game runs on.");
            }

            // 2. **내장 기본값도 설정이다.** 파일이 없으면 게임이 그리로 떨어지므로,
            //    그것도 자기 규칙을 통과해야 한다.
            {
                Balance defaults = Balance::Defaults();
                defaults.Validate();
                if (!defaults.problems.empty())
                    return fail("the built-in defaults are not playable on their own");
            }

            // 3. **규칙이 물어야 한다.** 한 번도 안 걸리는 규칙은 규칙이 없는 것과
            //    구별할 수 없고, 이 규칙이 지키는 것은 맵 생성을 완전히 멈춘 바로
            //    그 조합이다.
            {
                Balance clearable = Balance::Defaults();
                clearable.zombieCount = 12;
                clearable.startAmmo = 6;
                clearable.extraAmmoRounds = 6;   // twelve rounds against twelve zombies
                clearable.Validate();
                if (clearable.problems.empty())
                {
                    return fail("a config that can kill every zombie must be refused:"
                        " 6.10 says clearing the map is never on the table");
                }
            }

            // NaN/무한대는 비교식에서 조용히 빠져나갈 수 있으므로 키 이름으로
            // 직접 거절되어야 한다.
            {
                Balance nonFinite = Balance::Defaults();
                nonFinite.playerWalkSpeed = std::numeric_limits<float>::quiet_NaN();
                nonFinite.Validate();
                const bool named = std::any_of(nonFinite.problems.begin(), nonFinite.problems.end(),
                    [](const std::string& problem)
                    {
                        return problem.find("player_walk_speed") != std::string::npos;
                    });
                if (!named) return fail("a non-finite tuning value must be refused by key");
            }

            // 4. **구워 넣은 사본이 출하 파일이어야 한다.** 2026-08-30 부터 내장
            //    기본값은 빌드 때 Config/GameBalance.ini 에서 생성되므로, 손으로 적은
            //    목록 둘이 어긋나던 방식으로는 어긋날 수 없다 — **다만 빌드가 정말로
            //    다시 굽는 동안만 그렇다.** 낡은 굽기는 옛 문제와 똑같아 보일 것이고,
            //    그것을 알아채는 것이 이 검사다.
            {
                const Balance baked = Balance::Defaults();
                const std::vector<Balance::Difference> apart = baked.DifferencesFrom(shipped);
                if (!apart.empty())
                {
                    std::cerr << "FAIL balance sanity test: the baked defaults are not the"
                        " shipped config\n";
                    for (const Balance::Difference& change : apart)
                    {
                        std::cerr << "    " << change.key << "  baked " << change.here
                            << "  file " << change.there << "\n";
                    }
                    std::cerr << "  Config/GameBalance.ini is baked into the binary by the"
                        " BakeBalanceDefaults target. If these differ the bake did not run.\n";
                    return 1;
                }
            }

            // 5. **기본값 두 벌이 일치해야 한다.** 생성기는 자기 MapGenerationSettings
            //    기본값을 들고 있고 게임은 밸런스에서 설정을 만든다. 둘이 어긋났을 때
            //    64시드 훑기는 계속 통과했고, 그동안 출하되는 게임은 아무도 검사한 적
            //    없는 것을 만들고 있었다.
            {
                const Balance defaults = Balance::Defaults();
                const MapGenerationSettings generatorDefaults;
                if (defaults.extraAmmoRounds != generatorDefaults.extraAmmoRounds
                    || defaults.zombieCount != generatorDefaults.zombieCount
                    || defaults.startAmmo != generatorDefaults.startAmmo)
                {
                    std::cerr << "FAIL balance sanity test: the built-in balance and the"
                        " generator's own defaults disagree\n"
                        << "    extra_ammo_rounds " << defaults.extraAmmoRounds
                        << " vs " << generatorDefaults.extraAmmoRounds << "\n"
                        << "    zombie_count " << defaults.zombieCount
                        << " vs " << generatorDefaults.zombieCount << "\n"
                        << "    start_ammo " << defaults.startAmmo
                        << " vs " << generatorDefaults.startAmmo << "\n"
                        << "  The 64 seed sweep runs on the generator's defaults and the"
                        " game runs on the balance. Drift here means the sweep is"
                        " verifying a game nobody plays.\n";
                    return 1;
                }
            }
            return 0;
        }

        // 판 도중에 밸런스를 다시 읽기. 이 기능의 요점 전체가 여기라, Load 에 구조체를
        // 건네는 대신 **디스크의 진짜 파일**을 통해 검사한다.
        // 이 기능이 없애려는 실패가 **읽힌 파일과 편집된 파일이 서로 다른 파일**이었던
        // 것이기 때문이다.
        static int BalanceReload()
        {
            const auto fail = [](const char* what)
            {
                std::cerr << "FAIL balance reload test: " << what << "\n";
                return 1;
            };

            std::error_code error;
            const std::filesystem::path root =
                std::filesystem::temp_directory_path(error) / "ConsoleZombieReloadTest";
            std::filesystem::remove_all(root, error);
            // 실제와 같은 배치 — 빌드가 실행 파일 옆에 사본을 떨구고 저장소가 원본을
            // 한 폴더 위에 갖고 있다. **둘이 동시에 존재하고, 예전에는 가까운 쪽이
            // 이겼다.**
            std::filesystem::create_directories(root / "Bin" / "Config", error);
            std::filesystem::create_directories(root / "Config", error);
            if (error) return fail("could not make a scratch config folder");
            const std::filesystem::path nearPath = root / "Bin" / "Config" / "GameBalance.ini";
            const std::filesystem::path farPath = root / "Config" / "GameBalance.ini";
            const std::filesystem::path fakeExecutable = root / "Bin" / "ConsoleZombie.exe";

            // 맵 생성 제한은 Load()에서 끝난다. 게임과 출하 설정 검사가 각자 다시
            // 자르지 않아도 같은 값을 받아야 한다.
            {
                const std::filesystem::path limitsPath = root / "limits.ini";
                std::ofstream file(limitsPath, std::ios::trunc);
                file << "zombie_count = 99\nextra_ammo_rounds = 1\n";
                file.close();
                const Balance limited = Balance::Load(limitsPath);
                if (limited.zombieCount != 14 || limited.extraAmmoRounds != 2
                    || !limited.problems.empty())
                {
                    return fail("map generation limits must be applied once by Balance::Load");
                }
                std::ofstream lowerFile(limitsPath, std::ios::trunc);
                lowerFile << "zombie_count = -99\n";
                lowerFile.close();
                const Balance lowerLimited = Balance::Load(limitsPath);
                if (lowerLimited.zombieCount != 10 || !lowerLimited.problems.empty())
                {
                    return fail("the lower zombie limit must also be applied by Balance::Load");
                }
            }

            // 숫자가 아닌 값은 기본값으로 조용히 돌아가지 않고, nan/inf는 유한성
            // 검사에서 빠져나가지 않는다. 시작 경계는 이 problems 전체를 거절한다.
            {
                const std::filesystem::path invalidPath = root / "invalid.ini";
                std::ofstream file(invalidPath, std::ios::trunc);
                file << "player_walk_speed = not-a-number\n"
                    << "player_rotation_speed = nan\n"
                    << "stamina_max = inf\n";
                file.close();
                const Balance invalid = Balance::Load(invalidPath);
                for (const std::string key : {
                    "player_walk_speed", "player_rotation_speed", "stamina_max" })
                {
                    const bool named = std::any_of(invalid.problems.begin(), invalid.problems.end(),
                        [&key](const std::string& problem)
                        {
                            return problem.find(key) != std::string::npos;
                        });
                    if (!named) return fail("invalid numeric values must be reported by key");
                }
                bool gameRefused = false;
                try
                {
                    Game invalidGame(20260826u, invalid);
                }
                catch (const std::invalid_argument&)
                {
                    gameRefused = true;
                }
                if (!gameRefused)
                    return fail("Game itself must refuse a balance that has reported problems");
            }

            // 유한하지만 의미가 뒤집히는 음수도 시작 전에 키별로 거절한다.
            {
                const std::filesystem::path unsafePath = root / "unsafe.ini";
                std::ofstream file(unsafePath, std::ios::trunc);
                file << "player_walk_speed = -2\n"
                    << "zombie_attack_interval = -1\n"
                    << "stamina_roll_cost = -1\n"
                    << "breach_zombie_pressure_per_second = -3\n";
                file.close();
                const Balance unsafe = Balance::Load(unsafePath);
                for (const std::string key : { "player_walk_speed", "zombie_attack_interval",
                    "stamina_roll_cost", "breach_zombie_pressure_per_second" })
                {
                    const bool named = std::any_of(unsafe.problems.begin(), unsafe.problems.end(),
                        [&key](const std::string& problem)
                        {
                            return problem.find(key) != std::string::npos;
                        });
                    if (!named) return fail("unsafe negative values must be reported by key");
                }
            }

            // 먼 쪽 사본 — 저장소 원본, 사람이 편집하는 것 — 을 쓰고 가까운 것보다
            // **더 새것으로 도장을 찍는다.** 그래야 검사되는 것이 "가장 먼 것이 이긴다"가
            // 아니라 **"가장 최근 것이 이긴다"** 가 된다.
            const auto write = [&](const std::string& body)
            {
                {
                    std::ofstream file(farPath, std::ios::trunc);
                    file << body;
                }
                std::error_code stampError;
                const auto nearWritten = std::filesystem::last_write_time(nearPath, stampError);
                if (!stampError)
                {
                    std::filesystem::last_write_time(farPath,
                        nearWritten + std::chrono::seconds(10), stampError);
                }
            };

            // 실행 파일 옆의 낡은 사본. 빌드가 남겨놓는 그대로다.
            {
                std::ofstream file(nearPath, std::ios::trunc);
                file << "player_walk_speed = 2.0\nextra_ammo_rounds = 5\n";
            }
            Game game(20260826u, Balance::Load(nearPath), false, fakeExecutable);
            if (game.balance.playerWalkSpeed != 2.0f) return fail("the scratch config was not read");

            // 사람이 하는 편집. 더 멀고 더 새것인 저장소 사본에서 일어난다.
            // **여기서 가까운 것을 집는 것이 원래의 버그다** — 읽힌 파일과 편집된
            // 파일이 서로 다른 파일이었다.
            write("player_walk_speed = 3.5\nextra_ammo_rounds = 5\n");
            game.ReloadBalance();
            if (game.balance.playerWalkSpeed != 3.5f)
            {
                return fail("a reread must take the newest copy, not the nearest:"
                    " the nearest is the one the build wrote, which is older than the edit");
            }

            // **그리고 프로세스가 시작할 때도 같은 파일을 열어야 한다.**
            //
            // 정확히 이 검사가 없어서 고장이 하나 살아남았다. F5 는 가장 최근 것을
            // 고르게 고쳤는데 **시작 경로는 여전히 가장 가까운 것을 골랐다.** 그래서
            // ini 를 고치고 빌드 없이 실행하면 F5 를 누르기 전까지 고치기 전의 게임이
            // 돌았고, 바깥에서 보이는 것은 "`render_fps = 60` 으로 바꿨는데 안 먹네"
            // 하나였다.
            //
            // 이 검사가 실제 파일 배치가 아니라 이 임시 폴더 위에 서 있는 것이
            // 중요하다. 실행 중인 저장소에서는 방금 빌드가 exe 옆 사본을 새로 썼기
            // 때문에 **가장 가까운 것과 가장 최근인 것이 같은 파일**이고, 그 상태에서
            // 하는 비교는 버그를 통과시킨다.
            {
                std::error_code pickError;
                const std::filesystem::path picked = Balance::FindConfigFile(fakeExecutable);
                if (picked.empty() || !std::filesystem::equivalent(picked, farPath, pickError)
                    || Balance::Load(picked).playerWalkSpeed != 3.5f)
                {
                    return fail("startup must open the same copy a reread would — the newest,"
                        " not the one sitting beside the executable");
                }
            }

            // 적용이 아니라 **거절**이다. 좀비 열둘에 탄약 열둘은 플레이어가 맵을
            // 청소할 수 있게 하는 설정이고 6.10 이 그것을 금지한다.
            // 이 검사 전에는 그런 설정이 받아들여진 다음 **다음 맵 생성 백 번 실패**로
            // 발견됐을 것이다.
            write("player_walk_speed = 9.0\nstart_ammo = 6\nextra_ammo_rounds = 6\n");
            game.ReloadBalance();
            if (game.balance.playerWalkSpeed == 9.0f)
                return fail("a config with problems must be refused whole, not applied in part");

            // 그리고 **거절은 거절이라고 말해야 한다.** 조용한 거절은 조용한 수용과
            // 같은 경험이다 — 숫자는 바뀌었는데 게임은 안 바뀌었다.
            bool said = false;
            for (const StatusLog::Line& line : game.statusLog.Lines())
            {
                if (line.text.find("적용하지 않았다") != std::string::npos) said = true;
            }
            if (!said) return fail("a refused reread must say why on screen");

            // 생성 값이 움직였다 — 적용되지만 **"다음 판부터"라고 이름을 대며** 알린다.
            // 여기서 아무 말도 안 하는 것이 튜닝하는 사람이 "게임이 무시했다"고
            // 결론짓게 만드는 길이다.
            write("player_walk_speed = 3.5\nextra_ammo_rounds = 4\n");
            game.statusLog.Clear();
            game.ReloadBalance();
            if (game.balance.extraAmmoRounds != 4) return fail("a generation value must still be taken");
            bool deferred = false;
            for (const StatusLog::Line& line : game.statusLog.Lines())
            {
                if (line.text.find("다음 판부터") != std::string::npos) deferred = true;
            }
            if (!deferred)
                return fail("a value that cannot reach the map already built must be named as such");

            // 그리고 **"다음 판부터"가 참이어야 한다.** 생성기는 만들어질 때 받은
            // 설정을 들고 있으므로, 다시 읽으면서 생성기를 다시 만들지 않으면
            // **오지 않는 변화를 약속하는 것**이 된다. 이 기능이 없애려던 바로 그
            // 침묵이고, 다만 한 판 늦게 온다.
            game.Reset(game.seed);
            int placed = 0;
            for (const AmmoPickup& pickup : game.grid.ammoPickups) placed += pickup.rounds;
            if (placed != 4)
            {
                std::cerr << "FAIL balance reload test: the next map was built with "
                    << placed << " extra rounds, the reread asked for 4\n"
                    << "  The generator keeps the settings it was constructed with.\n";
                return 1;
            }

            std::filesystem::remove_all(root, error);
            return 0;
        }

        // 상태 로그의 노화·반복 거부와, 침입 사건이 거기까지 닿는가.
        static int StatusLog()
        {
        // 상태 로그. 발소리가 초당 두 번 나므로 쿨다운이 없으면 화면 전체가
        // 한 문장의 반복이 된다.
        {
            Balance logBalance = Balance::Defaults();
            logBalance.uiLogSeconds = 2.0f;
            logBalance.uiLogCooldown = 1.0f;
            Game talker(20260826u, logBalance);
            talker.statusLog.Clear();

            talker.PushLog("창문이 깨졌다");
            talker.PushLog("창문이 깨졌다");
            if (talker.statusLog.Lines().size() != 1)
            {
                std::cerr << "FAIL log test: the same line must not stack while its cooldown runs\n";
                return 1;
            }
            talker.PushLog("문이 부숴졌다");
            talker.PushLog("좀비가 그르렁거리는 소리가 들린다");
            talker.PushLog("탄환을 주웠다");
            if (talker.statusLog.Lines().size() != 3 || talker.statusLog.Lines().front().text != "문이 부숴졌다")
            {
                std::cerr << "FAIL log test: the log must hold the three newest lines\n";
                return 1;
            }
            for (int step = 0; step < 180; ++step) talker.UpdateLog(1.0f / 60.0f);
            if (!talker.statusLog.Lines().empty())
            {
                std::cerr << "FAIL log test: a line must age out on its own\n";
                return 1;
            }
            // 줄과 함께 쿨다운도 만료됐으므로 같은 문장을 다시 말할 수 있다.
            talker.PushLog("창문이 깨졌다");
            if (talker.statusLog.Lines().size() != 1)
            {
                std::cerr << "FAIL log test: a line must be sayable again once its cooldown lapses\n";
                return 1;
            }
        }

        // 침입구의 상태 변화가 **바깥에서 보여야 한다.** 아니면 로그가 금이 간 것과
        // 무너진 것을 구별할 수 없다.
        {
            Grid eventGrid(5, 3);
            eventGrid.Set({ 1, 1 }, Tile::Floor);
            eventGrid.Set({ 2, 1 }, Tile::WindowIntact);
            eventGrid.Set({ 3, 1 }, Tile::Floor);
            eventGrid.breachSpawns.push_back({ { 2, 1 }, { 3, 1 } });
            BreachSystem reporter(BreachTuning{ 10.0f, 20.0f, 30.0f, 0.0f, 0.0f });
            reporter.Initialize(eventGrid);
            if (!reporter.DrainEvents().empty())
            {
                std::cerr << "FAIL breach event test: a fresh system must report nothing\n";
                return 1;
            }
            reporter.AddPressure({ 2, 1 }, 10.0f, eventGrid);
            const std::vector<BreachEvent> warned = reporter.DrainEvents();
            if (warned.size() != 1 || warned.front().state != BreachState::Warning)
            {
                std::cerr << "FAIL breach event test: crossing a threshold must report it once\n";
                return 1;
            }
            if (!reporter.DrainEvents().empty())
            {
                std::cerr << "FAIL breach event test: draining must clear what it returned\n";
                return 1;
            }
            reporter.AddPressure({ 2, 1 }, 30.0f, eventGrid);
            const std::vector<BreachEvent> broke = reporter.DrainEvents();
            if (broke.empty() || broke.back().state != BreachState::Broken)
            {
                std::cerr << "FAIL breach event test: a collapse must be reported\n";
                return 1;
            }
        }
            return 0;
        }

        // 비용 필드로서의 소리 — **짧게 뚫는 길이 아니라 싸게 돌아가는 길**,
        // 발소리를 삼키는 벽, 빠지는 압력, 그리고 같은 시드가 두 번 같게 반응하는가.
        static int Acoustics()
        {
        // 다익스트라가 **짧게 뚫는 길이 아니라 싸게 돌아가는 길**을 골라야 한다.
        // (4,0) 으로 곧장 가면 2 + 6(벽) + 1 = 9 이고, 벽 밑으로 빠지면 8 이다.
        Grid detour(5, 3);
        for (int probeY = 0; probeY < 3; ++probeY)
            for (int probeX = 0; probeX < 5; ++probeX) detour.Set({ probeX, probeY }, Tile::Floor);
        detour.Set({ 3, 0 }, Tile::Wall);
        detour.Set({ 3, 1 }, Tile::Wall);

        const std::vector<float> heard = AStar::FloodCosts(detour, { 0, 0 }, CostField::Acoustic);
        const std::vector<float> walked = AStar::FloodCosts(detour, { 0, 0 }, CostField::Traversal);
        if (std::abs(heard[4] - 8.0f) > 0.001f)
        {
            std::cerr << "FAIL acoustic test: sound must take the cheaper detour, not punch through\n";
            return 1;
        }
        if (!std::isfinite(heard[3]) || std::isfinite(walked[3]))
        {
            std::cerr << "FAIL acoustic test: sound reaches a wall tile, a body never does\n";
            return 1;
        }

        // 복도 하나에 침입 창문 둘. 오른쪽 것은 **벽 셋 뒤에** 있으므로 같은 사격이
        // 훨씬 약해져서 닿아야 한다.
        //   z  =  .  S  #  #  #  .  =  z
        Grid corridor(10, 3);
        for (const int floorX : { 0, 2, 3, 7, 9 }) corridor.Set({ floorX, 1 }, Tile::Floor);
        corridor.Set({ 1, 1 }, Tile::WindowIntact);
        corridor.Set({ 8, 1 }, Tile::WindowIntact);
        corridor.breachSpawns.push_back({ { 1, 1 }, { 0, 1 } });
        corridor.breachSpawns.push_back({ { 8, 1 }, { 9, 1 } });
        const Int2 noiseOrigin{ 3, 1 };

        Grid loudGrid = corridor;
        BreachSystem loudTest(
            BreachTuning{ 30.0f, 60.0f, 100.0f, 0.0f, 0.0f },
            NoiseTuning{ 1.5f, 0.5f, 0.0f, 2 });
        loudTest.Initialize(loudGrid);
        const float nearCost = loudTest.PathCost(noiseOrigin, { 1, 1 }, loudGrid);
        const float farCost = loudTest.PathCost(noiseOrigin, { 8, 1 }, loudGrid);
        if (std::abs(nearCost - 5.0f) > 0.001f || std::abs(farCost - 23.0f) > 0.001f)
        {
            std::cerr << "FAIL acoustic test: path cost must charge 6 per wall and 4 for the glass\n";
            return 1;
        }

        std::mt19937 acousticRandom(20260827u);
        loudTest.ApplyNoise(TileCenter(noiseOrigin), 100.0f, 1.0f, loudGrid, acousticRandom);
        const float nearPressure = loudTest.Points()[0].pressure;
        const float farPressure = loudTest.Points()[1].pressure;
        if (std::abs(nearPressure - 46.25f) > 0.01f || std::abs(farPressure - 32.75f) > 0.01f)
        {
            std::cerr << "FAIL acoustic test: pressure must be effective noise times the scale\n";
            return 1;
        }
        if (!(farPressure < nearPressure))
        {
            std::cerr << "FAIL acoustic test: the breach behind more walls must take less pressure\n";
            return 1;
        }

        // 걷기는 벽 셋이 완전히 삼킬 만큼 조용하다.
        Grid quietGrid = corridor;
        BreachSystem quietTest(
            BreachTuning{ 30.0f, 60.0f, 100.0f, 0.0f, 0.0f },
            NoiseTuning{ 1.5f, 0.5f, 0.0f, 2 });
        quietTest.Initialize(quietGrid);
        quietTest.ApplyNoise(TileCenter(noiseOrigin), 8.0f, 1.0f, quietGrid, acousticRandom);
        if (quietTest.Points()[1].pressure != 0.0f || !(quietTest.Points()[0].pressure > 0.0f))
        {
            std::cerr << "FAIL acoustic test: a sufficiently walled breach must take no pressure at all\n";
            return 1;
        }

        // 압력은 유예 동안 버틴 다음 빠진다. **금은 절대 아물지 않는다.**
        Grid decayGrid = corridor;
        BreachSystem decayTest(
            BreachTuning{ 30.0f, 60.0f, 100.0f, 3.0f, 3.0f },
            NoiseTuning{ 1.5f, 0.5f, 0.0f, 1 });
        decayTest.Initialize(decayGrid);
        decayTest.AddPressure({ 1, 1 }, 20.0f, decayGrid);
        decayTest.Update(2.9f, decayGrid);
        if (std::abs(decayTest.Points()[0].pressure - 20.0f) > 0.001f)
        {
            std::cerr << "FAIL decay test: pressure must not fall within the delay after an increase\n";
            return 1;
        }
        decayTest.Update(0.2f, decayGrid);
        if (!(decayTest.Points()[0].pressure < 20.0f))
        {
            std::cerr << "FAIL decay test: pressure must start falling once the delay has passed\n";
            return 1;
        }

        decayTest.AddPressure({ 1, 1 }, 60.0f, decayGrid);
        if (decayTest.Points()[0].state != BreachState::Cracked)
        {
            std::cerr << "FAIL decay test: crossing the crack threshold must crack the window\n";
            return 1;
        }
        for (int tick = 0; tick < 600; ++tick) decayTest.Update(0.1f, decayGrid);
        if (decayTest.Points()[0].state != BreachState::Cracked || decayTest.Points()[0].pressure > 0.001f)
        {
            std::cerr << "FAIL decay test: a crack is permanent even after the pressure is gone\n";
            return 1;
        }

        // 같은 시드, 같은 소음 흐름, 같은 반응 순서.
        const auto reactionOrder = [&corridor, noiseOrigin](std::uint32_t streamSeed)
        {
            Grid replayGrid = corridor;
            BreachSystem replay(
                BreachTuning{ 30.0f, 60.0f, 100.0f, 0.0f, 0.0f },
                NoiseTuning{ 1.5f, 0.5f, 0.02f, 1 });
            replay.Initialize(replayGrid);
            std::mt19937 stream(streamSeed);
            std::vector<Int2> order;
            for (int event = 0; event < 24; ++event)
                for (const Int2 responder : replay.ApplyNoise(
                    TileCenter(noiseOrigin), 40.0f, 0.5f, replayGrid, stream))
                    order.push_back(responder);
            return order;
        };
        if (reactionOrder(7u).empty() || reactionOrder(7u) != reactionOrder(7u))
        {
            std::cerr << "FAIL acoustic test: the same seed and noise stream must replay identically\n";
            return 1;
        }

        // 걷기는 소리를 내야 하고, 고정 업데이트마다가 아니라 **보폭마다 정확히
        // 한 번**이어야 한다. 업데이트마다 나는 발소리는 예순 배로 시끄럽다.
        Balance loudSteps = Balance::Defaults();
        loudSteps.walkNoise = 400.0f;
        loudSteps.walkReactionChance = 1.0f;
        Game walker(20260826u, loudSteps);
        InputState stepping{};
        stepping.forward = true;
        walker.Update(1.0f / 60.0f, stepping);
        const float afterFirstStride = walker.footstepTimer;
        walker.Update(1.0f / 60.0f, stepping);
        if (afterFirstStride <= 0.0f || walker.footstepTimer >= afterFirstStride
            || walker.footstepTimer <= 0.0f)
        {
            std::cerr << "FAIL footstep test: a stride must arm the timer and the next update must not refire\n";
            return 1;
        }
        for (int frame = 0; frame < 180; ++frame) walker.Update(1.0f / 60.0f, stepping);
        float walkedPressure = 0.0f;
        for (const BreachPoint& point : walker.breachSystem.Points()) walkedPressure += point.pressure;
        if (walkedPressure <= 0.0f)
        {
            std::cerr << "FAIL footstep test: walking must raise breach pressure\n";
            return 1;
        }

        // 행동 하나하나가 설정 파일에서 독립적으로 튜닝된다.
        const std::filesystem::path tunedPath =
            std::filesystem::temp_directory_path() / "consolezombie-balance-selftest.ini";
        {
            std::ofstream tuned(tunedPath);
            tuned << "walk_noise = 1" << '\n' << "run_noise = 2" << '\n' << "roll_noise = 3" << '\n'
                << "door_noise = 4" << '\n' << "shot_noise = 5" << '\n'
                << "walk_reaction_chance = 0.11" << '\n' << "run_reaction_chance = 0.22" << '\n'
                << "roll_reaction_chance = 0.33" << '\n' << "door_reaction_chance = 0.44" << '\n'
                << "shot_reaction_chance = 0.55" << '\n' << "breach_decay_delay = 7" << '\n'
                << "noise_acoustic_falloff = 2.5" << '\n' << "noise_pressure_scale = 0.25" << '\n';
        }
        const Balance fromFile = Balance::Load(tunedPath);
        std::error_code removeError;
        std::filesystem::remove(tunedPath, removeError);
        const bool loudnessLoaded = fromFile.walkNoise == 1.0f && fromFile.runNoise == 2.0f
            && fromFile.rollNoise == 3.0f && fromFile.doorNoise == 4.0f && fromFile.shotNoise == 5.0f;
        const bool chancesLoaded = std::abs(fromFile.walkReactionChance - 0.11f) < 0.0001f
            && std::abs(fromFile.runReactionChance - 0.22f) < 0.0001f
            && std::abs(fromFile.rollReactionChance - 0.33f) < 0.0001f
            && std::abs(fromFile.doorReactionChance - 0.44f) < 0.0001f
            && std::abs(fromFile.shotReactionChance - 0.55f) < 0.0001f;
        const bool falloffLoaded = fromFile.breachDecayDelay == 7.0f
            && fromFile.noiseAcousticFalloff == 2.5f && fromFile.noisePressureScale == 0.25f;
        if (!loudnessLoaded || !chancesLoaded || !falloffLoaded)
        {
            std::cerr << "FAIL balance test: each action must tune independently from the config file\n";
            return 1;
        }
            return 0;
        }

        // 프레임이 뷰포트를 따라가는가, 개구부 흔적, 조준선, 중립적인 회색들,
        // 그리고 탈출구가 **두 상태 모두 바닥 구역으로 읽히는가.**
        static int Rendering(const MapGenerator& generator, const Grid& sample)
        {
        // 탑다운 배치의 왕복. 그리는 쪽은 타일 → 픽셀로 가고, 커서 표시는 픽셀 →
        // 타일로 돌아온다. **둘이 같은 함수를 쓰는 것만으로는 부족하다** — 되돌리는
        // 산술 자체가 틀릴 수 있고, 그러면 화면이 밝히는 칸과 숫자가 말하는 칸이 갈린다.
        //
        // 원점 왼쪽·위쪽까지 본다. 거기서 C 의 나눗셈은 0 쪽으로 끌어당기므로
        // **-1 픽셀과 +1 픽셀이 둘 다 타일 0** 이 된다. floor 가 아니면 여기서 걸린다.
        for (const int size : { 120, 200, 360 })
        {
            const TopDownLayout layout =
                RaycastRenderer::MakeTopDownLayout(size * 2, size, sample);
            for (int y = 0; y < sample.Height(); y += 7)
            {
                for (int x = 0; x < sample.Width(); x += 5)
                {
                    const Int2 tile{ x, y };
                    if (!(layout.TileAt(layout.CenterOf(tile)) == tile))
                    {
                        std::cerr << "FAIL top-down test: the layout must map a tile centre"
                            " back to that tile\n";
                        return 1;
                    }
                }
            }
            // 지도 바로 바깥 왼쪽 위. 격자 안 좌표가 나오면 커서 표시가 존재하지
            // 않는 칸의 비용을 자신 있게 적게 된다.
            const Int2 outside = layout.TileAt({ layout.originX - 1, layout.originY - 1 });
            if (sample.Contains(outside))
            {
                std::cerr << "FAIL top-down test: a pixel above and left of the map must not"
                    " land inside the grid\n";
                return 1;
            }
        }

        // 프레임은 터미널 뷰포트에 맞춰진다. 사용자가 최대화할 때 손으로 바꾸는
        // 그것이다. 깨끗하게 크기가 바뀌고 상하한 안에 머물러야 한다.
        RaycastRenderer sizing;
        if (sizing.Width() != RaycastRenderer::DefaultWidth
            || sizing.Height() != RaycastRenderer::DefaultHeight)
        {
            std::cerr << "FAIL resolution test: a default renderer must use the fixed reference size\n";
            return 1;
        }
        sizing.Resize(320, 200);
        if (sizing.Width() != 320 || sizing.Height() != 200)
        {
            std::cerr << "FAIL resolution test: Resize must adopt the requested frame size\n";
            return 1;
        }

        // 환경 사격 레이도 벽·좀비와 같은 열→카메라 오프셋과 ViewAngle을 쓴다.
        // 오른쪽 끝 열은 width와 width-1이 실제로 다른 타일에 닿도록 긴 방에서 잰다.
        {
            RaycastRenderer aimRenderer(RaycastRenderer::MinWidth, RaycastRenderer::MinHeight);
            Grid aimGrid(96, 96);
            for (int y = 1; y < aimGrid.Height() - 1; ++y)
                for (int x = 1; x < aimGrid.Width() - 1; ++x)
                    aimGrid.Set({ x, y }, Tile::Floor);
            RenderState aimState;
            aimState.playerPosition = TileCenter({ 48, 48 });
            aimState.playerAngle = 0.0f;
            aimState.aimPixel = { aimRenderer.Width() - 1, aimRenderer.Height() / 2 };
            for (const bool lookingBehind : { false, true })
            {
                aimState.lookingBehind = lookingBehind;
                for (const int requestedColumn : {
                    0, aimRenderer.Width() / 2, aimRenderer.Width() - 1 })
                {
                    aimState.aimPixel.x = requestedColumn;
                    const int column = std::clamp(requestedColumn, 0, aimRenderer.Width() - 1);
                    const float camera = static_cast<float>(column)
                        / static_cast<float>(aimRenderer.Width() - 1) - 0.5f;
                    const RaycastRenderer::RayHit expected = aimRenderer.CastRay(aimGrid,
                        aimState.playerPosition,
                        aimState.ViewAngle() + camera * RaycastRenderer::FieldOfView);
                    const EnvironmentHit actual = aimRenderer.HitTestEnvironment(aimGrid, aimState);
                    if (!actual.valid || !expected.hit || !(actual.position == expected.position)
                        || actual.tile != expected.tile
                        || std::abs(actual.distance - expected.distance) > 0.0001f)
                    {
                        std::cerr << "FAIL environment aim test: environment and camera rays disagree\n";
                        return 1;
                    }
                }
            }
        }
        sizing.Resize(100000, 100000);
        if (sizing.Width() != RaycastRenderer::MaxWidth || sizing.Height() != RaycastRenderer::MaxHeight)
        {
            std::cerr << "FAIL resolution test: an oversized viewport must clamp to the upper bound\n";
            return 1;
        }
        sizing.Resize(1, 1);
        if (sizing.Width() != RaycastRenderer::MinWidth || sizing.Height() != RaycastRenderer::MinHeight)
        {
            std::cerr << "FAIL resolution test: a tiny viewport must clamp to the lower bound\n";
            return 1;
        }
        // 새 크기에서도 렌더링이 계속 돌아야 한다. 깊이 버퍼가 프레임 폭으로
        // 색인되므로, **옛 크기로 남은 버퍼는 바로 여기서 드러난다.**
        sizing.Resize(320, 200);
        RenderState sizedState;
        sizedState.playerPosition = TileCenter(sample.playerSpawn);
        const PixelBuffer& sizedFrame = sizing.Render(sample, sizedState, {});
        if (sizedFrame.Width() != 320 || sizedFrame.Height() != 200)
        {
            std::cerr << "FAIL resolution test: the rendered frame must match the resized buffer\n";
            return 1;
        }

        // 깨진 창문과 열린 문은 **흔적을 남겨야 한다.** 없으면 빈 출입구와 픽셀
        // 단위로 똑같고, 그건 창문이 실제로는 E 를 요구하는데 걸어서 지나갈 수 있다고
        // 약속하는 것이다.
        //
        // 1타일 복도를 쓰면 어떤 생성 맵에도 안 기댄다 — 플레이어 (1,4),
        // 구멍 (4,4), 막다른 벽 (8,4).
        RaycastRenderer openingRenderer;
        Grid opening(9, 9);
        for (int corridorX = 1; corridorX < 8; ++corridorX) opening.Set({ corridorX, 4 }, Tile::Floor);
        opening.keyPosition = { -1, -1 };
        opening.exit = { { -1, -1 }, true };
        RenderState openingState;
        openingState.playerPosition = TileCenter({ 1, 4 });
        openingState.playerAngle = 0.0f;
        // 조준선은 조준 픽셀에 그려지는데 그 기본값이 프레임의 정확한 가운데라,
        // **이 검사가 표본으로 삼는 열을 덮어버린다.**
        openingState.aimPixel = { 2, 2 };

        const auto remnantColumn = [&openingRenderer, &opening, &openingState](Tile tile)
        {
            opening.Set({ 4, 4 }, tile);
            const PixelBuffer& pixels = openingRenderer.Render(opening, openingState, {});
            std::vector<Rgb> column;
            column.reserve(static_cast<std::size_t>(RaycastRenderer::DefaultHeight));
            for (int y = 0; y < RaycastRenderer::DefaultHeight; ++y)
                column.push_back(pixels.Get(RaycastRenderer::DefaultWidth / 2, y));
            return column;
        };
        // 회색 벽·천장·바닥이 전부 거의 중립색이라, **채널 비율 하나로** 유리와
        // 목재를 그 열이 담을 수 있는 다른 모든 것과 구별할 수 있다.
        const auto isGlass = [](Rgb pixel) { return pixel.b > pixel.r * 1.6f && pixel.b > 24; };
        const auto isTimber = [](Rgb pixel) { return pixel.r > pixel.b * 1.5f && pixel.r > 24; };
        const auto anyBetween = [](const std::vector<Rgb>& column, int fromY, int toY, auto predicate)
        {
            for (int y = fromY; y <= toY; ++y)
                if (predicate(column[static_cast<std::size_t>(y)])) return true;
            return false;
        };

        constexpr int horizonRow = RaycastRenderer::DefaultHeight / 2;
        const std::vector<Rgb> intactColumn = remnantColumn(Tile::WindowIntact);
        const std::vector<Rgb> brokenColumn = remnantColumn(Tile::WindowBroken);
        if (!isGlass(intactColumn[horizonRow]) || isGlass(brokenColumn[horizonRow]))
        {
            std::cerr << "FAIL remnant test: breaking a window must open the line of sight\n";
            return 1;
        }
        if (!anyBetween(brokenColumn, 45, 54, isGlass))
        {
            std::cerr << "FAIL remnant test: a broken window must keep a sill to climb over\n";
            return 1;
        }

        const std::vector<Rgb> closedColumn = remnantColumn(Tile::DoorClosed);
        const std::vector<Rgb> openColumn = remnantColumn(Tile::DoorOpen);
        if (!isTimber(closedColumn[horizonRow]) || isTimber(openColumn[horizonRow]))
        {
            std::cerr << "FAIL remnant test: opening a door must open the line of sight\n";
            return 1;
        }
        if (!anyBetween(openColumn, 25, 29, isTimber))
        {
            std::cerr << "FAIL remnant test: an opened door must leave its frame behind\n";
            return 1;
        }
        if (anyBetween(openColumn, 45, 54, isTimber))
        {
            std::cerr << "FAIL remnant test: a doorway must not grow a sill; only windows are climbed\n";
            return 1;
        }

        // 탈출구는 **두 상태 모두 바닥 구역으로 읽혀야 한다.** 예전에는 나가는 길을
        // 막을 수도 있는 붉은 벽으로 서 있었고, 열쇠가 그것을 여는 순간 보이지 않게
        // 됐다.
        //
        // 지평선 아래에 이만큼 채도가 높은 것이 달리 없으므로, **색조를 세는 것만으로**
        // 그 구역을 벽·바닥·HUD 와 가를 수 있다.
        RaycastRenderer exitRenderer;
        const Grid exitGrid = generator.Generate(20260826u);
        RenderState exitState;
        exitState.playerPosition = TileCenter(exitGrid.playerSpawn);
        const Vec2 exitOffset = TileCenter(exitGrid.exit.origin) - exitState.playerPosition;
        exitState.playerAngle = std::atan2(exitOffset.y, exitOffset.x);

        const auto countZonePixels = [&exitRenderer, &exitGrid, &exitState](bool carryingKey, bool green)
        {
            exitState.hasKey = carryingKey;
            const PixelBuffer& pixels = exitRenderer.Render(exitGrid, exitState, {});
            int count = 0;
            for (int y = RaycastRenderer::DefaultHeight / 2; y < RaycastRenderer::DefaultHeight; ++y)
            {
                for (int x = 0; x < RaycastRenderer::DefaultWidth; ++x)
                {
                    const Rgb pixel = pixels.Get(x, y);
                    const bool strongRed = pixel.r > pixel.g * 2.5f && pixel.r > pixel.b * 2.5f;
                    const bool strongGreen = pixel.g > pixel.r * 2.5f && pixel.g > pixel.b * 2.0f;
                    if (green ? strongGreen : strongRed) ++count;
                }
            }
            return count;
        };

        if (countZonePixels(false, false) <= 0 || countZonePixels(false, true) != 0)
        {
            std::cerr << "FAIL exit zone test: a locked exit must show as a red floor zone\n";
            return 1;
        }
        if (countZonePixels(true, true) <= 0 || countZonePixels(true, false) != 0)
        {
            std::cerr << "FAIL exit zone test: the key must turn the exit zone green\n";
            return 1;
        }
        if (exitGrid.IsOpaque(exitGrid.exit.origin) || !exitGrid.IsWalkable(exitGrid.exit.origin)
            || exitGrid.IsOpaque(exitGrid.exit.Second()) || !exitGrid.IsWalkable(exitGrid.exit.Second()))
        {
            std::cerr << "FAIL exit zone test: the exit must never block sight or movement\n";
            return 1;
        }
            return 0;
        }
    };

    int Game::RunSelfTest(const std::filesystem::path& executable)
    {
        // 여기 붙들어 둔다. 그래야 아래 검사들이 숫자를 다시 적는 대신
        // **생성기가 실제로 만들어질 때 받은 그 설정**에 대고 비교할 수 있다.
        const MapGenerationSettings mapSettings;
        MapGenerator generator(mapSettings);

        // 걷고, 보고, 듣는 어떤 것보다 먼저 — 그것들이 전부 읽는 규칙이다.
        if (const int code = SelfTestAccess::TileRules(); code != 0) return code;

        if (const int code = SelfTestAccess::MapGeneration(mapSettings, generator); code != 0) return code;

        // 나머지 검사들이 가리키는 맵 하나. 한 번만 만든다.
        Grid sample = generator.Generate(20260826u);
        if (const int code = SelfTestAccess::BreachPressure(sample); code != 0) return code;
        if (const int code = SelfTestAccess::BreachFlow(); code != 0) return code;
        if (const int code = SelfTestAccess::Stamina(); code != 0) return code;
        if (const int code = SelfTestAccess::ConfigAndText(executable); code != 0) return code;
        if (const int code = SelfTestAccess::Audio(); code != 0) return code;
        if (const int code = SelfTestAccess::AimSway(); code != 0) return code;
        if (const int code = SelfTestAccess::LookBehind(); code != 0) return code;
        if (const int code = SelfTestAccess::Screens(); code != 0) return code;
        if (const int code = SelfTestAccess::ZombieSprites(); code != 0) return code;
        if (const int code = SelfTestAccess::ZombieSeparation(); code != 0) return code;
        if (const int code = SelfTestAccess::SealedDoors(); code != 0) return code;
        if (const int code = SelfTestAccess::ZombieWaking(); code != 0) return code;
        {
            const Balance shipped = Balance::Load(Balance::FindConfigFile(executable));
            if (const int code = SelfTestAccess::BalanceSanity(shipped); code != 0) return code;
            if (const int code = SelfTestAccess::ShippedConfigGenerates(shipped); code != 0) return code;
            if (const int code = SelfTestAccess::BalanceReload(); code != 0) return code;
        }
        if (const int code = SelfTestAccess::StatusLog(); code != 0) return code;
        if (const int code = SelfTestAccess::Acoustics(); code != 0) return code;
        if (const int code = SelfTestAccess::Rendering(generator, sample); code != 0) return code;

        std::cout << "PASS: one table of tile rules that walking, seeing, path cost and sound are all read from, agreeing with itself on what blocks and what merely charges; 64 deterministic maps; nine trace stages; exhausted generation never returning an invalid grid or trace; leaves that tile the root; tile connectivity matching the planned Room Graph; connected rooms; a two-tile exit in the start room wall with the key three edges and 40 cost away; two-tile corridors with doorways per edge, a solid border and sealed infected spaces; four plain doors on distinct edges, one infected space opening two ways from an interior leaf, one traversal window; breach candidates limited to real intrusions; traversal model agreement; acoustic Dijkstra falloff, decay delay and permanent cracks; per-action noise from config; twelve zombies with three per infected space and six spread one or two to a room around an empty one, deterministic local separation that keeps pursuing zombies from occupying one body, four extra rounds in two bundles, window shooting, E vaulting, both breach kinds waking their zombies, player and zombie opening both tiles of a doorway, secondary breach pressure following the responder, an eight round budget on the whole map, the shipped config being what was baked into the binary, refusing malformed and non-finite numeric values or a config that could clear the map, and still generating maps with the normalized numbers the game actually plays, a balance file that can be reread mid-run with what only reaches the next map named and a broken one refused whole, Hangul HUD text baked from a system font into frame pixels with a fallback that checks what GDI really loaded, frames written only on the cells they claim, a status log that ages out and refuses to repeat, breach transitions reported as events, shots heard only as far as the acoustic field carries them, woken zombies breaking their own breach, a growl on noticing the player however it noticed and every few seconds after, staggered so a woken group is not a metronome, stamina spent by running and rolling, a look behind that turns the camera and refuses the shot, environment aim rays matching camera columns at the frame edge, an aim that drifts once and reproducibly and settles when the player stops, a wav parser that walks past a LIST chunk and cues that stay silent with no device, a screen flow whose keys step one screen at a time, a menu that rolls a new map unless a seed was asked for, a pause that freezes the world and the clock, an R that hands back the same map and an N that does not, a record that ignores a trigger pull on an empty chamber, every screen key also offered to the pointer with exactly the line under it lit and a click that starts a run, cards that stay inside the frame at every size the game renders at, a zombie silhouette whose every drawn pixel can be shot and whose every gap cannot, through a whole stride, a body that falls and stops being a target, eyes only on a head turned this way, a gait measured in distance walked, a sealed door that keeps its door and wears bars, and five screens that each say something and no two the same; the exit floor zone, window/door remnants and viewport-fitted resolution verified.\n";
        // HUD 가 배치하는 것 전부를 대표하는 숫자 하나.
        //
        // 렌더 다이제스트는 **렌더러가 그리는 픽셀**을 접는다. HUD 는 그중이 아니다 —
        // Game 이 만들고 TextRaster 가 나중에 얹는 글자 덩어리 목록이라
        // **아무도 그것을 보고 있지 않았다.** 이 코드가 상태 행렬을 MakeOverlay() 에
        // 통과시키고 모든 덩어리의 위치·색·높이·바이트를 접는다.
        //
        // 렌더 다이제스트와 같은 규칙 — **이 값이 움직였으면 HUD 가 움직인 것이다.**
        {
            std::uint64_t hud = 1469598103934665603ull;
            const auto fold = [&hud](std::uint64_t value)
            {
                for (int byte = 0; byte < 8; ++byte)
                {
                    hud ^= (value >> (byte * 8)) & 0xFFull;
                    hud *= 1099511628211ull;
                }
            };

            Game dresser(20260826u, Balance::Defaults());
            const int sizes[][2] = { { 120, 80 }, { 360, 200 }, { 160, 100 }, { 480, 320 } };
            for (int step = 0; step < 16; ++step)
            {
                dresser.health = step % 4;
                dresser.ammo = step % 5;
                dresser.hasKey = (step % 2) == 0;
                dresser.measuredFps = 12.0f + static_cast<float>(step) * 7.5f;
                dresser.lookingBehind = (step % 3) == 0;
                dresser.won = step == 5;
                dresser.lost = step == 9;
                // 결과 카드가 보고하는 기록. 값을 바꿔가며 넣으므로, **판이 아니라
                // 상수를 찍는 카드는 여기서 다른 숫자가 된다.**
                dresser.runSeconds = static_cast<float>(step) * 13.7f;
                dresser.shotsFired = step;
                dresser.shotsHit = step / 2;
                dresser.kills = step / 3;
                dresser.statusLog.Clear();
                if (step % 2 == 0) dresser.PushLog("문을 두드리는 소리가 들린다");
                if (step % 3 == 0) dresser.PushLog("창문이 깨졌다");
                if (step % 4 == 0) dresser.PushLog("열쇠를 손에 넣었다. 탈출구로 돌아가라");

                const int (&size)[2] = sizes[step % 4];
                const auto foldSpans = [&fold](const std::vector<TextSpan>& spans)
                {
                    for (const TextSpan& span : spans)
                    {
                        fold(static_cast<std::uint64_t>(span.pixel.x + 4096));
                        fold(static_cast<std::uint64_t>(span.pixel.y + 4096));
                        fold(static_cast<std::uint64_t>(span.height));
                        fold(static_cast<std::uint64_t>(span.color.r) << 16
                            | static_cast<std::uint64_t>(span.color.g) << 8
                            | static_cast<std::uint64_t>(span.color.b));
                        for (const char character : span.text)
                            fold(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
                    }
                };
                foldSpans(dresser.MakeOverlay(size[0], size[1]));
                // 메뉴, 생성 카드, 일시정지 카드, 결과 카드 둘이 **같은 접기를**
                // 통과한다. 아무도 접지 않는 화면은 오라클이 볼 수 없는 화면이고,
                // **이 프로젝트는 정확히 그런 식으로 눈먼 검사를 이미 네 번 출하했다.**
                static const Screen cards[] = { Screen::MainMenu, Screen::Generating,
                    Screen::Playing, Screen::Paused, Screen::Dead, Screen::Escaped };
                dresser.screen = cards[step % 6];
                // 패스마다 다른 선택지 위에 포인터를 올린다. 추측이 아니라 **카드
                // 자신의 버튼 좌표**에서 가져오므로, 밝아진 상태가 오라클 안에 들어온다.
                // 아니면 쉬는 색으로만 접히게 된다.
                const RunSummary shown = dresser.MakeRunSummary();
                const ScreenLayout resting = BuildScreenLayout(dresser.screen, shown,
                    size[0], size[1], dresser.balance.uiTextHeight, Int2{ -1, -1 });
                const Int2 pointer = resting.buttons.empty()
                    ? Int2{ -1, -1 }
                    : resting.buttons[static_cast<std::size_t>(step) % resting.buttons.size()]
                        .bounds.Center();
                foldSpans(BuildScreenLayout(dresser.screen, shown,
                    size[0], size[1], dresser.balance.uiTextHeight, pointer).spans);
                // 개발자 탑다운의 숫자들도 같은 접기를 지난다. **HUD 다이제스트가
                // 보는 것이 이제 화면에 나오는 글자 전부**여야 한다 — 아무도 안 접는
                // 글자 덩어리는 조용히 사라져도 아무 값도 안 움직인다.
                //
                // 게임에서 실제로 오는 값을 쓰되, 게임이 지금 못 만드는 상태 —
                // 쫓는 좀비, 히트맵 세 상태, 격자 밖 커서 — 는 손으로 섞는다.
                DebugReadout readout = dresser.MakeDebugReadout(size[0], size[1]);
                readout.chasers = step % 5;
                readout.hasNearestChase = (step % 3) != 0;
                readout.nearestChaseCost = 4.5f + static_cast<float>(step) * 2.25f;
                readout.nearestChaseTiles = step * 3;
                readout.guideToExit = (step % 2) == 0;
                readout.hasGuide = (step % 7) != 0;
                readout.hasCursor = (step % 4) != 0;
                // 못 가는 칸. "-" 로 적히는 갈래가 여기 없으면 오라클 밖에 남는다.
                if (step % 8 == 3) readout.cursorTraversal = std::numeric_limits<float>::infinity();
                static const CostOverlay fields[] = { CostOverlay::None,
                    CostOverlay::Traversal, CostOverlay::Acoustic };
                readout.overlay = fields[step % 3];
                readout.overlayScale = 12.0f + static_cast<float>(step);
                foldSpans(BuildDebugReadout(readout, size[0], size[1],
                    dresser.balance.uiTextHeight));
            }
            std::cout << "HUD digest " << hud << "\n";
        }

        // 렌더러가 그리는 것 전부를 대표하는 숫자 하나. 맵 해시가 세계가 같다는 것을
        // 증명하고, **이것이 그 세계가 여전히 같은 방식으로 그려진다는 것을 증명한다.**
        // 이 값을 움직인 리팩토링은 diff 가 어떻게 보였든 **그림을 바꾼 것이다.**
        std::cout << "Render digest " << ComputeRenderDigest() << "\n";
        std::cout << "Sample seed 20260826 | hash " << sample.StableHash()
            << " | rooms " << sample.rooms.size() << " | zombies " << sample.zombieSpawns.size()
            << " | breach pockets " << sample.breachSpawns.size() << "\n";
        for (int y = 0; y < sample.Height(); ++y)
        {
            for (int x = 0; x < sample.Width(); ++x)
            {
                const Int2 point{ x, y };
                char glyph = '#';
                switch (sample.Get(point))
                {
                case Tile::Floor: glyph = '.'; break;
                case Tile::DoorClosed: glyph = '+'; break;
                case Tile::DoorWarning: glyph = '!'; break;
                case Tile::DoorCracked: glyph = '%'; break;
                case Tile::DoorBroken: glyph = '-'; break;
                case Tile::DoorOpen: glyph = '/'; break;
                case Tile::WindowIntact: glyph = '='; break;
                case Tile::WindowWarning: glyph = '!'; break;
                case Tile::WindowCracked: glyph = '%'; break;
                case Tile::WindowBroken: glyph = '-'; break;
                case Tile::ExitLocked: glyph = 'E'; break;
                case Tile::ExitOpen: glyph = 'O'; break;
                default: break;
                }
                if (point == sample.playerSpawn) glyph = 'P';
                if (point == sample.keyPosition) glyph = 'K';
                for (const BreachSpawn& spawn : sample.breachSpawns)
                {
                    if (point == spawn.waitingPosition) glyph = 'z';
                }
                std::cout << glyph;
            }
            std::cout << '\n';
        }
        return 0;
    }

    int Game::WriteSnapshot(std::uint32_t seed, const std::filesystem::path& path)
    {
        MapGenerator generator;
        const Grid grid = generator.Generate(seed);
        RaycastRenderer renderer;
        RenderState state;
        state.playerPosition = TileCenter(grid.playerSpawn);
        state.playerAngle = 3.14159265f;
        state.aimPixel = { RaycastRenderer::DefaultWidth / 2, RaycastRenderer::DefaultHeight / 2 };
        state.showDebugMap = true;
        std::vector<RenderActor> actors;
        Vec2 showcasePosition = state.playerPosition + Vec2{ -3.0f, 0.0f };
        if (!grid.IsWalkable(ToTile(showcasePosition)))
        {
            showcasePosition = state.playerPosition + Vec2{ 3.0f, 0.0f };
            state.playerAngle = 0.0f;
        }
        actors.push_back({ showcasePosition, true, true });
        for (const ZombieSpawn& spawn : grid.zombieSpawns)
            actors.push_back({ TileCenter(spawn.position), true, true });
        const PixelBuffer& pixels = renderer.Render(grid, state, actors);

        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            std::cerr << "Unable to write snapshot: " << path.string() << '\n';
            return 2;
        }
        const std::uint32_t rowStride = static_cast<std::uint32_t>((pixels.Width() * 3 + 3) & ~3);
        const std::uint32_t imageSize = rowStride * static_cast<std::uint32_t>(pixels.Height());
        std::array<unsigned char, 54> header{};
        const auto put16 = [&header](std::size_t offset, std::uint16_t value)
        {
            header[offset] = static_cast<unsigned char>(value & 0xffu);
            header[offset + 1] = static_cast<unsigned char>((value >> 8u) & 0xffu);
        };
        const auto put32 = [&header](std::size_t offset, std::uint32_t value)
        {
            for (int byte = 0; byte < 4; ++byte)
                header[offset + static_cast<std::size_t>(byte)] = static_cast<unsigned char>((value >> (byte * 8)) & 0xffu);
        };
        header[0] = 'B';
        header[1] = 'M';
        put32(2, 54u + imageSize);
        put32(10, 54u);
        put32(14, 40u);
        put32(18, static_cast<std::uint32_t>(pixels.Width()));
        put32(22, static_cast<std::uint32_t>(pixels.Height()));
        put16(26, 1u);
        put16(28, 24u);
        put32(34, imageSize);
        file.write(reinterpret_cast<const char*>(header.data()), header.size());

        std::vector<unsigned char> row(rowStride, 0u);
        for (int y = pixels.Height() - 1; y >= 0; --y)
        {
            for (int x = 0; x < pixels.Width(); ++x)
            {
                const Rgb pixel = pixels.Get(x, y);
                const std::size_t offset = static_cast<std::size_t>(x * 3);
                row[offset] = pixel.b;
                row[offset + 1] = pixel.g;
                row[offset + 2] = pixel.r;
            }
            file.write(reinterpret_cast<const char*>(row.data()), row.size());
        }
        std::cout << "Wrote renderer snapshot: " << path.string() << '\n';
        return 0;
    }
}
