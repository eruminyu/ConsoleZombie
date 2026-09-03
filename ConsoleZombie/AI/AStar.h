#pragma once

#include <World/Grid.h>
#include <vector>

namespace Zombie
{
    // 탐색이 무슨 비용을 치르고 어디까지 들어갈 수 있는가.
    //
    // 이 열거형 하나가 같은 탐색 코어를 길찾기와 음향 필드 둘로 쓰게 해준다.
    enum class CostField
    {
        Traversal,  // 이동 비용. 통행 불가 타일에는 못 들어간다
        Acoustic    // 음향 비용. 벽에도 들어가되 비싸게 치른다
    };

    // 4방향 탐색 하나로 경로와 음향 필드를 둘 다 처리한다.
    //
    // 다른 알고리즘으로 갈아끼우지 말 것. 확장해서 재사용한다.
    class AStar
    {
    public:
        // start 에서 goal 까지의 타일 목록. 못 가면 빈 목록이다.
        static std::vector<Int2> FindPath(const Grid& grid, Int2 start, Int2 goal);

        // 경로가 실제로 치르는 비용의 합. 열쇠를 얼마나 깊이 둘지가 이 값으로 정해진다.
        static float PathCost(const Grid& grid, const std::vector<Int2>& path);

        // 다익스트라 — 휴리스틱을 0으로 고정한 같은 탐색이다. 그러면 사방으로 고르게
        // 퍼지면서 모든 타일까지의 비용을 한 번에 돌려준다. 못 가는 타일은 무한대로
        // 남는다. 색인은 y * grid.Width() + x 다.
        static std::vector<float> FloodCosts(const Grid& grid, Int2 source, CostField field);
    };
}
