#include "MapGeneratorDetail.h"

namespace Zombie::MapDetail
{
    // Room Graph 위의 너비 우선 탐색. 간선 목록이 짧아서(Edge 8개) 인접 리스트를
    // 만들지 않고 매 단계 전체 간선을 훑는다. 자료구조를 세우는 값이 더 비싸다.
    std::vector<int> GraphDistances(const std::vector<RoomEdge>& edges, int count, int from)
    {
        std::vector<int> distance(static_cast<std::size_t>(count), -1);
        if (from < 0 || from >= count) return distance;
        distance[static_cast<std::size_t>(from)] = 0;
        std::vector<int> frontier{ from };
        while (!frontier.empty())
        {
            std::vector<int> next;
            for (const int node : frontier)
            {
                for (const RoomEdge& edge : edges)
                {
                    // 이 간선이 현재 노드에 붙어 있으면 반대쪽 끝, 아니면 -1.
                    const int other = edge.a == node ? edge.b : (edge.b == node ? edge.a : -1);
                    // 이미 거리가 정해진 노드는 건너뛴다. 너비 우선이라 먼저 정해진
                    // 값이 곧 최단 거리다.
                    if (other < 0 || distance[static_cast<std::size_t>(other)] >= 0) continue;
                    distance[static_cast<std::size_t>(other)] =
                        distance[static_cast<std::size_t>(node)] + 1;
                    next.push_back(other);
                }
            }
            frontier.swap(next);
        }
        return distance;
    }
}
