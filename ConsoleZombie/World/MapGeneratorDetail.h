#pragma once

#include <World/Grid.h>
#include <array>
#include <cstddef>
#include <vector>

// 생성기와 검증기가 둘 다 필요로 하는 몇 안 되는 것들.
//
// 공개 인터페이스가 아니다. MapGenerator.cpp 와 MapValidation.cpp 만 포함한다.
// 검증기를 생성기에서 떼어내면서 이것들에 호출자가 둘이 됐고, 양쪽에 복사해 두면
// 그게 둘이 조용히 어긋나는 첫걸음이 된다.
namespace Zombie::MapDetail
{
    // 직교 네 방향. 생성기가 언제나 걸어온 순서 그대로다.
    //
    // 타일 홍수 채우기와 Room Graph 읽기가 둘 다 이 순서에 기대어 같은 결과를 낸다.
    // 순서를 바꾸면 맵 해시가 움직인다.
    inline constexpr std::array<Int2, 4> Orthogonal = {
        Int2{ 1, 0 }, Int2{ -1, 0 }, Int2{ 0, 1 }, Int2{ 0, -1 } };

    // Room Graph 위에서 한 방으로부터 다른 모든 방까지의 홉 수. 못 가면 -1 이다.
    //
    // 열쇠 배치가 "몇 Edge 이상 떨어진 방"을 찾을 때 쓰고, 검증기가 실제로 거기
    // 놓였는지 확인할 때도 쓴다. 둘이 같은 질문을 한다.
    std::vector<int> GraphDistances(const std::vector<RoomEdge>& edges, int count, int from);
}
