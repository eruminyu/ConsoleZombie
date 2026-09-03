#include "Grid.h"

#include <iterator>

namespace Zombie
{
    namespace
    {
        // 통행 불가를 뜻하는 비용. 0 이나 음수가 아니라 아주 큰 수인 이유는 A* 가
        // 비용을 더하며 도는 알고리즘이기 때문이다. "못 간다"를 특수값으로 두면
        // 더하는 쪽마다 그 값을 알아야 한다.
        constexpr float Blocked = 1.0e9f;

        // 타일 하나에 대해 게임이 아는 것 전부.
        //
        // Grid 는 타일 동작을 한곳에서 정하는 "게임의 헌법"이다. 이 규칙이 예전에는
        // switch 와 || 사슬 일곱 조각으로 흩어져 있었다. "이 타일이 어떻게
        // 동작하나"를 알려면 일곱 곳을 봐야 했고, 타일을 하나 추가하면 일곱 곳을
        // 고쳐야 했다. 하나를 빠뜨려도 default: 가 조용히 삼켰다.
        struct TileRules
        {
            bool walkable;        // 무언가가 이 칸에 설 수 있는가
            bool opaque;          // 시야를 막는가
            float traversalCost;  // 걸어서 지나는 값. A* 가 더한다
            float acousticCost;   // 소리가 넘어가며 치르는 값. 이동과 별개다
            bool closedDoor;      // 플레이어가 E 로 열 수 있는 문
            bool sealedDoor;      // 압력으로만 열리는 문. E 를 받지 않는다
            bool sealedWindow;    // 아직 안 깨진 창문. 총알이 여는 것
        };

        // 헌법 전문. 행 하나가 타일 하나다.
        //
        // 눈에 띄는 세 칸은 일부러 그런 값이다.
        //  - DoorCracked 의 통행 비용만 2.5 다. 갈라진 문은 미는 데 덜 걸린다.
        //  - WindowBroken 은 통행 가능이고 비용 2 다. 넘는 데 값을 치른다는 뜻이다.
        //  - 막힌 문 셋은 통행 불가인데 음향 비용은 평범한 문과 같은 3 이다.
        //    이게 밀폐된 감염 공간이 바깥 소리를 듣는 유일한 경로다. 여기를 벽과
        //    같은 6 으로 적으면 그 안의 좀비들이 한 판 내내 깨어나지 않는다.
        constexpr TileRules Rules[] = {
            /* Wall              */ { false, true,  Blocked, 6.0f, false, false, false },
            /* Floor             */ { true,  false, 1.0f,    1.0f, false, false, false },
            /* DoorClosed        */ { true,  true,  3.0f,    3.0f, true,  false, false },
            /* DoorWarning       */ { true,  true,  3.0f,    3.0f, true,  false, false },
            /* DoorCracked       */ { true,  true,  2.5f,    3.0f, true,  false, false },
            /* DoorBroken        */ { true,  false, 1.0f,    1.0f, false, false, false },
            /* DoorOpen          */ { true,  false, 1.0f,    1.0f, false, false, false },
            /* DoorSealed        */ { false, true,  Blocked, 3.0f, false, true,  false },
            /* DoorSealedWarning */ { false, true,  Blocked, 3.0f, false, true,  false },
            /* DoorSealedCracked */ { false, true,  Blocked, 3.0f, false, true,  false },
            /* WindowIntact      */ { false, true,  Blocked, 4.0f, false, false, true  },
            /* WindowWarning     */ { false, true,  Blocked, 4.0f, false, false, true  },
            /* WindowCracked     */ { false, true,  Blocked, 4.0f, false, false, true  },
            /* WindowBroken      */ { true,  false, 2.0f,    1.0f, false, false, false },
            /* ExitLocked        */ { true,  false, 1.0f,    1.0f, false, false, false },
            /* ExitOpen          */ { true,  false, 1.0f,    1.0f, false, false, false }
        };

        // 표가 열거형 전체를 덮지 않으면 빌드가 여기서 멈춘다. switch 시절에는
        // default: 가 삼켜서 안 멈췄고, 그래서 새 타일이 조용히 바닥처럼 굴었다.
        static_assert(std::size(Rules) == static_cast<std::size_t>(Tile::ExitOpen) + 1,
            "Tile 이 하나 늘었으면 이 표에도 한 줄 늘어야 한다");

        // 범위 검사를 안 하는 이유: 정의역이 열거형이고 위의 static_assert 가 표가
        // 그 전체를 덮는 것을 보장한다. 타일 값은 Grid::Set() 을 통해서만 들어오고
        // 벡터는 생성자가 Wall 로 채운다. A* 가 노드마다 부르는 자리라 없는 경우를
        // 위한 분기를 둘 이유가 없다.
        const TileRules& RulesFor(Tile tile)
        {
            return Rules[static_cast<std::size_t>(tile)];
        }
    }

    // 플레이어가 E 로 열 수 있는 문인가. 막힌 문에 이걸 참으로 만들면 열 수 없는
    // 문에 E 안내가 뜬다.
    bool IsClosedDoor(Tile tile) { return RulesFor(tile).closedDoor; }

    // 압력으로만 열리는 문인가. E 도 총알도 통하지 않는다.
    bool IsSealedDoor(Tile tile) { return RulesFor(tile).sealedDoor; }

    // 아직 안 깨진 창문인가. 총으로 깰 수 있는 것이 이것뿐이다.
    bool IsSealedWindow(Tile tile) { return RulesFor(tile).sealedWindow; }

    // 전부 벽인 격자로 시작한다. 생성기가 파내는 방식이라 빈 곳이 아니라 꽉 찬 곳이
    // 출발점이다.
    Grid::Grid(int width, int height)
        : width(width), height(height), tiles(static_cast<std::size_t>(width * height), Tile::Wall)
    {
    }

    // 격자 안의 좌표인가.
    bool Grid::Contains(Int2 point) const
    {
        return point.x >= 0 && point.y >= 0 && point.x < width && point.y < height;
    }

    // 밖은 벽으로 답한다. 호출부마다 경계를 확인하지 않아도 되게 하려는 것이고,
    // 덕분에 레이캐스트와 A* 가 격자 밖으로 새지 않는다.
    Tile Grid::Get(Int2 point) const
    {
        if (!Contains(point))
        {
            return Tile::Wall;
        }
        return tiles[static_cast<std::size_t>(point.y * width + point.x)];
    }

    // 값이 실제로 바뀔 때만 revision 을 올린다. 배치에 기대는 캐시(음향 필드)가
    // 이 숫자를 비교해서 스스로 무효화하므로, 호출부가 무효화를 기억할 필요가 없다.
    // 같은 값을 다시 써도 올라가게 두면 캐시가 매 프레임 다시 계산된다.
    void Grid::Set(Int2 point, Tile tile)
    {
        if (!Contains(point)) return;
        Tile& slot = tiles[static_cast<std::size_t>(point.y * width + point.x)];
        if (slot == tile) return;
        slot = tile;
        ++revision;
    }

    // 무언가가 설 수 있는 칸인가. 플레이어에게는 규칙이 하나 더 붙는다 —
    // Game::CanPlayerOccupy() 를 보라. 깨진 창문은 좀비는 지나고 플레이어는 E 로 넘는다.
    bool Grid::IsWalkable(Int2 point) const { return RulesFor(Get(point)).walkable; }

    // 시야를 막는 칸인가. 탈출구는 두 상태 모두 막지 않는다 — 바닥 구역이지 문이 아니다.
    // 열쇠만이 승리를 막고, 그래서 탈출구가 플레이어를 가둘 방법이 없다.
    bool Grid::IsOpaque(Int2 point) const { return RulesFor(Get(point)).opaque; }

    // 걸어서 지나는 비용.
    float Grid::TraversalCost(Int2 point) const { return RulesFor(Get(point)).traversalCost; }

    // 소리가 넘어가는 비용. 이동 비용과 겸용하면 안 된다 — 벽이 Blocked 라
    // 소리가 완전히 차단되고, 밀폐된 침입구가 영영 안 깨진다.
    float Grid::AcousticCost(Int2 point) const { return RulesFor(Get(point)).acousticCost; }

    // 타일 배열만 접은 64비트 값. 회귀를 잡는 맵 해시가 이것이다. 시드나 재시도
    // 횟수 같은 메타데이터는 일부러 빼서, 같은 지형이면 같은 값이 나오게 한다.
    std::uint64_t Grid::StableHash() const
    {
        std::uint64_t hash = 1469598103934665603ull;
        for (const Tile tile : tiles)
        {
            hash ^= static_cast<std::uint8_t>(tile);
            hash *= 1099511628211ull;
        }
        return hash;
    }
}
