#include "AStar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

namespace Zombie
{
    namespace
    {
        // 우선순위 큐에 들어가는 한 칸. score 가 작을수록 먼저 나온다.
        struct OpenNode
        {
            float score = 0.0f;
            int index = 0;
            bool operator>(const OpenNode& rhs) const { return score > rhs.score; }
        };

        // 남은 거리의 낙관적 추정. 4방향 이동이라 맨해튼 거리가 정확히 하한이고,
        // 하한이어야 A* 가 최단 경로를 보장한다.
        float Heuristic(Int2 a, Int2 b)
        {
            return static_cast<float>(std::abs(a.x - b.x) + std::abs(a.y - b.y));
        }

        // 한 칸 들어가는 값. 두 비용을 겸용하면 안 되는 이유는 Grid.cpp 의 표에 있다.
        float StepCost(const Grid& grid, Int2 point, CostField field)
        {
            return field == CostField::Acoustic ? grid.AcousticCost(point) : grid.TraversalCost(point);
        }

        // 들어갈 수 있는 칸인가.
        //
        // 이 분기를 지우면 소리가 벽을 못 넘는다. 그러면 밀폐된 침입구가 영영 안 깨지고,
        // 그 뒤에서 기다리던 좀비들이 한 판 내내 안 나온다.
        bool CanEnter(const Grid& grid, Int2 point, CostField field)
        {
            return field == CostField::Acoustic ? grid.Contains(point) : grid.IsWalkable(point);
        }

        struct SearchResult
        {
            std::vector<float> costs;
            std::vector<int> parents;
        };

        // 공개된 진입점 둘이 같이 쓰는 탐색 코어.
        //
        // 목표가 있으면 A* 다 — 휴리스틱이 방향을 잡고 도착하면 멈춘다. 목표가 없으면
        // 휴리스틱이 0 이고, 그게 정확히 다익스트라다. 도달 가능한 모든 타일이 채워진다.
        SearchResult Search(const Grid& grid, Int2 start, Int2 goal, bool hasGoal, CostField field)
        {
            const int width = grid.Width();
            const int count = width * grid.Height();
            const float infinity = std::numeric_limits<float>::infinity();
            SearchResult result{
                std::vector<float>(static_cast<std::size_t>(count), infinity),
                std::vector<int>(static_cast<std::size_t>(count), -1) };
            if (!grid.Contains(start))
            {
                return result;
            }

            const int startIndex = start.y * width + start.x;
            const int goalIndex = hasGoal ? goal.y * width + goal.x : -1;
            std::vector<bool> closed(static_cast<std::size_t>(count), false);
            std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<>> open;

            result.costs[static_cast<std::size_t>(startIndex)] = 0.0f;
            open.push({ hasGoal ? Heuristic(start, goal) : 0.0f, startIndex });

            constexpr std::array<Int2, 4> directions = { Int2{ 1, 0 }, Int2{ -1, 0 }, Int2{ 0, 1 }, Int2{ 0, -1 } };
            while (!open.empty())
            {
                const int currentIndex = open.top().index;
                open.pop();
                // std::priority_queue 에는 decrease-key 가 없다. 더 싼 경로를 찾으면
                // 같은 타일을 그냥 다시 push 하고, 낡은 사본은 큐에서 지우는 대신
                // 여기서 버린다. 이 줄이 없으면 이미 확정된 타일을 다시 펼쳐
                // 잘못된 부모가 남는다.
                if (closed[static_cast<std::size_t>(currentIndex)])
                {
                    continue;
                }
                if (currentIndex == goalIndex)
                {
                    break;
                }
                closed[static_cast<std::size_t>(currentIndex)] = true;
                const Int2 current{ currentIndex % width, currentIndex / width };

                for (const Int2 direction : directions)
                {
                    const Int2 next{ current.x + direction.x, current.y + direction.y };
                    if (!CanEnter(grid, next, field))
                    {
                        continue;
                    }
                    const int nextIndex = next.y * width + next.x;
                    const float candidate = result.costs[static_cast<std::size_t>(currentIndex)]
                        + StepCost(grid, next, field);
                    if (candidate < result.costs[static_cast<std::size_t>(nextIndex)])
                    {
                        result.costs[static_cast<std::size_t>(nextIndex)] = candidate;
                        result.parents[static_cast<std::size_t>(nextIndex)] = currentIndex;
                        open.push({ candidate + (hasGoal ? Heuristic(next, goal) : 0.0f), nextIndex });
                    }
                }
            }
            return result;
        }
    }

    // 시작과 목표 사이의 타일 목록. 부모 배열을 목표에서 거꾸로 타고 올라가 뒤집는다.
    //
    // 양 끝을 먼저 검사하는 것은 비용 문제가 아니라 의미 문제다. 통행 불가 타일에서
    // 출발하거나 그런 타일을 목표로 삼는 것은 경로가 없는 것이지 경로가 긴 것이 아니다.
    std::vector<Int2> AStar::FindPath(const Grid& grid, Int2 start, Int2 goal)
    {
        if (!grid.Contains(start) || !grid.Contains(goal) || !grid.IsWalkable(start) || !grid.IsWalkable(goal))
        {
            return {};
        }

        const int width = grid.Width();
        const int startIndex = start.y * width + start.x;
        const int goalIndex = goal.y * width + goal.x;
        const SearchResult search = Search(grid, start, goal, true, CostField::Traversal);

        // 시작과 목표가 같은 칸이면 부모가 없는 것이 정상이다. 그 경우를 빼고
        // 부모가 없다는 것은 도달하지 못했다는 뜻이다.
        if (startIndex != goalIndex && search.parents[static_cast<std::size_t>(goalIndex)] < 0)
        {
            return {};
        }

        std::vector<Int2> path;
        for (int index = goalIndex; index >= 0; index = search.parents[static_cast<std::size_t>(index)])
        {
            path.push_back({ index % width, index / width });
            if (index == startIndex)
            {
                break;
            }
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    // 경로가 치르는 비용의 합.
    //
    // 색인이 1부터인 것은 출발 칸에 들어가는 값을 내지 않기 때문이다. 이미 거기 서 있다.
    float AStar::PathCost(const Grid& grid, const std::vector<Int2>& path)
    {
        float total = 0.0f;
        for (std::size_t index = 1; index < path.size(); ++index)
        {
            total += grid.TraversalCost(path[index]);
        }
        return total;
    }

    // 한 지점에서 퍼지는 비용 필드. 목표를 출발점과 같게 주고 hasGoal 을 끄면
    // 휴리스틱이 0 이 되어 다익스트라가 된다.
    std::vector<float> AStar::FloodCosts(const Grid& grid, Int2 source, CostField field)
    {
        return Search(grid, source, source, false, field).costs;
    }
}
