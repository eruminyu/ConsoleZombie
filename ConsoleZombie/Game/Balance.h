#pragma once

#include <filesystem>
#include <istream>
#include <string>
#include <utility>
#include <vector>

namespace Zombie
{
    struct Balance
    {
        // **여기 있는 0들은 일부러 0이다.** 게임이 실제로 노는 숫자는 전부
        // Config/GameBalance.ini 에서 온다 — 디스크의 파일이거나, 빌드 때 바이너리에
        // 구워 넣은 그 사본이거나. Defaults() 가 후자를 파싱한다.
        //
        // **이 초기값으로 만들어진 Balance 가 게임에 도달하는 일은 없다.** Load() 는
        // Defaults() 에서 출발하고, 예전에 Balance{} 라고 쓰던 자리도 전부 그렇다.
        //
        // 한때는 여기에도 진짜 값이 적혀 있었고 ini 에도 적혀 있었으며, 둘을 맞춰
        // 두라는 주석이 붙어 있었다. **그게 하루에 두 번 실패했다** — 튜닝된 ini 가
        // 낡은 기본값과 싸웠고, 예산 상수 하나가 모든 시드를 만들 수 없게 만들었다.
        // 둘 다 잡히기는 했지만, **실수를 잡는 것보다 실수를 할 수 없게 하는 것이 낫다.**
        float playerWalkSpeed = 0.0f;
        float playerRunSpeed = 0.0f;
        float playerRotationSpeed = 0.0f;
        float rollSpeed = 0.0f;
        float rollDuration = 0.0f;
        float rollCooldown = 0.0f;
        // 반동은 총알이 나갔다는 신호이므로 실제로 나갔을 때만 돈다. 튀는 양은
        // 그것이 움직이는 무기와 마찬가지로 프레임 높이에 대한 비율이다. 아니면
        // 창 크기마다 다른 뜻이 된다.
        //
        // 조준선이 튀는 것도 같은 시계를 쓴다. 무기의 반동과 조준선의 반동은 **한
        // 사건**이고, 시계를 나누면 이유 없이 서로 어긋날 수 있다.
        float weaponRecoilDuration = 0.0f;
        float weaponRecoilKick = 0.0f;
        // 조준 흔들림. 단위는 설계 기준인 120x80 논리 화면의 픽셀이다. 쓸 때
        // frameHeight / 80 으로 배율을 준다. 안 그러면 값 1이 지금 게임이 실제로
        // 도는 프레임에서는 보이지도 않는다.
        float aimSwayWalkPixels = 0.0f;
        float aimSwayRunPixels = 0.0f;
        float aimSwayRollPixels = 0.0f;
        // 멈춘 뒤 흔들림이 가라앉는 데 걸리는 초.
        float aimSwaySettle = 0.0f;
        float aimKickUpPixels = 0.0f;
        float aimKickSidePixels = 0.0f;
        // 맞으면 화면 가장자리가 붉어진다. 전체를 물들이지 않고 비네트로 두는 이유는,
        // 이 게임의 조준 도구가 조준선이고 그것을 가리면 피드백이 벌이 되기 때문이다.
        float hitFlashDuration = 0.0f;
        float hitFlashStrength = 0.0f;
        // 소리. World 큐는 거리가 아니라 **음향 경로 비용**으로 잦아든다. 그래서
        // 플레이어와 소리 사이의 벽이 벽으로 들린다.
        float masterVolume = 0.0f;
        float sfxVolume = 0.0f;
        // 배경음은 한 세션 내내 도니까 효과음보다 한참 아래 둔다.
        float bgmVolume = 0.0f;
        float audioHearingCost = 0.0f;
        // 달리기는 이제 자원이다. 총량의 단위가 **달릴 수 있는 초**라서, 총량 6에
        // 소모 1이면 정확히 6초를 전력으로 달린다. 구르기가 그중 2.5를 쓴다.
        float staminaMax = 0.0f;
        float staminaRunDrain = 0.0f;
        float staminaRollCost = 0.0f;
        float staminaRegen = 0.0f;
        float staminaRegenDelay = 0.0f;
        float zombieSpeed = 0.0f;
        float zombieAwarenessRange = 0.0f;
        float zombieRepathInterval = 0.0f;
        float zombieAttackInterval = 0.0f;
        // 깨어난 좀비가 아직 오고 있다고 얼마나 자주 말하는가. 첫 울음 뒤의 침묵은
        // 좀비가 관심을 잃은 것으로 읽히는데, 좀비는 절대 관심을 잃지 않는다.
        float zombieGrowlInterval = 0.0f;
        // 좀비가 보폭 한 주기에 걷는 거리(타일), 그리고 시체가 바닥에 닿는 데
        // 걸리는 시간. 둘 다 규칙이 아니라 겉모습이지만, **잘못된 겉모습은 다시
        // 빌드하지 않고는 아무도 못 고치는 겉모습이다.**
        float zombieStrideTiles = 0.0f;
        float zombieDeathDuration = 0.0f;
        // 아래 소음량과 함께 내려간 값이다. 소리가 훨씬 덜 퍼지게 됐으니 전달하는
        // 압력도 작아졌고, 임계값도 같이 움직였다.
        float breachWarningPressure = 0.0f;
        float breachCrackPressure = 0.0f;
        float breachBreakPressure = 0.0f;
        float breachPressureDecay = 0.0f;
        float breachDecayDelay = 0.0f;
        float breachExistingPressureWeight = 0.0f;
        // 소리를 들은 좀비는 자기 침입구가 무너질 때까지 민다. **이게 없으면 감염
        // 공간은 충분히 큰 소음을 영원히 기다린다.**
        float breachZombiePressurePerSecond = 0.0f;

        // 유효 소음 = max(0, 원본 - 음향 경로 비용 × falloff).
        float noiseAcousticFalloff = 0.0f;
        float noisePressureScale = 0.0f;

        // 행동마다 — 얼마나 시끄러운지, 그리고 맵이 반응할 확률.
        //
        // 소음 하나가 닿는 음향 경로 비용은 `소음량 / falloff` 다. falloff 1.5 에서
        // 총성 30이면 열린 바닥 스무 타일쯤 가는데, 방 하나와 다음 방까지의 복도
        // 정도다. 플레이 테스트 2026-08-27: 100이었을 때 예순여섯 타일을 갔고
        // **맵의 모든 좀비가 달려왔다.**
        float walkNoise = 0.0f;
        float walkReactionChance = 0.0f;
        float walkNoiseInterval = 0.0f;
        float runNoise = 0.0f;
        float runReactionChance = 0.0f;
        float runNoiseInterval = 0.0f;
        float rollNoise = 0.0f;
        float rollReactionChance = 0.0f;
        float doorNoise = 0.0f;
        float doorReactionChance = 0.0f;
        float shotNoise = 0.0f;
        float shotReactionChance = 0.0f;
        int breachMaxResponders = 0;
        // 렌더러가 터미널 뷰포트에 맞추는 프레임의 상한(프레임 픽셀).
        // **기기와 무관한 출력 해상도**가 이것이다 — 이보다 큰 창은 더 많이 그려서
        // 채우는 것이 아니라 같은 프레임을 확대해서 채운다. 내리면 선명함을 주고
        // 레이캐스트 비용을 산다.
        int renderMaxWidth = 0;
        int renderMaxHeight = 0;
        // 프레임이 차지할 수 있는 터미널 셀의 상한. 0 은 창 전체다.
        // **비용의 단위가 셀이다** — 써야 하는 바이트도, 호스트가 그리는 글리프도
        // 셀에 비례하고, 둘 다 렌더 해상도는 신경 쓰지 않는다. 창이 너무 커서 채우는
        // 값이 비쌀 때만 켠다. 그러면 프레임이 가운데로 가고 여백은 검게 남는다.
        int presentMaxColumns = 0;
        int presentMaxRows = 0;
        // HUD 글자의 em 높이(프레임 픽셀). 0 이면 프레임에서 계산한다.
        int uiTextHeight = 0;
        // 콘솔 셀 높이(픽셀). 시작할 때 한 번 적용한다. **Windows Terminal 은 이것을
        // 무시하고 자기 프로필 글꼴을 쓴다.** conhost 는 따르지만 셀 크기를 바꾸면
        // 마우스 매핑이 흐트러질 수 있어서 기본이 꺼짐이다. 0 이면 글꼴을 안 건드린다.
        int terminalFontHeight = 0;
        // 열쇠가 얼마나 깊이 앉아야 하는가. 탈출구에서의 A* 비용으로 잰다.
        // 가장 좋은 열쇠 방이 이보다 싼 시드는 버리고 다시 굴린다.
        float minimumKeyPathCost = 0.0f;
        int startAmmo = 0;
        // Balance::Load()가 10..14로 자른다. 게임과 자체 테스트가 같은 값을 본다.
        int zombieCount = 0;
        int extraAmmoRounds = 0;
        int playerHealth = 0;
        int renderFps = 0;
        // 0 이면 장치를 아예 안 연다. 그 외에는 아무것도 안 바뀐다.
        int audioEnabled = 0;
        // 상태 한 줄이 떠 있는 시간과, 같은 줄이 다시 안 올라오는 시간.
        // 발소리가 0.5초마다 나므로 쿨다운이 없으면 로그가 "문 두드리는 소리가
        // 들린다" 하나로 가득 찬다.
        float uiLogSeconds = 0.0f;
        float uiLogCooldown = 0.0f;

        // 이 설정의 무엇이 잘못됐는지를 말로. 놀 수 있는 설정이면 비어 있다.
        // 어느 경로로 들어왔든 Load() 가 채운다.
        //
        // 잘못된 조합은 예전에 "map generation gave up after 100 attempts on seed 1"
        // 로 드러났는데, 그건 원인에서 세 단계 떨어진 증상의 이름이다.
        // **2026-08-29 에 그것이 저녁 하나를 먹었다** — extra_ammo_rounds 가 예산 6에
        // 대해 5가 되면서 모든 시드가 거절됐고, 어디에도 이유를 말하는 곳이 없었다.
        // 그래서 이제 **숫자가 도착하는 자리에서** 한 번 검사한다.
        std::vector<std::string> problems;

        // 파일이 없어서 여기 값이 전부 내장 기본값일 때 false. 게임이 화면에 그렇게
        // 말한다. 방금 고친 설정을 조용히 무시하는 것이 가장 나쁜 실패 방식이다.
        bool loadedFromFile = false;
        // Load() 가 **실제로 따른** 파일. 빌드가 실행 파일 옆에 사본을 떨구므로 둘
        // 이상 존재할 수 있다. 게임이 읽은 것의 이름을 직접 말한다 — 플레이어가
        // 동작을 보고 역추적하게 두지 않는다.
        std::filesystem::path sourcePath;

        // 튜닝값 하나하나와 ini 키의 짝. Load() 와 자체 테스트의 일치 검사가
        // **이 목록 하나를** 걷는다. 그래서 설정을 파서에만 가르치고 검사에서
        // 빠뜨리거나, 그 반대가 되는 일이 생길 수 없다.
        template <typename T> using Field = std::pair<const char*, T Balance::*>;
        static const std::vector<Field<float>>& FloatFields();
        static const std::vector<Field<int>>& IntFields();

        // 어긋난 키 하나와 양쪽 값. 값을 같이 들고 오는 이유는, 다음 할 일이 대개
        // 한쪽을 다른 쪽에 복사하는 것이고 키 이름만 늘어놓으면 그것들을 전부 다시
        // 찾아봐야 하기 때문이다.
        struct Difference
        {
            std::string key;
            double here = 0.0;
            double there = 0.0;
        };
        // 이쪽 값이 other 와 다른 키들. 같으면 비어 있다.
        std::vector<Difference> DifferencesFrom(const Balance& other) const;

        // 이 값들의 **조합**에서 problems 를 채운다. Load() 가 모든 경로에서 부른다.
        // 공개인 이유는 손으로 만든 설정에 이 규칙을 겨눌 수 있어야 하기 때문이고,
        // 그것이 규칙이 실제로 문다는 것을 보이는 유일한 방법이다.
        void Validate();

        // 출하된 Config/GameBalance.ini 를 빌드 때 바이너리에 구운 것을, **파일과
        // 같은 코드로** 파싱한 결과.
        //
        // 설정 파일이 없는 빌드가 노는 것이 이것이고, 어떤 기본값도 두 번 적히지
        // 않는 이유가 이것이다 — **ini 가 이 숫자들이 적히는 유일한 곳이다.**
        static const Balance& Defaults();
    private:
        // 파일과 구워 넣은 사본을 읽는 파서 하나.
        static std::vector<std::string> ReadInto(Balance& result, std::istream& stream);
    public:

        static Balance Load(const std::filesystem::path& path);
        // 검색 경로에 있는 설정 파일 **전부**를 가까운 순으로. 보통 하나 이상이다 —
        // 빌드가 실행 파일 옆에 하나를 떨구고 저장소가 원본을 갖고 있다. 자체
        // 테스트가 전부 일치하는지 확인하는데, 실제로 따르는 것은 맨 앞 하나뿐이라
        // 그 뒤에 숨은 낡은 사본은 게임 안에서 보이지 않기 때문이다.
        static std::vector<std::filesystem::path> FindConfigFiles(
            const std::filesystem::path& executable);
        // Load() 에게 넘길 하나 — 후보 중 **가장 최근에 쓰인 것**. 아무것도 안
        // 걸리면 빈 경로다. 시작할 때와 F5 가 이 함수 하나를 쓴다. 왜 "가까운"이
        // 아니라 "최근"인지는 .cpp 에 있다.
        static std::filesystem::path FindConfigFile(const std::filesystem::path& executable);
    };
}
