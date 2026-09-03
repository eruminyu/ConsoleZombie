#pragma once

#include <World/Grid.h>
#include <array>
#include <memory>
#include <random>
#include <string>

namespace Zombie
{
    // 생성기가 지키는 구조 규칙들. 밸런스 파일이 덮어쓰는 것은 아래 몇 개뿐이고
    // 나머지는 기획 6절이 정한 형태 자체다.
    struct MapGenerationSettings
    {
        int width = 48;
        int height = 48;
        // RoomRegion 은 리프를 사방으로 이만큼 안쪽으로 민 것이다.
        int roomMargin = 1;
        // 방 바닥 크기와, 가로세로가 벌어져도 되는 정도.
        int roomMinSize = 6;
        int roomMaxSize = 10;
        int roomAspectSlack = 3;
        // Room Graph — 활성 방들을 잇는 신장 트리에 고리 몇 개를 더한 것.
        int extraRoomEdges = 2;
        int maxRoomDegree = 3;
        // 구조 규칙을 어긴 시드는 버리고 다시 굴린다.
        int maxSeedAttempts = 100;
        // 플레이어가 탈출구에서 얼마나 떨어져 시작해야 하는지, 그리고 열쇠가 얼마나
        // 깊이 있어야 하는지. 가장 깊은 열쇠 방이 이보다 싸면 그 시드는 버린다.
        int playerSpawnDistance = 4;
        float minimumKeyPathCost = 40.0f;
        // 6.9 는 이것들을 예산이 아니라 **개수**로 못 박았다. 간선 여덟 중 넷에 문이
        // 하나씩, 감염 공간마다 침입구가 정확히 하나, 맵 전체에 통과 창문이 하나.
        int doorCount = 4;
        int maxDoorsPerRoom = 2;
        // 6.9 의 4 가 아니라 6 이다. 첫 침입구는 가장 짧은 길로 나가지만, 같은 방의
        // 다른 면에서 나가는 두 번째는 대개 더 멀리 가야 한다. 4 로 붙들었더니
        // 생성기가 쓸 시드가 말라붙었다.
        int breachConnectionLength = 6;
        int traversalWindowCount = 1;
        // 6.10 의 좀비 열둘. 감염 공간 두 곳에 셋씩 자고, 나머지 여섯이 방들에
        // 흩어진다. 추가 탄약은 두 뭉치로 나뉜다 (아래 extraAmmoRounds).
        int zombieCount = 12;
        // 감염 공간 하나당 셋. 그래서 맵 좀비의 절반이 밖을 돌아다닌다.
        // 플레이 테스트 2026-08-27: 대부분을 가둬놨더니 한 판이 너무 조용했다.
        int infectedZombieCount = 3;
        int maxZombiesPerRoom = 2;
        // 감염 공간마다 나가는 길이 둘, 서로 다른 면에. 그래야 구석 주머니가 아니라
        // 두 동선 사이에 놓인다.
        int breachesPerInfectedSpace = 2;
        // 맵 전체 탄약 예산은 startAmmo + extraAmmoRounds = 8 발이다.
        // 추가분은 1발과 3발 두 뭉치로 놓인다 (1, extraAmmoRounds - 1).
        //
        // 자체 테스트는 이 구조체 기본값으로 훑고 게임은 밸런스 파일로 설정을 만든다.
        // **그래서 이 값이 ini 와 어긋나면 아무도 안 하는 게임을 검증하게 된다.**
        int extraAmmoRounds = 4;
        int startAmmo = 4;
    };

    // 생성 도중 한 단계의 사진. 디버그 모드의 `[` `]` 가 이 목록을 넘긴다.
    struct MapGenerationSnapshot
    {
        std::string label;
        Grid grid;
    };

    // 쿼드트리 절차 생성. 만드는 쪽이다. 읽고 판정만 하는 쪽은 MapValidation.cpp 에 있다.
    class MapGenerator
    {
    public:
        explicit MapGenerator(MapGenerationSettings settings = {});
        // 검증을 통과한 Grid만 반환한다. 제한 횟수를 모두 쓰면 사유별 집계를 담은
        // std::runtime_error를 던지고, trace도 성공한 시도에서만 교체한다.
        Grid Generate(std::uint32_t seed, std::vector<MapGenerationSnapshot>* trace = nullptr) const;

        // 이 Grid가 왜 거절됐는지. 맵이 유효하면 빈 문자열이다.
        static std::string Validate(const Grid& grid, const MapGenerationSettings& settings);

        // 완성된 타일이 실제로 잇고 있는 방 쌍. **계획을 믿는 것이 아니라 맵에서
        // 되읽는다.** 통행 가능한 비-방 바닥의 한 연결 덩어리가 두 방에 닿아 있으면
        // 그 둘이 인접이다. 정렬돼 있고 모든 항목에서 a < b 다.
        static std::vector<RoomEdge> ExtractRoomAdjacency(const Grid& grid);

    private:
        struct Node
        {
            Rect region{};
            Rect room{};
            LeafRole role = LeafRole::Reserved;
            std::vector<std::unique_ptr<Node>> children;
            bool IsLeaf() const { return children.empty(); }
        };

        // 복도는 Band 두세 개가 이어진 사슬이고 Band 하나가 정확히 폭 2타일이다.
        // 이웃한 Band 는 꺾이는 곳에서 2×2 블록을 공유하는데, 그게 모퉁이에서 폭을
        // 1로 조이지 않고 2로 붙들어 주는 장치다.
        struct CorridorPlan
        {
            std::array<Rect, 3> bands{};
            int bandCount = 0;
            int score = 0;
            Doorway doorways[2]{};
        };

        Grid GenerateOnce(std::uint32_t seed, std::vector<MapGenerationSnapshot>* trace) const;
        void SplitIntoFour(Node& node) const;
        static void CollectLeafNodes(Node& node, std::vector<Node*>& leaves);
        void AssignLeafRoles(Node& root, std::mt19937& random) const;
        void CreateRooms(Node& node, Grid& grid, std::mt19937& random) const;
        std::vector<RoomEdge> BuildRoomGraph(const std::vector<Rect>& rooms, std::mt19937& random) const;
        void CarveRoom(Grid& grid, const Rect& room) const;

        // 간선 하나라도 **제3의 방을 파고들지 않으면서 폭 2를 유지하는 직교 경로**가
        // 없으면 즉시 false 다. 그 시드는 버린다.
        bool CarveCorridors(Grid& grid) const;
        static bool PlanCorridor(const Grid& grid, int edgeIndex, const RoomEdge& edge,
            const std::vector<Rect>& blockers, const std::vector<Int2>& claimed, CorridorPlan& best);

        // 시작 방과, 그 방의 바깥을 향하는 2타일 탈출구, 그리고 그것을 바라보는
        // 플레이어 시작 자리. 맵 경계에 닿는 활성 방이 없으면 false 다.
        bool PlaceStartAndExit(Grid& grid, std::mt19937& random) const;

        // Graph 거리 3 이상인 방 중 가장 깊은 곳. 자격을 갖춘 방이 없거나 가장 좋은
        // 곳조차 최소 비용보다 싸면 false 다.
        bool PlaceKey(Grid& grid, std::mt19937& random) const;

        // 간선 여덟 중 넷에, 한쪽 끝에만, 한 방에 셋은 절대 안 되게.
        bool PlaceDoors(Grid& grid, std::mt19937& random) const;

        // 감염 공간마다 침입구 — 막힌 문 하나, 창문 하나.
        bool PlaceBreachConnections(Grid& grid, std::mt19937& random) const;

        // Graph 거리 3 떨어진 활성 방 둘을 잇는 창문 정확히 하나.
        bool PlaceTraversalWindow(Grid& grid, std::mt19937& random) const;

        // 좀비 열둘과 추가 탄약. **탄약은 필수 동선 밖에만 놓인다.**
        bool PlaceZombiesAndAmmo(Grid& grid, std::mt19937& random) const;

        void CollectLeafRegions(const Node& node, std::vector<LeafRegion>& regions) const;

        MapGenerationSettings settings;
    };
}
