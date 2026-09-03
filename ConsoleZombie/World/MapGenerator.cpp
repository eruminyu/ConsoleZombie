#include "MapGenerator.h"

#include <World/MapGeneratorDetail.h>

#include <AI/AStar.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <string>
#include <array>
#include <limits>
#include <map>
#include <numeric>

namespace Zombie
{
    using MapDetail::GraphDistances;
    using MapDetail::Orthogonal;

    MapGenerator::MapGenerator(MapGenerationSettings settings)
        : settings(settings)
    {
    }

    namespace
    {
        // 시도 번호를 시드에 섞는다. 그래야 재시도까지 결정적이다 —
        // 같은 Base Seed 는 언제나 같은 시도 순서를 낸다.
        std::uint32_t MixSeed(std::uint32_t seed, int attempt)
        {
            std::uint64_t value = static_cast<std::uint64_t>(seed) * 0x9e3779b97f4a7c15ull
                + static_cast<std::uint64_t>(attempt) * 0xbf58476d1ce4e5b9ull;
            value ^= value >> 31;
            value *= 0x94d049bb133111ebull;
            value ^= value >> 33;
            return static_cast<std::uint32_t>(value);
        }

        // 구조적으로 폭 2타일이다. 행 Band 는 `row` 와 `row + 1` 을,
        // 열 Band 는 `column` 과 `column + 1` 을 덮는다.
        Rect RowBand(int firstColumn, int lastColumn, int row)
        {
            return { firstColumn, row, lastColumn - firstColumn + 1, 2 };
        }

        // 세로로 뻗는 폭 2타일 Band.
        Rect ColumnBand(int firstRow, int lastRow, int column)
        {
            return { column, firstRow, 2, lastRow - firstRow + 1 };
        }

        // Band 는 꺾이는 곳마다 2×2 블록을 공유하므로, 앞선 Band 가 이미 덮은
        // 타일은 두 번 방문하지 않고 건너뛴다.
        // 복도 Band 사슬이 덮는 타일을 중복 없이 한 번씩 방문한다.
        template <typename Visitor>
        void ForEachCorridorTile(const std::array<Rect, 3>& bands, int count, Visitor&& visit)
        {
            for (int index = 0; index < count; ++index)
            {
                const Rect& band = bands[static_cast<std::size_t>(index)];
                for (int y = band.y; y < band.Bottom(); ++y)
                {
                    for (int x = band.x; x < band.Right(); ++x)
                    {
                        bool covered = false;
                        for (int earlier = 0; earlier < index && !covered; ++earlier)
                            covered = bands[static_cast<std::size_t>(earlier)].Contains({ x, y });
                        if (!covered) visit(Int2{ x, y });
                    }
                }
            }
        }

        // 이 타일이 Band 사슬 안에 들어 있는가.
        bool CoversTile(const std::array<Rect, 3>& bands, int count, Int2 point)
        {
            for (int index = 0; index < count; ++index)
            {
                if (bands[static_cast<std::size_t>(index)].Contains(point)) return true;
            }
            return false;
        }

        // 방을 둘러싼 1타일 벽 링. 복도가 여기 닿을 수 있는 곳은 자기가 출입구로
        // 여는 두 타일뿐이다.
        bool OnWallRing(const Rect& room, Int2 point)
        {
            return point.x >= room.x - 1 && point.x <= room.Right()
                && point.y >= room.y - 1 && point.y <= room.Bottom()
                && !room.Contains(point);
        }

        // 상하좌우로 맞닿은 두 타일인가. 대각은 아니다.
        bool AreAdjacent(Int2 lhs, Int2 rhs)
        {
            return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y) == 1;
        }

        // 두 타일을 출입구 하나로 묶는다. origin 은 x, 같으면 y 가 작은 쪽이다.
        Doorway MakeDoorway(const Int2 (&tiles)[2], int edgeIndex, int roomIndex)
        {
            const bool firstIsOrigin = tiles[0].x < tiles[1].x
                || (tiles[0].x == tiles[1].x && tiles[0].y < tiles[1].y);
            Doorway doorway;
            doorway.tiles.origin = firstIsOrigin ? tiles[0] : tiles[1];
            doorway.tiles.spansX = tiles[0].y == tiles[1].y;
            doorway.edge = edgeIndex;
            doorway.room = roomIndex;
            return doorway;
        }

        // 후보 모양 하나에 점수를 매긴다. false 는 아예 쓸 수 없다는 뜻이라
        // 비교할 점수 자체가 없다. 점수는 낮을수록 좋고, 구조적 결함이 전부
        // 즉시 폐기가 된 지금 점수로 남은 것은 길이뿐이다.
        bool ScoreCorridor(const Grid& grid, const std::array<Rect, 3>& bands, int count,
            const Rect& roomA, const Rect& roomB, const std::vector<Rect>& blockers,
            int& score, Int2 (&doorA)[2], Int2 (&doorB)[2])
        {
            bool valid = true;
            int length = 0;
            int ringA = 0;
            int ringB = 0;

            ForEachCorridorTile(bands, count, [&](Int2 tile)
            {
                if (!valid) return;
                // 맵의 가장 바깥 링은 통행 불가 경계로 예약돼 있다.
                if (tile.x < 1 || tile.y < 1
                    || tile.x >= grid.Width() - 1 || tile.y >= grid.Height() - 1)
                {
                    valid = false;
                    return;
                }
                for (const Rect& blocker : blockers)
                {
                    if (blocker.Contains(tile))
                    {
                        valid = false;
                        return;
                    }
                    // 벽 없이 제3의 방 바닥과 맞닿으면 Room Graph 간선 없이 둘이
                    // 이어진다. 6.7 이 금지하는 것이고, 감염 공간이 밀폐된 채로
                    // 남는 것도 이 규칙 덕분이다.
                    for (const Int2 step : Orthogonal)
                    {
                        if (blocker.Contains({ tile.x + step.x, tile.y + step.y }))
                        {
                            valid = false;
                            return;
                        }
                    }
                }
                if (OnWallRing(roomA, tile))
                {
                    if (ringA >= 2)
                    {
                        valid = false;
                        return;
                    }
                    doorA[ringA++] = tile;
                }
                if (OnWallRing(roomB, tile))
                {
                    if (ringB >= 2)
                    {
                        valid = false;
                        return;
                    }
                    doorB[ringB++] = tile;
                }
                if (roomA.Contains(tile) || roomB.Contains(tile)) return;

                ++length;
                // 어느 방에도 속하지 않은 바닥은 앞선 간선을 위해 판 복도일 수밖에
                // 없다. 그것과 합쳐지는 것은 물론 스치기만 해도 두 덩어리가 하나로
                // 붙고, 붙은 덩어리는 Room Graph 가 계획한 적 없는 방들을 잇는다.
                // 6.7 이 정확히 그것을 금지한다. 그래서 점수가 아니라 **즉시
                // 폐기**다 — 후보 경로 하나를 버리는 것이 최종 검사에서 시드를
                // 통째로 버리는 것보다 훨씬 싸다.
                if (grid.Get(tile) == Tile::Floor)
                {
                    valid = false;
                    return;
                }
                for (const Int2 step : Orthogonal)
                {
                    const Int2 neighbour{ tile.x + step.x, tile.y + step.y };
                    if (CoversTile(bands, count, neighbour)) continue;
                    if (roomA.Contains(neighbour) || roomB.Contains(neighbour)) continue;
                    if (grid.Get(neighbour) == Tile::Floor)
                    {
                        valid = false;
                        return;
                    }
                }
            });

            if (!valid || ringA != 2 || ringB != 2) return false;
            // 출입구는 나란한 두 타일이어야 한다. 그 외의 모양은 Band 가 벽을
            // 비스듬히 지나 계획보다 넓은 구멍을 냈다는 뜻이다.
            if (!AreAdjacent(doorA[0], doorA[1]) || !AreAdjacent(doorB[0], doorB[1])) return false;

            score = length;
            return true;
        }

        // 방과 예약된 경계 링 사이에 타일이 몇 개 있는지, 네 면 각각.
        // 색인 순서는 아래 OutwardStep 과 같다.
        std::array<int, 4> BorderDistances(const Grid& grid, const Rect& room)
        {
            return {
                room.x - 1,                    // left
                grid.Width() - 1 - room.Right(),   // right
                room.y - 1,                    // top
                grid.Height() - 1 - room.Bottom() };  // bottom
        }

        // 면 번호를 바깥을 향하는 한 걸음으로. 순서는 위 BorderDistances 와 같다.
        Int2 OutwardStep(int side)
        {
            const std::array<Int2, 4> steps = {
                Int2{ -1, 0 }, Int2{ 1, 0 }, Int2{ 0, -1 }, Int2{ 0, 1 } };
            return steps[static_cast<std::size_t>(side)];
        }

        // `from` 에서 `step` 방향으로 곧게 뻗는 벽 타일 줄. 첫 바닥 타일에서 끝난다.
        // 벽 타일을 순서대로 돌려주고, 줄이 너무 길거나 맵을 벗어나거나 벽 선이
        // 옆으로 새면 빈 목록이다.
        //
        // 바뀌는 모든 타일은 줄의 **양옆이 단단한 벽**이어야 한다. 아니면 굴을 파는
        // 순간 밀폐돼 있던 쪽이 활동 구역으로 새어 나온다.
        std::vector<Int2> WallRun(const Grid& grid, Int2 from, Int2 step, int maximumLength)
        {
            const Int2 perpendicular{ step.y, step.x };
            std::vector<Int2> run;
            Int2 cursor{ from.x + step.x, from.y + step.y };
            for (int length = 0; length <= maximumLength; ++length)
            {
                if (cursor.x < 1 || cursor.y < 1
                    || cursor.x >= grid.Width() - 1 || cursor.y >= grid.Height() - 1)
                {
                    return {};
                }
                if (grid.Get(cursor) == Tile::Floor) return run;
                if (grid.Get(cursor) != Tile::Wall) return {};
                if (grid.Get({ cursor.x + perpendicular.x, cursor.y + perpendicular.y }) != Tile::Wall
                    || grid.Get({ cursor.x - perpendicular.x, cursor.y - perpendicular.y }) != Tile::Wall)
                {
                    return {};
                }
                run.push_back(cursor);
                cursor = { cursor.x + step.x, cursor.y + step.y };
            }
            return {};
        }

        // 두 타일 범위 사이의 빈 구간에 들어갈 수 있는 Band 위치 전부, 그 밖은 없다.
        // 이 구간이 두 번 꺾는 탐색을 붙들어 준다 — 맵 전체를 훑는 것이 아니라
        // 틈 안의 몇 자리다. 걷는 대신 샘플링했더니 폐기 시드가 5분의 1쯤 늘었다.
        std::vector<int> MidBandStarts(int lowA, int highA, int lowB, int highB)
        {
            int first = 0;
            int last = -1;
            if (highA < lowB)
            {
                first = highA + 1;
                last = lowB - 2;
            }
            else if (highB < lowA)
            {
                first = highB + 1;
                last = lowA - 2;
            }
            std::vector<int> starts;
            for (int value = first; value <= last; ++value) starts.push_back(value);
            return starts;
        }
    }

    // 유효한 맵이 나올 때까지 시드를 바꿔가며 최대 maxSeedAttempts 번 만든다.
    //
    // 실패해도 다른 Base Seed 로 조용히 갈아타지 않는다. 100번을 다 쓰면 무엇이
    // 몇 번 막았는지를 사유별로 찍는다.
    Grid MapGenerator::Generate(std::uint32_t seed, std::vector<MapGenerationSnapshot>* trace) const
    {
        Grid grid;
        std::map<std::string, int> reasons;
        const int attempts = std::max(1, settings.maxSeedAttempts);
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            std::vector<MapGenerationSnapshot> attemptTrace;
            grid = GenerateOnce(attempt == 0 ? seed : MixSeed(seed, attempt),
                trace ? &attemptTrace : nullptr);
            grid.seedAttempt = attempt;
            const std::string reason = Validate(grid, settings);
            if (reason.empty())
            {
                if (trace) *trace = std::move(attemptTrace);
                return grid;
            }
            ++reasons[reason];
        }

        // 다른 Base Seed 로 조용히 갈아타지 않는다. 무엇이 잘못됐는지 말한다.
        // **사유별 횟수를 전부** 찍고 마지막 시도의 사유만 찍지 않는다. 시도마다
        // 다른 규칙에서 실패하므로, 마지막 것은 이 시드를 실제로 막고 있는 규칙이
        // 아니라 우연히 마지막에 걸린 규칙의 이름이다.
        std::vector<std::pair<int, std::string>> ranked;
        for (const auto& entry : reasons) ranked.emplace_back(entry.second, entry.first);
        std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs)
        {
            if (lhs.first != rhs.first) return lhs.first > rhs.first;
            return lhs.second < rhs.second;
        });
        std::ostringstream message;
        message << "Map generation gave up after " << attempts
            << " attempts on seed " << seed << ":";
        for (const auto& entry : ranked)
        {
            message << "\n  " << entry.first << "x " << entry.second;
        }
        // 마지막 실패작도 Grid 모양은 갖추고 있어서 호출부가 실수로 플레이할 수
        // 있었다. 성공한 Grid만 반환하고, 실패는 호출 스택을 통해 명시적으로 알린다.
        throw std::runtime_error(message.str());
    }

    // 한 번의 생성 시도. 6.11 이 정한 순서를 그대로 따른다 —
    // 쿼드트리 → 역할 → 방 → Room Graph → 복도 → Start/Exit/Key → 문·창문 → 좀비·탄약.
    //
    // trace 가 있으면 단계마다 사진을 남긴다. 디버그 모드의 `[` `]` 가 그것을 넘긴다.
    Grid MapGenerator::GenerateOnce(std::uint32_t seed, std::vector<MapGenerationSnapshot>* trace) const
    {
        Grid grid(settings.width, settings.height);
        grid.seed = seed;
        std::mt19937 random(seed);

        Node root{ Rect{ 1, 1, settings.width - 2, settings.height - 2 } };
        const auto record = [this, trace, &grid](const std::string& label, const Node& tree)
        {
            if (!trace) return;
            Grid snapshot = grid;
            snapshot.leaves.clear();
            CollectLeafRegions(tree, snapshot.leaves);
            trace->push_back({ label, std::move(snapshot) });
        };

        if (trace) trace->clear();
        record("01 Empty grid", root);
        SplitIntoFour(root);
        record("02 Root split into four quadrants", root);

        // 깊이 1 사분면 넷 중 **정확히 둘**만 다시 쪼갠다. 그래서 트리는 언제나
        // 4 - 2 + 2 × 4 = 10 개의 리프로 끝난다. 개수를 고정하면 리프 예산
        // (활성 7 · 감염 2 · 예약 1)이 시드마다 흔들리지 않는다.
        std::array<int, 4> quadrantOrder = { 0, 1, 2, 3 };
        std::shuffle(quadrantOrder.begin(), quadrantOrder.end(), random);
        for (int index = 0; index < 2; ++index)
            SplitIntoFour(*root.children[static_cast<std::size_t>(quadrantOrder[static_cast<std::size_t>(index)])]);
        AssignLeafRoles(root, random);
        record("03 Adaptive quadtree: ten leaves with roles", root);
        CreateRooms(root, grid, random);
        record("04 Rooms carved inside leaves", root);
        grid.roomEdges = BuildRoomGraph(grid.rooms, random);
        // 간선 하나라도 길이 없으면 그 즉시 시드를 통째로 포기한다. 계속해봐야
        // Validate() 가 곧 버릴 맵에 A* 를 쓰는 것뿐이다.
        if (!CarveCorridors(grid)) return grid;
        record("05 Room Graph corridors carved", root);

        // 6.11 이 순서를 못 박았다 — Start / Exit / Key 가 Door / Window 보다 먼저다.
        // 그래서 열쇠 거리를 잴 때 모든 출입구가 아직 평범한 바닥이다. 문은 걸음을
        // 비싸게 만들 뿐 불가능하게 만들지 않으므로 필수 동선은 살아남는다.
        if (!PlaceStartAndExit(grid, random)) return grid;
        record("06 Start room, exit zone and player spawn", root);

        if (!PlaceKey(grid, random)) return grid;
        record("07 Key placed in the farthest qualifying room", root);

        if (!PlaceDoors(grid, random)) return grid;
        if (!PlaceBreachConnections(grid, random)) return grid;
        if (!PlaceTraversalWindow(grid, random)) return grid;
        record("08 Doors, breaches and the traversal window placed", root);

        if (!PlaceZombiesAndAmmo(grid, random)) return grid;
        record("09 Zombies and ammunition placed", root);

        return grid;
    }

    // 구역 하나를 사분면 넷으로 나눈다. 가운데에서 정확히 반씩이다.
    void MapGenerator::SplitIntoFour(Node& node) const
    {
        const int leftWidth = node.region.width / 2;
        const int topHeight = node.region.height / 2;
        const int rightWidth = node.region.width - leftWidth;
        const int bottomHeight = node.region.height - topHeight;
        const std::array<Rect, 4> regions = {
            Rect{ node.region.x, node.region.y, leftWidth, topHeight },
            Rect{ node.region.x + leftWidth, node.region.y, rightWidth, topHeight },
            Rect{ node.region.x, node.region.y + topHeight, leftWidth, bottomHeight },
            Rect{ node.region.x + leftWidth, node.region.y + topHeight, rightWidth, bottomHeight }
        };
        for (const Rect& region : regions)
        {
            auto child = std::make_unique<Node>();
            child->region = region;
            node.children.push_back(std::move(child));
        }
    }

    // 리프마다 방 하나를 RoomRegion 안에 놓고 바닥을 판다. 예약 리프는 건너뛴다.
    void MapGenerator::CreateRooms(Node& node, Grid& grid, std::mt19937& random) const
    {
        if (!node.IsLeaf())
        {
            for (auto& child : node.children)
            {
                CreateRooms(*child, grid, random);
            }
            return;
        }

        if (node.role == LeafRole::Reserved)
        {
            // 여기엔 방이 없다. 복도는 이 구역을 지나갈 수 있다.
            node.room = {};
            grid.leaves.push_back({ node.region, node.room, node.role });
            return;
        }

        // RoomRegion — 리프를 사방으로 한 타일 안쪽으로 민 것이다. 그래야 이웃한
        // 방이 벽을 공유하지 않고, 문을 놓을 자리가 언제나 남는다.
        const int margin = settings.roomMargin;
        const Rect roomRegion{
            node.region.x + margin, node.region.y + margin,
            node.region.width - margin * 2, node.region.height - margin * 2 };

        // 24×24 리프도 12×12 리프와 같은 크기 범위를 쓴다. 방은 방이지
        // "리프가 어쩌다 커진 만큼"이 아니다.
        const int widthLimit = std::min(settings.roomMaxSize, roomRegion.width);
        std::uniform_int_distribution<int> widthRoll(std::min(settings.roomMinSize, widthLimit), widthLimit);
        const int roomWidth = widthRoll(random);

        const int heightLimit = std::min({ settings.roomMaxSize, roomRegion.height,
            roomWidth + settings.roomAspectSlack });
        const int heightFloor = std::min(
            std::max(settings.roomMinSize, roomWidth - settings.roomAspectSlack), heightLimit);
        std::uniform_int_distribution<int> heightRoll(heightFloor, heightLimit);
        const int roomHeight = heightRoll(random);

        std::uniform_int_distribution<int> xRoll(0, std::max(0, roomRegion.width - roomWidth));
        std::uniform_int_distribution<int> yRoll(0, std::max(0, roomRegion.height - roomHeight));
        node.room = { roomRegion.x + xRoll(random), roomRegion.y + yRoll(random), roomWidth, roomHeight };

        // grid.rooms 는 Room Graph 의 노드 집합이므로 활성 방만 들어간다.
        // 감염·예약 리프는 grid.leaves 에서 역할로 찾는다.
        if (node.role == LeafRole::Active) grid.rooms.push_back(node.room);
        grid.leaves.push_back({ node.region, node.room, node.role });
        CarveRoom(grid, node.room);
    }

    // 활성 방들을 잇는 Room Graph. 맨해튼 가중치 Kruskal 로 신장 트리를 만들고
    // 고리 몇 개를 더한다. 결과는 Node 7 · Edge 8 이고 차수는 3 이하다.
    std::vector<RoomEdge> MapGenerator::BuildRoomGraph(
        const std::vector<Rect>& rooms, std::mt19937& random) const
    {
        const int count = static_cast<int>(rooms.size());
        std::vector<RoomEdge> chosen;
        if (count < 2) return chosen;

        struct Candidate
        {
            int weight = 0;
            int tie = 0;
            int a = 0;
            int b = 0;
        };

        // 방 중심 사이의 맨해튼 거리는 복도 길이의 근사로 충분하고, 복도가 쓰게 될
        // 4방향 이동과도 단위가 맞는다.
        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(count * (count - 1) / 2));
        std::uniform_int_distribution<int> tieRoll(0, 1 << 20);
        for (int a = 0; a < count; ++a)
        {
            for (int b = a + 1; b < count; ++b)
            {
                const Int2 centreA = rooms[static_cast<std::size_t>(a)].Center();
                const Int2 centreB = rooms[static_cast<std::size_t>(b)].Center();
                const int weight = std::abs(centreA.x - centreB.x) + std::abs(centreA.y - centreB.y);
                // 가중치가 같으면 시드 난수로 가른다. 같은 시드가 언제나 같은
                // 방식으로 동점을 푼다.
                candidates.push_back({ weight, tieRoll(random), a, b });
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs)
        {
            if (lhs.weight != rhs.weight) return lhs.weight < rhs.weight;
            return lhs.tie < rhs.tie;
        });

        // Kruskal — 서로 다른 두 덩어리를 잇는 가장 싼 간선부터 가져간다.
        std::vector<int> parent(static_cast<std::size_t>(count));
        std::iota(parent.begin(), parent.end(), 0);
        const std::function<int(int)> find = [&parent, &find](int node)
        {
            while (parent[static_cast<std::size_t>(node)] != node)
            {
                parent[static_cast<std::size_t>(node)] =
                    parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(node)])];
                node = parent[static_cast<std::size_t>(node)];
            }
            return node;
        };

        std::vector<int> degree(static_cast<std::size_t>(count), 0);
        for (const Candidate& candidate : candidates)
        {
            if (static_cast<int>(chosen.size()) >= count - 1) break;
            const int rootA = find(candidate.a);
            const int rootB = find(candidate.b);
            if (rootA == rootB) continue;
            parent[static_cast<std::size_t>(rootA)] = rootB;
            chosen.push_back({ candidate.a, candidate.b });
            ++degree[static_cast<std::size_t>(candidate.a)];
            ++degree[static_cast<std::size_t>(candidate.b)];
        }

        // 지금까지 만든 트리 위에서의 간선 거리. 추가 간선은 먼 쌍을 선호하는데,
        // 그래야 긴 우회로가 진짜 지름길로 바뀐다.
        const auto graphDistance = [&chosen, count](int from, int to)
        {
            return GraphDistances(chosen, count, from)[static_cast<std::size_t>(to)];
        };

        const auto alreadyJoined = [&chosen](int a, int b)
        {
            return std::any_of(chosen.begin(), chosen.end(), [a, b](const RoomEdge& edge)
            {
                return (edge.a == a && edge.b == b) || (edge.a == b && edge.b == a);
            });
        };

        // 3간선 떨어진 쌍을 먼저 보고, 없으면 조건을 푼다. 기획이 "선호"라고 했지
        // "요구"라고 하지 않았다. 먼 쌍에 자리가 없다고 시드를 버리지 않는다.
        for (int added = 0; added < settings.extraRoomEdges; ++added)
        {
            bool placed = false;
            for (int minimumDistance = 3; minimumDistance >= 1 && !placed; --minimumDistance)
            {
                for (const Candidate& candidate : candidates)
                {
                    if (alreadyJoined(candidate.a, candidate.b)) continue;
                    if (degree[static_cast<std::size_t>(candidate.a)] >= settings.maxRoomDegree) continue;
                    if (degree[static_cast<std::size_t>(candidate.b)] >= settings.maxRoomDegree) continue;
                    if (graphDistance(candidate.a, candidate.b) < minimumDistance) continue;
                    chosen.push_back({ candidate.a, candidate.b });
                    ++degree[static_cast<std::size_t>(candidate.a)];
                    ++degree[static_cast<std::size_t>(candidate.b)];
                    placed = true;
                    break;
                }
            }
            if (!placed) break;
        }
        return chosen;
    }

    // 방 사각형 안을 바닥으로 판다. 둘레의 벽 링은 건드리지 않는다.
    void MapGenerator::CarveRoom(Grid& grid, const Rect& room) const
    {
        for (int y = room.y; y < room.Bottom(); ++y)
        {
            for (int x = room.x; x < room.Right(); ++x)
            {
                grid.Set({ x, y }, Tile::Floor);
            }
        }
    }

    // 간선 하나를 이을 가장 좋은 복도 모양을 찾는다. L 자 두 방향을 먼저 보고,
    // 둘 다 실패하면 가운데 Band 를 낀 두 번 꺾는 모양을 본다.
    //
    // 찾으면 best 를 채우고 true. 못 찾으면 이 시드는 버려진다.
    bool MapGenerator::PlanCorridor(const Grid& grid, int edgeIndex, const RoomEdge& edge,
        const std::vector<Rect>& blockers, const std::vector<Int2>& claimed, CorridorPlan& best)
    {
        const Rect& roomA = grid.rooms[static_cast<std::size_t>(edge.a)];
        const Rect& roomB = grid.rooms[static_cast<std::size_t>(edge.b)];
        bool found = false;

        const auto consider = [&](const std::array<Rect, 3>& bands, int count)
        {
            int score = 0;
            Int2 doorA[2]{};
            Int2 doorB[2]{};
            if (!ScoreCorridor(grid, bands, count, roomA, roomB, blockers, score, doorA, doorB)) return;
            // 두 간선이 출입구 하나를 공유하면 복도 둘이 같은 구멍을 지나고,
            // 그러면 6.9 가 두 번째 간선의 문을 걸 자리가 없다. 간선마다
            // 자기 구멍을 낸다.
            for (const Int2 tile : { doorA[0], doorA[1], doorB[0], doorB[1] })
            {
                if (std::find(claimed.begin(), claimed.end(), tile) != claimed.end()) return;
            }
            if (found && score >= best.score) return;
            found = true;
            best.bands = bands;
            best.bandCount = count;
            best.score = score;
            best.doorways[0] = MakeDoorway(doorA, edgeIndex, edge.a);
            best.doorways[1] = MakeDoorway(doorB, edgeIndex, edge.b);
        };

        // 한 번 꺾는 L 자 두 방향을, 두 방이 내주는 모든 2타일 출입구 위치에 대해
        // 훑는다. 훑는 순서가 고정이고 동점이면 앞 후보를 남기므로, **생성기 난수를
        // 뽑지 않고도** 선택이 결정적이다.
        //
        // 행 Band 를 방 A 안에, 열 Band 를 방 B 안에 걸치는 것이 이 모양이 두 방에
        // 모두 닿게 만드는 장치다. 행 Band 가 A 의 폭 전체를 가로지르므로 A 의 좌우
        // 벽을 자르고, 열 Band 가 B 의 높이 전체를 가로지른다. 열 Band 가 마침 A 자신의
        // 열 범위 안에 떨어지면 행 Band 가 A 안에 묻히고 쌍이 직선 복도로 퇴화한다.
        for (int row = roomA.y; row + 1 < roomA.Bottom(); ++row)
        {
            for (int column = roomB.x; column + 1 < roomB.Right(); ++column)
            {
                std::array<Rect, 3> bands{};
                bands[0] = RowBand(std::min(roomA.x, column), std::max(roomA.Right() - 1, column + 1), row);
                bands[1] = ColumnBand(std::min(row, roomB.y), std::max(row + 1, roomB.Bottom() - 1), column);
                consider(bands, 2);
            }
        }
        for (int column = roomA.x; column + 1 < roomA.Right(); ++column)
        {
            for (int row = roomB.y; row + 1 < roomB.Bottom(); ++row)
            {
                std::array<Rect, 3> bands{};
                bands[0] = ColumnBand(std::min(roomA.y, row), std::max(roomA.Bottom() - 1, row + 1), column);
                bands[1] = RowBand(std::min(column, roomB.x), std::max(column + 1, roomB.Right() - 1), row);
                consider(bands, 2);
            }
        }
        if (found) return true;

        // L 자가 전부 실패한 뒤에만 온다 — 두 방 사이 빈 구간에 가운데 Band 를 두고
        // 양쪽에 출입구를 하나씩 붙이는 모양이다.
        for (const int column : MidBandStarts(roomA.x, roomA.Right() - 1, roomB.x, roomB.Right() - 1))
        {
            for (int rowA = roomA.y; rowA + 1 < roomA.Bottom(); ++rowA)
            {
                for (int rowB = roomB.y; rowB + 1 < roomB.Bottom(); ++rowB)
                {
                    std::array<Rect, 3> bands{};
                    bands[0] = RowBand(std::min(roomA.x, column), std::max(roomA.Right() - 1, column + 1), rowA);
                    bands[1] = ColumnBand(std::min(rowA, rowB), std::max(rowA + 1, rowB + 1), column);
                    bands[2] = RowBand(std::min(column, roomB.x), std::max(column + 1, roomB.Right() - 1), rowB);
                    consider(bands, 3);
                }
            }
        }
        for (const int row : MidBandStarts(roomA.y, roomA.Bottom() - 1, roomB.y, roomB.Bottom() - 1))
        {
            for (int columnA = roomA.x; columnA + 1 < roomA.Right(); ++columnA)
            {
                for (int columnB = roomB.x; columnB + 1 < roomB.Right(); ++columnB)
                {
                    std::array<Rect, 3> bands{};
                    bands[0] = ColumnBand(std::min(roomA.y, row), std::max(roomA.Bottom() - 1, row + 1), columnA);
                    bands[1] = RowBand(std::min(columnA, columnB), std::max(columnA + 1, columnB + 1), row);
                    bands[2] = ColumnBand(std::min(row, roomB.y), std::max(row + 1, roomB.Bottom() - 1), columnB);
                    consider(bands, 3);
                }
            }
        }
        return found;
    }

    // Room Graph 의 모든 간선에 복도를 판다. 순서가 곧 신장 트리 먼저이므로
    // 필수 연결이 자리를 먼저 잡고 추가 고리가 남은 틈으로 들어간다.
    bool MapGenerator::CarveCorridors(Grid& grid) const
    {
        // 어떤 방도 거기서 끝나지 않는 복도에게는 출입 금지다. 감염 공간도 포함이다 —
        // 6.9 에서 침입구가 열기 전까지 밀폐된 채로 있어야 한다.
        std::vector<Rect> allRooms;
        for (const LeafRegion& leaf : grid.leaves)
        {
            if (leaf.room.width > 0 && leaf.room.height > 0) allRooms.push_back(leaf.room);
        }

        // 이미 뚫은 출입구 타일. 뒤에 오는 간선이 재사용하지 못하게 한다.
        std::vector<Int2> claimed;

        // BuildRoomGraph 가 신장 트리를 먼저 넣고 추가 고리를 뒤에 붙이므로,
        // 목록을 순서대로 걷는 것만으로 MST 복도가 먼저 파인다.
        for (std::size_t index = 0; index < grid.roomEdges.size(); ++index)
        {
            const RoomEdge& edge = grid.roomEdges[index];
            const Rect& roomA = grid.rooms[static_cast<std::size_t>(edge.a)];
            const Rect& roomB = grid.rooms[static_cast<std::size_t>(edge.b)];
            std::vector<Rect> blockers;
            std::copy_if(allRooms.begin(), allRooms.end(), std::back_inserter(blockers),
                [&roomA, &roomB](const Rect& room) { return !(room == roomA) && !(room == roomB); });

            CorridorPlan plan;
            if (!PlanCorridor(grid, static_cast<int>(index), edge, blockers, claimed, plan)) return false;
            ForEachCorridorTile(plan.bands, plan.bandCount,
                [&grid](Int2 tile) { grid.Set(tile, Tile::Floor); });
            for (const Doorway& doorway : plan.doorways)
            {
                claimed.push_back(doorway.tiles.origin);
                claimed.push_back(doorway.tiles.Second());
                grid.doorways.push_back(doorway);
            }
        }
        return true;
    }

    // 시작 방과 탈출구와 플레이어 시작 자리.
    //
    // 탈출구는 **통행과 시야를 막지 않는다.** 잠김 여부는 바닥 색으로만 말한다.
    bool MapGenerator::PlaceStartAndExit(Grid& grid, std::mt19937& random) const
    {
        // 리프가 루트 경계에 닿는 방만 나가는 길을 가질 수 있다. 쪼개진 사분면의
        // 안쪽 리프는 경계에 아예 안 닿는다.
        const Rect root{ 1, 1, settings.width - 2, settings.height - 2 };
        std::vector<int> candidates;
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            const Rect& room = grid.rooms[index];
            const auto leaf = std::find_if(grid.leaves.begin(), grid.leaves.end(),
                [&room](const LeafRegion& entry) { return entry.room == room; });
            if (leaf == grid.leaves.end()) continue;
            const bool touchesBorder = leaf->region.x == root.x || leaf->region.y == root.y
                || leaf->region.Right() == root.Right() || leaf->region.Bottom() == root.Bottom();
            if (touchesBorder) candidates.push_back(static_cast<int>(index));
        }
        if (candidates.empty()) return false;

        // 맵 경계에 가장 가까운 쪽이 이긴다. 동점은 시드 난수로 가르는데, 그래야
        // 시작점이 늘 같은 사분면에 앉지 않는다.
        std::uniform_int_distribution<int> tieRoll(0, 1 << 20);
        int bestRoom = -1;
        int bestSide = 0;
        int bestDistance = 0;
        int bestTie = 0;
        for (const int index : candidates)
        {
            const std::array<int, 4> distances = BorderDistances(grid, grid.rooms[static_cast<std::size_t>(index)]);
            for (int side = 0; side < 4; ++side)
            {
                const int tie = tieRoll(random);
                if (bestRoom >= 0 && distances[static_cast<std::size_t>(side)] > bestDistance) continue;
                if (bestRoom >= 0 && distances[static_cast<std::size_t>(side)] == bestDistance
                    && tie >= bestTie)
                {
                    continue;
                }
                bestRoom = index;
                bestSide = side;
                bestDistance = distances[static_cast<std::size_t>(side)];
                bestTie = tie;
            }
        }
        if (bestRoom < 0) return false;

        const Rect& room = grid.rooms[static_cast<std::size_t>(bestRoom)];
        const Int2 outward = OutwardStep(bestSide);
        const bool verticalWall = outward.x != 0;

        // 탈출구는 그 면의 벽 링 위에 2타일 폭으로 앉고 방 밖으로 튀어나오지 않는다.
        // 시작 자리가 여기서 역산되므로 반드시 방 안에 떨어져야 한다.
        TilePair exit;
        exit.spansX = !verticalWall;
        if (verticalWall)
        {
            const int wallX = outward.x < 0 ? room.x - 1 : room.Right();
            std::uniform_int_distribution<int> roll(room.y, room.Bottom() - 2);
            exit.origin = { wallX, roll(random) };
        }
        else
        {
            const int wallY = outward.y < 0 ? room.y - 1 : room.Bottom();
            std::uniform_int_distribution<int> roll(room.x, room.Right() - 2);
            exit.origin = { roll(random), wallY };
        }

        grid.startRoom = bestRoom;
        grid.exit = exit;
        grid.Set(exit.origin, Tile::ExitLocked);
        grid.Set(exit.Second(), Tile::ExitLocked);

        // 탈출구에서 곧장 안쪽으로. 가장 작은 방이 6×6 이라 벽에서 네 타일 들어간
        // 자리는 언제나 바닥이고, 기획이 요구하는 3~5 범위에 들어간다.
        grid.playerSpawn = { exit.origin.x - outward.x * settings.playerSpawnDistance,
            exit.origin.y - outward.y * settings.playerSpawnDistance };
        return room.Contains(grid.playerSpawn);
    }

    // 열쇠를 가장 깊은 곳에 둔다. Graph 거리 3 이상인 방 중에서 탈출구 안쪽
    // 기준 A* 비용이 가장 큰 자리다. 그 비용이 minimumKeyPathCost 미만이면 시드 폐기.
    bool MapGenerator::PlaceKey(Grid& grid, std::mt19937& random) const
    {
        const std::vector<int> graphDistance =
            GraphDistances(grid.roomEdges, static_cast<int>(grid.rooms.size()), grid.startRoom);

        // 후보 타일마다 A* 를 돌리는 대신, 플레이어가 탈출구를 조작하는 타일에서
        // 다익스트라를 한 번 돌린다. FloodCosts 는 PathCost 와 타일당 같은 비용을
        // 쓰므로 나온 숫자를 최소 기준과 바로 비교할 수 있다.
        const Int2 outward{ grid.exit.origin.x - grid.playerSpawn.x,
            grid.exit.origin.y - grid.playerSpawn.y };
        const Int2 inside{ grid.exit.origin.x - (outward.x > 0 ? 1 : (outward.x < 0 ? -1 : 0)),
            grid.exit.origin.y - (outward.y > 0 ? 1 : (outward.y < 0 ? -1 : 0)) };
        const std::vector<float> costs = AStar::FloodCosts(grid, inside, CostField::Traversal);

        std::uniform_int_distribution<int> tieRoll(0, 1 << 20);
        float bestCost = -1.0f;
        int bestTie = 0;
        int bestRoom = -1;
        Int2 bestTile{};
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            // 최소 3간선 밖. 그래야 열쇠가 복도 하나 건너에 있지 않다.
            if (graphDistance[index] < 3) continue;
            const Rect& room = grid.rooms[index];
            for (int y = room.y + 1; y < room.Bottom() - 1; ++y)
            {
                for (int x = room.x + 1; x < room.Right() - 1; ++x)
                {
                    const Int2 tile{ x, y };
                    if (tile == grid.playerSpawn) continue;
                    // 6.8 은 모든 문·창문 후보에서 2타일 여유도 요구한다. 위의
                    // **1타일 안쪽 여백이 이미 그것을 지킨다** — 출입구는 벽 링 위에
                    // 있으므로 가장 가까운 후보 타일이 구조적으로 Chebyshev 2 다.
                    // 여기에 필터를 두면 아무것도 거절하지 않는다. 대신 자체
                    // 테스트가 그 여백을 지킨다.
                    const float cost = costs[static_cast<std::size_t>(tile.y * grid.Width() + tile.x)];
                    if (!std::isfinite(cost)) continue;
                    const int tie = tieRoll(random);
                    if (cost < bestCost || (cost == bestCost && tie >= bestTie)) continue;
                    bestCost = cost;
                    bestTie = tie;
                    bestRoom = static_cast<int>(index);
                    bestTile = tile;
                }
            }
        }
        // 3간선 밖에 방이 없거나, 가장 깊은 곳조차 갈 값을 못 한다. 어느 쪽이든
        // 이 시드는 고치는 것이 아니라 버린다.
        if (bestRoom < 0 || bestCost < settings.minimumKeyPathCost) return false;

        grid.keyRoom = bestRoom;
        grid.keyPosition = bestTile;
        return true;
    }

    // 좀비 열둘과 추가 탄약을 놓는다.
    //
    // **좀비를 실제로 만드는 곳은 여기가 아니다.** 여기는 자리를 정할 뿐이고,
    // 만드는 것은 Game::Reset() 한 곳뿐이다.
    bool MapGenerator::PlaceZombiesAndAmmo(Grid& grid, std::mt19937& random) const
    {
        // 필수 동선은 시작에서 열쇠까지다. 그 동선이 지나는 방에 탄약을 놓으면
        // 플레이어에게 강요하는 것이 되고, 6.10 은 그러면 안 된다고 한다.
        // **좀비는 다른 얘기다** — 아래 흩뿌리는 부분을 보라.
        const std::vector<Int2> requiredPath = AStar::FindPath(grid, grid.playerSpawn, grid.keyPosition);
        if (requiredPath.empty()) return false;

        std::vector<int> offPathRooms;
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            const Rect& room = grid.rooms[index];
            const bool onPath = std::any_of(requiredPath.begin(), requiredPath.end(),
                [&room](Int2 tile) { return room.Contains(tile); });
            if (!onPath) offPathRooms.push_back(static_cast<int>(index));
        }
        // 필수 동선 밖에 묶여 있는 것은 이제 탄약뿐이고, 두 뭉치는 방 둘이 필요하다.
        // **탄약 획득을 선택 사항으로 만드는 유일한 장치가 이 규칙이다.** 그대로 둔다.
        if (offPathRooms.size() < 2) return false;
        std::shuffle(offPathRooms.begin(), offPathRooms.end(), random);

        // 서로 2타일. 제곱 거리로 재므로 대각 이웃도 떨어진 것으로 친다.
        // Chebyshev 로 바꾸면 가장 작은 방이 감당할 수 있는 것보다 엄해진다.
        std::vector<Int2> taken;
        const auto clearOfOthers = [&taken](Int2 tile)
        {
            return std::none_of(taken.begin(), taken.end(), [tile](Int2 other)
            {
                const int dx = tile.x - other.x;
                const int dy = tile.y - other.y;
                return dx * dx + dy * dy < 4;
            });
        };

        // 침입구가 아니라 감염 **공간** 하나당이다. 공간 하나에 침입구가 둘이 된
        // 지금, 침입구당으로 세면 모든 공간의 인구가 조용히 두 배가 된다.
        for (const LeafRegion& leaf : grid.leaves)
        {
            if (leaf.role != LeafRole::Infected) continue;

            std::vector<Int2> ways;
            for (const BreachSpawn& spawn : grid.breachSpawns)
            {
                if (leaf.room.Contains(spawn.waitingPosition)) ways.push_back(spawn.breachPosition);
            }
            if (ways.empty() || ways.size() > 2) return false;
            if (ways.size() == 1) ways.push_back({ -1, -1 });

            std::vector<Int2> landings;
            for (const BreachSpawn& spawn : grid.breachSpawns)
            {
                if (leaf.room.Contains(spawn.waitingPosition)) landings.push_back(spawn.waitingPosition);
            }

            // 열쇠가 쓰는 1타일 안쪽 여백이 아니라 방 바닥 그 자체를 쓴다.
            // 6×6 안에 서로 2타일 떨어진 넷이 들어가야 한다.
            std::vector<Int2> tiles;
            for (int y = leaf.room.y; y < leaf.room.Bottom(); ++y)
            {
                for (int x = leaf.room.x; x < leaf.room.Right(); ++x) tiles.push_back({ x, y });
            }
            std::shuffle(tiles.begin(), tiles.end(), random);

            int placed = 0;
            for (const Int2 tile : tiles)
            {
                if (placed >= settings.infectedZombieCount) break;
                if (!grid.IsWalkable(tile)) continue;
                // 침입구 **둘 다**에서 떨어뜨린다. 아니면 구멍으로 나오는 첫
                // 좀비가 이미 그 구멍에 서 있는 꼴이 된다.
                const bool blocking = std::any_of(landings.begin(), landings.end(),
                    [tile](Int2 landing)
                    {
                        const int dx = tile.x - landing.x;
                        const int dy = tile.y - landing.y;
                        return dx * dx + dy * dy < 4;
                    });
                if (blocking || !clearOfOthers(tile)) continue;
                taken.push_back(tile);
                grid.zombieSpawns.push_back({ tile, ways[0], ways[1] });
                ++placed;
            }
            if (placed != settings.infectedZombieCount) return false;
        }

        // 흩어지는 쪽은 활성 방 전부에 퍼지되 둘은 뺀다 — 좀비를 향해 열리면 안 되는
        // 시작 방과, 일부러 비워 두는 방 하나다.
        //
        // **이제 필수 동선 밖 제약은 없다.** 여섯 마리가 그 동선이 안 지나는 방
        // 서너 개에 들어갈 리 없고, 6.10 도 그중 일부는 동선 위에 있기를 기대한다 —
        // "필수 경로의 좀비는 구르기와 이동으로 통과 가능해야 하고, 사살이 필수가
        // 되어서는 안 된다". 아래의 **1타일 안쪽 여백**이 이들을 출입구에서 떼어 놓는다.
        const std::vector<int> fromStart =
            GraphDistances(grid.roomEdges, static_cast<int>(grid.rooms.size()), grid.startRoom);

        // 빈 방은 최소 2간선 밖에 둔다. 그래야 바로 옆방이 되지 않는다 —
        // 걸어 들어가서 찾아낸 안전한 방이 시작하자마자 받은 방보다 값이 나간다.
        std::vector<int> safeCandidates;
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            if (fromStart[index] >= 2) safeCandidates.push_back(static_cast<int>(index));
        }
        if (safeCandidates.empty()) return false;
        std::uniform_int_distribution<std::size_t> safeRoll(0, safeCandidates.size() - 1);
        grid.safeRoom = safeCandidates[safeRoll(random)];

        std::vector<int> occupiedRooms;
        for (std::size_t index = 0; index < grid.rooms.size(); ++index)
        {
            if (static_cast<int>(index) == grid.startRoom) continue;
            if (static_cast<int>(index) == grid.safeRoom) continue;
            occupiedRooms.push_back(static_cast<int>(index));
        }
        std::shuffle(occupiedRooms.begin(), occupiedRooms.end(), random);

        // 방마다 하나씩 먼저 돌리고, 남은 것을 두 번째 패스로 돌린다. 그래야
        // 먼저 뽑힌 방에 몰리지 않고 고르게 퍼진다.
        const int looseZombies = settings.zombieCount - settings.infectedZombieCount * 2;
        int placedLoose = 0;
        for (int pass = 0; pass < settings.maxZombiesPerRoom && placedLoose < looseZombies; ++pass)
        {
            for (const int roomIndex : occupiedRooms)
            {
                if (placedLoose >= looseZombies) break;
                const Rect& room = grid.rooms[static_cast<std::size_t>(roomIndex)];
                std::vector<Int2> tiles;
                for (int y = room.y + 1; y < room.Bottom() - 1; ++y)
                {
                    for (int x = room.x + 1; x < room.Right() - 1; ++x) tiles.push_back({ x, y });
                }
                std::shuffle(tiles.begin(), tiles.end(), random);
                for (const Int2 tile : tiles)
                {
                    if (!grid.IsWalkable(tile) || tile == grid.keyPosition) continue;
                    if (!clearOfOthers(tile)) continue;
                    if (AStar::FindPath(grid, tile, grid.playerSpawn).empty()) continue;
                    taken.push_back(tile);
                    grid.zombieSpawns.push_back({ tile, { -1, -1 }, { -1, -1 } });
                    ++placedLoose;
                    break;
                }
            }
        }
        if (placedLoose != looseZombies) return false;

        // 추가 탄약을 두 뭉치로 나눠 **필수 동선 밖의 서로 다른 방**에 놓는다.
        // 뭉치는 1발과 나머지 전부다(기본 설정에서 1발과 3발).
        // 절대 필수가 아니라는 것 — "동선 밖"이 사는 것이 정확히 그것이다.
        const std::array<int, 2> bundles = { 1, settings.extraAmmoRounds - 1 };
        std::size_t bundle = 0;
        for (const int roomIndex : offPathRooms)
        {
            if (bundle >= bundles.size()) break;
            const Rect& room = grid.rooms[static_cast<std::size_t>(roomIndex)];
            std::vector<Int2> tiles;
            for (int y = room.y + 1; y < room.Bottom() - 1; ++y)
            {
                for (int x = room.x + 1; x < room.Right() - 1; ++x) tiles.push_back({ x, y });
            }
            std::shuffle(tiles.begin(), tiles.end(), random);
            for (const Int2 tile : tiles)
            {
                if (!grid.IsWalkable(tile) || tile == grid.keyPosition) continue;
                if (!clearOfOthers(tile)) continue;
                taken.push_back(tile);
                grid.ammoPickups.push_back({ tile, bundles[bundle] });
                ++bundle;
                break;
            }
        }
        return bundle == bundles.size();
    }

    // 평범한 문 넷. 폭 2타일 복도에는 1타일 문 자리가 없으므로 **출입구 위에
    // 두 타일로** 놓는다.
    bool MapGenerator::PlaceDoors(Grid& grid, std::mt19937& random) const
    {
        // 6.9 는 간선 여덟 중 넷을 골라 **한쪽 끝에만** 문을 놓는다. 그래서 문은
        // 언제나 "이쪽이 저 방으로 들어가는 길"을 뜻하지, 복도를 양끝에서 봉하는
        // 것이 아니다. 한 방이 문을 셋 이상 갖지 않는다.
        std::vector<int> edgeOrder(grid.roomEdges.size());
        std::iota(edgeOrder.begin(), edgeOrder.end(), 0);
        std::shuffle(edgeOrder.begin(), edgeOrder.end(), random);

        std::vector<int> doorsInRoom(grid.rooms.size(), 0);
        std::bernoulli_distribution pickFirst(0.5);
        int placed = 0;
        for (const int edge : edgeOrder)
        {
            if (placed >= settings.doorCount) break;

            std::vector<const Doorway*> ends;
            for (const Doorway& doorway : grid.doorways)
            {
                if (doorway.edge == edge) ends.push_back(&doorway);
            }
            if (ends.size() != 2) continue;
            if (!pickFirst(random)) std::swap(ends[0], ends[1]);

            for (const Doorway* doorway : ends)
            {
                if (doorsInRoom[static_cast<std::size_t>(doorway->room)] >= settings.maxDoorsPerRoom) continue;
                const Int2 first = doorway->tiles.origin;
                const Int2 second = doorway->tiles.Second();
                if (grid.Get(first) != Tile::Floor || grid.Get(second) != Tile::Floor) continue;
                grid.Set(first, Tile::DoorClosed);
                grid.Set(second, Tile::DoorClosed);
                ++doorsInRoom[static_cast<std::size_t>(doorway->room)];
                ++placed;
                break;
            }
        }
        return placed == settings.doorCount;
    }

    // 감염 공간을 활동 구역에 잇는 침입구와, 그 뒤의 대기 자리.
    //
    // 침입구는 1타일 주머니가 아니라 WallRun() 이 판 짧은 굴이다.
    bool MapGenerator::PlaceBreachConnections(Grid& grid, std::mt19937& random) const
    {
        std::vector<LeafRegion> infected;
        for (const LeafRegion& leaf : grid.leaves)
        {
            if (leaf.role == LeafRole::Infected) infected.push_back(leaf);
        }
        if (infected.size() != 2) return false;

        // 안쪽 리프의 감염 공간은 두 방향으로 열린다 — 막힌 문 하나와 창문 하나.
        // 그래야 구석 주머니가 아니라 두 동선 사이에 놓인다. 나머지 하나는 6.9 의
        // 막힌 문 하나를 그대로 지킨다.
        const Rect rootRegion{ 1, 1, settings.width - 2, settings.height - 2 };
        std::bernoulli_distribution doorFirst(0.5);

        for (const LeafRegion& leaf : infected)
        {
            const Rect& room = leaf.room;
            const bool reachesBorder = leaf.region.x == rootRegion.x
                || leaf.region.y == rootRegion.y
                || leaf.region.Right() == rootRegion.Right()
                || leaf.region.Bottom() == rootRegion.Bottom();
            const int wanted = reachesBorder ? 1 : settings.breachesPerInfectedSpace;
            // 공간 하나당 한 번 굴린다. 침입구마다 굴렸더니 한 공간이 같은 종류를
            // 두 번 뽑아서 막힌 문이나 창문 한쪽을 잃었다.
            const bool sealedFirst = doorFirst(random);
            // 도달 가능성은 가정하지 않고 **확인한다.** 이미 판 침입구마다 그 뒤에
            // 밀폐된 바닥이 남는데, 거기서 굴을 시작하면 감염 공간 둘이 활동 구역이
            // 아니라 서로 이어진다. 앞의 침입구가 맵을 바꿔놨으므로 침입구마다
            // 다시 계산한다.
            struct Route
            {
                std::vector<Int2> run;
                Int2 landing{};
                Int2 step{};
            };
            std::vector<Route> chosen;
            for (int slot = 0; slot < wanted; ++slot)
            {
                const std::vector<float> reachable =
                    AStar::FloodCosts(grid, grid.playerSpawn, CostField::Traversal);
                Route best;
                for (int y = 1; y < grid.Height() - 1; ++y)
                {
                    for (int x = 1; x < grid.Width() - 1; ++x)
                    {
                        const Int2 outside{ x, y };
                        if (grid.Get(outside) != Tile::Floor || room.Contains(outside)) continue;
                        if (!std::isfinite(reachable[static_cast<std::size_t>(y * grid.Width() + x)])) continue;
                        for (const Int2 step : Orthogonal)
                        {
                            // 같은 면에 두 번째 침입구를 뚫는 것은 같은 통로를
                            // 두 번 만드는 것이다. 서로 다른 면이라야 그 공간이
                            // 두 동선 사이에 놓인다.
                            const bool sameSide = std::any_of(chosen.begin(), chosen.end(),
                                [step](const Route& route) { return route.step == step; });
                            if (sameSide) continue;
                            const std::vector<Int2> run =
                                WallRun(grid, outside, step, settings.breachConnectionLength);
                            if (run.empty()) continue;
                            const Int2 landing{ run.back().x + step.x, run.back().y + step.y };
                            if (!room.Contains(landing)) continue;
                            if (!best.run.empty() && run.size() >= best.run.size()) continue;
                            best = { run, landing, step };
                        }
                    }
                }
                if (best.run.empty()) return false;

                // 침입구는 활동 구역 쪽 끝에 둔다. 그래야 접근 타일이 감염 쪽에
                // 남고 대기 좀비가 바로 그 뒤에 모인다.
                // 한 번만 열리는 공간은 언제나 막힌 문으로 열린다 — **권총이 그것을
                // 지나가는 수단이 되어서는 안 된다.**
                const Tile breachTile = wanted == 1 || (slot == 0) == sealedFirst
                    ? Tile::DoorSealed : Tile::WindowIntact;
                grid.Set(best.run.front(), breachTile);
                for (std::size_t step = 1; step < best.run.size(); ++step)
                {
                    grid.Set(best.run[step], Tile::Floor);
                    grid.narrowTiles.push_back(best.run[step]);
                }

                // 막혀 있을 때는 격리해야 하고, 부서졌을 때는 이어야 한다.
                if (!AStar::FindPath(grid, grid.playerSpawn, best.landing).empty()) return false;
                Grid opened = grid;
                opened.Set(best.run.front(), Tile::DoorBroken);
                if (AStar::FindPath(opened, best.landing, grid.playerSpawn).empty()) return false;

                grid.breachSpawns.push_back({ best.run.front(), best.landing });
                chosen.push_back(best);
            }

            // 양방향 공간은 종류를 하나씩 갖는다. 같은 것 둘은 안 된다.
            int sealedDoors = 0;
            for (const Route& route : chosen)
            {
                if (grid.Get(route.run.front()) == Tile::DoorSealed) ++sealedDoors;
            }
            if (sealedDoors != 1) return false;
        }
        return true;
    }

    // 통과 창문 정확히 하나. 플레이어가 쏘라고 만든 유일한 창문이다.
    bool MapGenerator::PlaceTraversalWindow(Grid& grid, std::mt19937& random) const
    {
        // 플레이어가 쏘라고 만든 유일한 창문. Room Graph 가 멀리 떼어놓은 활성 방
        // 둘을 잇는다. 그래서 깨는 것이 진짜 지름길이지, 이미 옆에 있던 복도로
        // 나가는 두 번째 문이 아니다.
        struct Candidate
        {
            std::vector<Int2> run;
            Int2 landing{};
        };

        // 3간선 떨어져 있어야 지름길이 갈 값을 한다. 다만 6.9 는 "선호"라고 했지
        // "요구"라고 하지 않았다. 먼 쌍이 마침 벽 네 타일 안에 없다고 시드를 버리지
        // 않는다. 추가 Room Graph 간선과 같은 방식으로 조건을 푼다.
        std::vector<Candidate> candidates;
        for (int minimumDistance = 3; minimumDistance >= 1 && candidates.empty(); --minimumDistance)
        {
            for (std::size_t index = 0; index < grid.rooms.size(); ++index)
            {
                const std::vector<int> distance = GraphDistances(
                    grid.roomEdges, static_cast<int>(grid.rooms.size()), static_cast<int>(index));
                const Rect& room = grid.rooms[index];
                for (std::size_t other = 0; other < grid.rooms.size(); ++other)
                {
                    if (other == index || distance[other] < minimumDistance) continue;
                    const Rect& target = grid.rooms[other];
                    for (int y = room.y; y < room.Bottom(); ++y)
                    {
                        for (int x = room.x; x < room.Right(); ++x)
                        {
                            for (const Int2 step : Orthogonal)
                            {
                                const std::vector<Int2> run =
                                    WallRun(grid, { x, y }, step, settings.breachConnectionLength);
                                if (run.empty()) continue;
                                const Int2 landing{ run.back().x + step.x, run.back().y + step.y };
                                if (!target.Contains(landing)) continue;
                                candidates.push_back({ run, landing });
                            }
                        }
                    }
                }
            }
        }
        if (candidates.empty()) return false;

        // 짧은 것부터, 같은 길이끼리는 시드 난수로 하나 고른다.
        const std::size_t shortest = std::min_element(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) { return lhs.run.size() < rhs.run.size(); })->run.size();
        std::vector<const Candidate*> best;
        for (const Candidate& candidate : candidates)
        {
            if (candidate.run.size() == shortest) best.push_back(&candidate);
        }
        std::uniform_int_distribution<std::size_t> pick(0, best.size() - 1);
        const Candidate& chosen = *best[pick(random)];

        grid.Set(chosen.run.front(), Tile::WindowIntact);
        for (std::size_t step = 1; step < chosen.run.size(); ++step)
        {
            grid.Set(chosen.run[step], Tile::Floor);
            grid.narrowTiles.push_back(chosen.run[step]);
        }
        // **일부러 BreachSpawn 에 넣지 않는다.** 뒤에 기다리는 것이 없으므로 침입
        // 압력을 모으면 안 된다. 넣으면 아무도 없는 창문이 압력을 빨아들인다.
        // BreachSystem::Initialize() 는 breachSpawns 만 순회한다.
        return true;
    }

    // 트리를 훑어 리프의 구역·방·역할을 목록으로 뽑는다. Grid::leaves 가 된다.
    void MapGenerator::CollectLeafRegions(const Node& node, std::vector<LeafRegion>& regions) const
    {
        if (node.IsLeaf())
        {
            regions.push_back({ node.region, node.room, node.role });
            return;
        }
        for (const auto& child : node.children)
        {
            CollectLeafRegions(*child, regions);
        }
    }

    // 트리를 훑어 리프 노드 포인터를 모은다. 역할 배정이 이 목록을 고친다.
    void MapGenerator::CollectLeafNodes(Node& node, std::vector<Node*>& leaves)
    {
        if (node.IsLeaf())
        {
            leaves.push_back(&node);
            return;
        }
        for (auto& child : node.children)
        {
            CollectLeafNodes(*child, leaves);
        }
    }

    // 리프 열 개에 역할을 나눠 준다 — 활성 7, 감염 2, 예약 1.
    void MapGenerator::AssignLeafRoles(Node& root, std::mt19937& random) const
    {
        // 깊이 1 사분면마다 활성 방이 최소 하나는 나와야 한다. 그래서 안 쪼개진 둘은
        // 정의상 활성이다 — 리프가 하나뿐이니까. 나머지는 쪼개진 사분면 안의 리프
        // 여덟 개가 나눠 갖는다.
        //
        // 감염 공간 하나는 **서로 다른 두 면**에서 뚫린다. 그런데 맵 경계에 닿는
        // 리프는 두 면이 아무것도 없는 바깥을 향한다. 쪼개진 사분면마다 경계에 닿지
        // 않는 리프가 정확히 하나 있고 — 맵 한가운데에 가장 가까운 것 — 양방향
        // 공간은 거기로 간다.
        //
        // **둘 중 하나만 그러면 된다.** 플레이 테스트 2026-08-27: 방들 사이에 낀
        // 감염 공간은 반드시 상대해야 하는 자리이고, 하나면 그 역할에 충분하다.
        // 둘 다 요구했더니 시드가 100회 전부 죽었다.
        const Rect rootRegion = root.region;
        const auto reachesBorder = [&rootRegion](const Node* leaf)
        {
            return leaf->region.x == rootRegion.x || leaf->region.y == rootRegion.y
                || leaf->region.Right() == rootRegion.Right()
                || leaf->region.Bottom() == rootRegion.Bottom();
        };

        std::vector<Node*> free;
        std::vector<Node*> interior;
        for (auto& quadrant : root.children)
        {
            std::vector<Node*> leaves;
            CollectLeafNodes(*quadrant, leaves);
            if (leaves.size() == 1)
            {
                leaves.front()->role = LeafRole::Active;
                continue;
            }
            std::shuffle(leaves.begin(), leaves.end(), random);

            std::vector<Node*> bordering;
            for (Node* leaf : leaves)
            {
                if (reachesBorder(leaf)) bordering.push_back(leaf);
                else interior.push_back(leaf);
            }
            // 쪼개진 사분면마다 활성 리프 하나를 보장한다. 그래야 깊이 1 구역 넷이
            // 전부 방을 갖는다. 경계에 닿는 리프에서 뽑는데, 안쪽 리프는 이미
            // 양방향 감염 공간에 배정됐을 수 있기 때문이다.
            if (bordering.empty()) continue;
            bordering.front()->role = LeafRole::Active;
            free.insert(free.end(), bordering.begin() + 1, bordering.end());
        }

        std::shuffle(interior.begin(), interior.end(), random);
        Node* twoWay = interior.empty() ? nullptr : interior.front();
        if (twoWay) twoWay->role = LeafRole::Infected;
        free.insert(free.end(), interior.begin() + (twoWay ? 1 : 0), interior.end());

        std::shuffle(free.begin(), free.end(), random);
        std::size_t index = 0;
        for (std::size_t infectedLeft = twoWay ? 1 : 2;
            infectedLeft > 0 && index < free.size(); --infectedLeft, ++index)
        {
            free[index]->role = LeafRole::Infected;
        }
        for (; index + 1 < free.size(); ++index) free[index]->role = LeafRole::Active;
        for (; index < free.size(); ++index) free[index]->role = LeafRole::Reserved;
    }
}
