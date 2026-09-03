#include <Game/Balance.h>
#include <Game/Game.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

// 진입점. 갈래가 셋이다 — 자체 테스트(--self-test), 생성 스냅샷(--snapshot),
// 그리고 평범한 실행. 앞의 둘은 콘솔도 오디오도 열지 않는다.
int main(int argc, char** argv)
{
    bool selfTest = false;
    std::filesystem::path snapshotPath;
    std::uint32_t seed = static_cast<std::uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // 이 시드가 요청받은 것인지 시계에서 나온 것인지. 메뉴는 요청받은 시드를 매번
    // 그대로 재현하고, 아니면 시작할 때마다 새 맵을 굴린다. 한 세션 안에서 메뉴
    // 시작이 늘 같은 맵이면 시작 버튼이 고장 난 것으로 읽힌다.
    bool seedPinned = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--self-test")
        {
            selfTest = true;
        }
        else if (argument == "--seed" && index + 1 < argc)
        {
            try
            {
                seed = static_cast<std::uint32_t>(std::stoul(argv[++index]));
                seedPinned = true;
            }
            catch (...)
            {
                std::cerr << "Invalid --seed value.\n";
                return 2;
            }
        }
        else if (argument == "--snapshot" && index + 1 < argc)
        {
            snapshotPath = argv[++index];
        }
    }

    // argv[0] 을 넘기는 이유는 셋 다 같다 — 설정 파일을 실행 파일 옆에서부터
    // 찾아 올라가야 하기 때문이다. 빌드가 exe 옆에 사본을 떨궈 둔다.
    try
    {
        if (selfTest)
        {
            return Zombie::Game::RunSelfTest(argc > 0 ? argv[0] : "");
        }
        if (!snapshotPath.empty())
        {
            return Zombie::Game::WriteSnapshot(seed, snapshotPath);
        }

        const std::filesystem::path balancePath =
            Zombie::Balance::FindConfigFile(argc > 0 ? argv[0] : "");
        const Zombie::Balance balance = Zombie::Balance::Load(balancePath);
        if (!balance.problems.empty())
        {
            std::cerr << "GameBalance.ini contains invalid settings";
            if (!balance.sourcePath.empty()) std::cerr << " (" << balance.sourcePath << ')';
            std::cerr << ":\n";
            for (const std::string& problem : balance.problems)
            {
                std::cerr << "  - " << problem << '\n';
            }
            return 2;
        }

        Zombie::Game game(seed, balance, seedPinned, argc > 0 ? argv[0] : "");
        return game.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 3;
    }
}
