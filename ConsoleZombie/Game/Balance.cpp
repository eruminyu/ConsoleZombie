#include "Balance.h"

#include <GameBalanceDefaults.g.h>

#include <Render/RaycastRenderer.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cmath>
#include <unordered_map>

namespace Zombie
{
    namespace
    {
        // 앞뒤 공백 제거.
        std::string Trim(std::string value)
        {
            const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
            value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
            return value;
        }

        // 튜닝값이 갇히는 범위 전부. 한 곳에 모아 두어야 설정 파일이 없는 경로와
        // 불러온 경로가 범위에 대해 다른 말을 할 수 없다.
        void ApplyLimits(Balance& value)
        {
            value.renderFps = std::clamp(value.renderFps, 10, 60);
            const bool floatsFinite = std::all_of(value.FloatFields().begin(), value.FloatFields().end(),
                [&value](const Balance::Field<float>& field)
                {
                    return std::isfinite(value.*field.second);
                });
            // NaN에 std::max를 적용하면 기본값처럼 보이는 다른 숫자로 바뀔 수 있다.
            // 유효하지 않은 원문은 그대로 두어 Validate()가 정확한 키를 보고하게 한다.
            if (floatsFinite)
            {
                value.breachWarningPressure = std::max(0.0f, value.breachWarningPressure);
                value.breachCrackPressure = std::max(value.breachWarningPressure + 0.01f, value.breachCrackPressure);
                value.breachBreakPressure = std::max(value.breachCrackPressure + 0.01f, value.breachBreakPressure);
                value.breachPressureDecay = std::max(0.0f, value.breachPressureDecay);
                value.breachDecayDelay = std::max(0.0f, value.breachDecayDelay);
                value.breachExistingPressureWeight = std::max(0.0f, value.breachExistingPressureWeight);
                value.noiseAcousticFalloff = std::max(0.0f, value.noiseAcousticFalloff);
                value.noisePressureScale = std::max(0.0f, value.noisePressureScale);
                value.walkNoise = std::max(0.0f, value.walkNoise);
                value.runNoise = std::max(0.0f, value.runNoise);
                value.rollNoise = std::max(0.0f, value.rollNoise);
                value.doorNoise = std::max(0.0f, value.doorNoise);
                value.shotNoise = std::max(0.0f, value.shotNoise);
                value.walkReactionChance = std::clamp(value.walkReactionChance, 0.0f, 1.0f);
                value.runReactionChance = std::clamp(value.runReactionChance, 0.0f, 1.0f);
                value.rollReactionChance = std::clamp(value.rollReactionChance, 0.0f, 1.0f);
                value.doorReactionChance = std::clamp(value.doorReactionChance, 0.0f, 1.0f);
                value.shotReactionChance = std::clamp(value.shotReactionChance, 0.0f, 1.0f);
                value.walkNoiseInterval = std::max(0.05f, value.walkNoiseInterval);
                value.runNoiseInterval = std::max(0.05f, value.runNoiseInterval);
            }
            value.breachMaxResponders = std::clamp(value.breachMaxResponders, 1, 4);
            value.renderMaxWidth = std::clamp(value.renderMaxWidth,
                RaycastRenderer::MinWidth, RaycastRenderer::MaxWidth);
            value.renderMaxHeight = std::clamp(value.renderMaxHeight,
                RaycastRenderer::MinHeight, RaycastRenderer::MaxHeight);
            // 60열이나 20행 아래로 내려가면 프레임을 읽을 수 없다. 그래서 작은
            // 양수는 글자 그대로 받지 않고 하한으로 올려 준다.
            value.presentMaxColumns = value.presentMaxColumns <= 0
                ? 0 : std::max(value.presentMaxColumns, RaycastRenderer::MinWidth);
            value.presentMaxRows = value.presentMaxRows <= 0
                ? 0 : std::max(value.presentMaxRows, RaycastRenderer::MinHeight / 2);
            value.uiTextHeight = value.uiTextHeight <= 0
                ? 0 : std::clamp(value.uiTextHeight, 6, 48);
            value.terminalFontHeight = value.terminalFontHeight <= 0
                ? 0 : std::clamp(value.terminalFontHeight, 4, 48);
            value.zombieCount = std::clamp(value.zombieCount, 10, 14);
            value.extraAmmoRounds = std::max(2, value.extraAmmoRounds);
        }

    }

    // 위의 개별 범위와 달리 **조합**을 본다.
    //
    // 숫자 하나만 보면 아주 멀쩡한데 다른 숫자 옆에 서면 게임을 불가능하게 만드는
    // 경우가 있다. 그리고 그 실패는 게임 안에서는 읽어낼 수 없는 종류다 —
    // 화면에 나오는 말이 원인에서 세 단계 떨어진 "맵 생성 실패"가 된다.
    // 이 설정 조합이 게임으로서 말이 되는가. 문제를 problems 에 채운다.
    void Balance::Validate()
    {
        {
            Balance& value = *this;
            value.problems.clear();
            for (const Field<float>& field : FloatFields())
            {
                if (!std::isfinite(value.*field.second))
                {
                    value.problems.push_back(std::string(field.first) + " must be finite");
                }
            }
            const auto requirePositive = [&value](const char* key, float number)
            {
                if (std::isfinite(number) && number <= 0.0f)
                    value.problems.push_back(std::string(key) + " must be greater than zero");
            };
            const auto requireNonNegative = [&value](const char* key, float number)
            {
                if (std::isfinite(number) && number < 0.0f)
                    value.problems.push_back(std::string(key) + " must not be negative");
            };
            const auto requireUnitRange = [&value](const char* key, float number)
            {
                if (std::isfinite(number) && (number < 0.0f || number > 1.0f))
                    value.problems.push_back(std::string(key) + " must stay between zero and one");
            };
            requirePositive("player_walk_speed", value.playerWalkSpeed);
            requirePositive("player_run_speed", value.playerRunSpeed);
            requirePositive("player_rotation_speed", value.playerRotationSpeed);
            requireNonNegative("roll_speed", value.rollSpeed);
            requireNonNegative("roll_duration", value.rollDuration);
            requireNonNegative("roll_cooldown", value.rollCooldown);
            requireNonNegative("weapon_recoil_duration", value.weaponRecoilDuration);
            requireNonNegative("weapon_recoil_kick", value.weaponRecoilKick);
            requireNonNegative("aim_sway_walk_pixels", value.aimSwayWalkPixels);
            requireNonNegative("aim_sway_run_pixels", value.aimSwayRunPixels);
            requireNonNegative("aim_sway_roll_pixels", value.aimSwayRollPixels);
            requireNonNegative("aim_sway_settle", value.aimSwaySettle);
            requireNonNegative("aim_kick_up_pixels", value.aimKickUpPixels);
            requireNonNegative("aim_kick_side_pixels", value.aimKickSidePixels);
            requireNonNegative("hit_flash_duration", value.hitFlashDuration);
            requireUnitRange("hit_flash_strength", value.hitFlashStrength);
            requireUnitRange("master_volume", value.masterVolume);
            requireUnitRange("sfx_volume", value.sfxVolume);
            requireUnitRange("bgm_volume", value.bgmVolume);
            requirePositive("audio_hearing_cost", value.audioHearingCost);
            requireNonNegative("zombie_speed", value.zombieSpeed);
            requireNonNegative("zombie_awareness_range", value.zombieAwarenessRange);
            requirePositive("zombie_repath_interval", value.zombieRepathInterval);
            requirePositive("zombie_attack_interval", value.zombieAttackInterval);
            requirePositive("zombie_growl_interval", value.zombieGrowlInterval);
            requirePositive("zombie_stride_tiles", value.zombieStrideTiles);
            requireNonNegative("zombie_death_duration", value.zombieDeathDuration);
            requireNonNegative("breach_zombie_pressure_per_second",
                value.breachZombiePressurePerSecond);
            requireNonNegative("stamina_max", value.staminaMax);
            requireNonNegative("stamina_run_drain", value.staminaRunDrain);
            requireNonNegative("stamina_roll_cost", value.staminaRollCost);
            requireNonNegative("stamina_regen", value.staminaRegen);
            requireNonNegative("stamina_regen_delay", value.staminaRegenDelay);
            requireNonNegative("minimum_key_path_cost", value.minimumKeyPathCost);
            requireNonNegative("ui_log_seconds", value.uiLogSeconds);
            requireNonNegative("ui_log_cooldown", value.uiLogCooldown);
            // 6.10 이 실제로 요구하는 것. 탄약은 선택 사항이고 맵을 청소하는 것이
            // 선택지에 있어서는 안 된다. 그러므로 맵 전체의 탄약이 좀비 수보다 적어야 한다.
            //
            // **고정된 총량이 아니라 좀비 수에 대한 관계로 적었다.** 그래야 한쪽을
            // 튜닝했을 때 다른 쪽이 조용히 무효가 되지 않는다. 상수 6이 2026-08-29 에
            // 한 일이 정확히 그것이었다.
            if (value.startAmmo < 0)
            {
                value.problems.push_back("start_ammo must not be negative");
            }
            const long long rounds = static_cast<long long>(value.startAmmo)
                + static_cast<long long>(value.extraAmmoRounds);
            if (rounds >= value.zombieCount)
            {
                value.problems.push_back("start_ammo + extra_ammo_rounds ("
                    + std::to_string(rounds) + ") must stay under zombie_count ("
                    + std::to_string(value.zombieCount)
                    + "): 6.10 requires that killing every zombie is never possible");
            }
            if (value.staminaMax <= 0.0f && value.staminaRunDrain > 0.0f)
            {
                value.problems.push_back(
                    "stamina_max is zero while stamina_run_drain is not: running would never work");
            }
            if (value.playerHealth <= 0)
            {
                value.problems.push_back("player_health must be at least one");
            }
        }
    }

    // 검색 경로에 있는 설정 파일 **전부**를 가까운 순으로.
    //
    // 보통 하나 이상이다. 빌드가 실행 파일 옆에 사본을 떨구고 저장소가 원본을 갖고
    // 있다. 자체 테스트가 전부 일치하는지 검사하는데, 실제로 읽히는 것은 하나뿐이라
    // 그 뒤에 숨은 낡은 사본은 게임 안에서 보이지 않기 때문이다.
    std::vector<std::filesystem::path> Balance::FindConfigFiles(
        const std::filesystem::path& executable)
    {
        std::vector<std::filesystem::path> found;
        // 슬래시가 든 리터럴이 아니라 조각을 이어 만든다. 그래야 여기서 보고하는
        // 경로가 구분자 두 종류를 섞어 쓰지 않는다.
        const std::filesystem::path relative = std::filesystem::path("Config") / "GameBalance.ini";
        std::error_code error;

        // 배포 배치는 Config/ 가 실행 파일 옆에 있고, 개발 배치는 실행 파일이
        // Bin/ 아래 몇 단계 깊이에 있으므로 위로 올라가야 한다.
        // **보통 둘 다 동시에 존재한다.** 그래서 이 함수는 고르지 않고 모은다.
        std::filesystem::path directory = std::filesystem::absolute(executable, error).parent_path();
        for (int depth = 0; depth < 8 && !directory.empty(); ++depth)
        {
            const std::filesystem::path candidate = directory / relative;
            if (std::filesystem::exists(candidate, error)) found.push_back(candidate);
            const std::filesystem::path parent = directory.parent_path();
            if (parent.empty() || parent == directory) break;
            directory = parent;
        }

        // 마지막 수단은 작업 디렉터리. Visual Studio 가 잡아주는 것이 그것이다.
        if (std::filesystem::exists(relative, error))
        {
            const std::filesystem::path absolute = std::filesystem::absolute(relative, error);
            bool alreadyFound = false;
            for (const std::filesystem::path& entry : found)
                if (std::filesystem::equivalent(entry, absolute, error)) alreadyFound = true;
            if (!alreadyFound) found.push_back(absolute);
        }
        return found;
    }

    // Load() 에게 넘길 파일 하나 — 후보 중 **가장 최근에 쓰인 것**. 없으면 빈 경로다.
    //
    // 가장 가까운 것이 아니다. 가까운 것은 빌드가 실행 파일 옆에 떨군 사본이고,
    // 그것은 지금 게임을 켜게 만든 편집보다 언제나 오래됐다.
    //
    // **F5 다시 읽기는 처음부터 이 규칙이었는데 시작 경로만 옛 규칙에 남아 있었다.**
    // 그래서 ini 를 고치고 빌드 없이 실행하면, F5 를 누르기 전까지는 고치기 전의
    // 게임이 돌았다. `render_fps` 가 정확히 그렇게 안 먹었다 — 파일에는 60 이
    // 적혀 있고 게임은 exe 옆 사본의 30 으로 돌고 있었다.
    //
    // 규칙이 여기 하나에 있어야 셋째 호출부가 생겨도 다시 갈라지지 않는다.
    std::filesystem::path Balance::FindConfigFile(const std::filesystem::path& executable)
    {
        std::filesystem::path chosen;
        std::filesystem::file_time_type newest{};
        for (const std::filesystem::path& candidate : FindConfigFiles(executable))
        {
            std::error_code error;
            const auto written = std::filesystem::last_write_time(candidate, error);
            // 시간을 못 읽는 후보는 없는 것으로 친다. 0 으로 치면 그것이 이길 수 있고,
            // 그러면 읽을 수 없는 사본이 읽을 수 있는 것을 밀어낸다.
            if (error) continue;
            if (chosen.empty() || written > newest)
            {
                chosen = candidate;
                newest = written;
            }
        }
        return chosen;
    }

    // 실수 튜닝값과 ini 키의 짝 전부.
    //
    // Load() 와 자체 테스트의 일치 검사가 **이 목록 하나를** 걷는다. 그래서 설정을
    // 파서에만 가르치고 검사에서 빠뜨리거나 그 반대가 되는 일이 없다.
    const std::vector<Balance::Field<float>>& Balance::FloatFields()
    {
        static const std::vector<Field<float>> fields = {
            { "player_walk_speed", &Balance::playerWalkSpeed },
            { "player_run_speed", &Balance::playerRunSpeed },
            { "player_rotation_speed", &Balance::playerRotationSpeed },
            { "roll_speed", &Balance::rollSpeed },
            { "roll_duration", &Balance::rollDuration },
            { "roll_cooldown", &Balance::rollCooldown },
            { "weapon_recoil_duration", &Balance::weaponRecoilDuration },
            { "weapon_recoil_kick", &Balance::weaponRecoilKick },
            { "aim_sway_walk_pixels", &Balance::aimSwayWalkPixels },
            { "aim_sway_run_pixels", &Balance::aimSwayRunPixels },
            { "aim_sway_roll_pixels", &Balance::aimSwayRollPixels },
            { "aim_sway_settle", &Balance::aimSwaySettle },
            { "aim_kick_up_pixels", &Balance::aimKickUpPixels },
            { "aim_kick_side_pixels", &Balance::aimKickSidePixels },
            { "hit_flash_duration", &Balance::hitFlashDuration },
            { "hit_flash_strength", &Balance::hitFlashStrength },
            { "master_volume", &Balance::masterVolume },
            { "sfx_volume", &Balance::sfxVolume },
            { "bgm_volume", &Balance::bgmVolume },
            { "audio_hearing_cost", &Balance::audioHearingCost },
            { "zombie_speed", &Balance::zombieSpeed },
            { "zombie_awareness_range", &Balance::zombieAwarenessRange },
            { "zombie_repath_interval", &Balance::zombieRepathInterval },
            { "zombie_attack_interval", &Balance::zombieAttackInterval },
            { "zombie_growl_interval", &Balance::zombieGrowlInterval },
            { "zombie_stride_tiles", &Balance::zombieStrideTiles },
            { "zombie_death_duration", &Balance::zombieDeathDuration },
            { "breach_warning_pressure", &Balance::breachWarningPressure },
            { "breach_crack_pressure", &Balance::breachCrackPressure },
            { "breach_break_pressure", &Balance::breachBreakPressure },
            { "breach_pressure_decay", &Balance::breachPressureDecay },
            { "breach_decay_delay", &Balance::breachDecayDelay },
            { "breach_existing_pressure_weight", &Balance::breachExistingPressureWeight },
            { "breach_zombie_pressure_per_second", &Balance::breachZombiePressurePerSecond },
            { "stamina_max", &Balance::staminaMax },
            { "stamina_run_drain", &Balance::staminaRunDrain },
            { "stamina_roll_cost", &Balance::staminaRollCost },
            { "stamina_regen", &Balance::staminaRegen },
            { "stamina_regen_delay", &Balance::staminaRegenDelay },
            { "minimum_key_path_cost", &Balance::minimumKeyPathCost },
            { "ui_log_seconds", &Balance::uiLogSeconds },
            { "ui_log_cooldown", &Balance::uiLogCooldown },
            { "noise_acoustic_falloff", &Balance::noiseAcousticFalloff },
            { "noise_pressure_scale", &Balance::noisePressureScale },
            { "walk_noise", &Balance::walkNoise },
            { "walk_reaction_chance", &Balance::walkReactionChance },
            { "walk_noise_interval", &Balance::walkNoiseInterval },
            { "run_noise", &Balance::runNoise },
            { "run_reaction_chance", &Balance::runReactionChance },
            { "run_noise_interval", &Balance::runNoiseInterval },
            { "roll_noise", &Balance::rollNoise },
            { "roll_reaction_chance", &Balance::rollReactionChance },
            { "door_noise", &Balance::doorNoise },
            { "door_reaction_chance", &Balance::doorReactionChance },
            { "shot_noise", &Balance::shotNoise },
            { "shot_reaction_chance", &Balance::shotReactionChance }
        };
        return fields;
    }

    // 정수 튜닝값과 ini 키의 짝 전부. FloatFields() 와 같은 규칙이다.
    const std::vector<Balance::Field<int>>& Balance::IntFields()
    {
        static const std::vector<Field<int>> fields = {
            { "start_ammo", &Balance::startAmmo },
            { "zombie_count", &Balance::zombieCount },
            { "extra_ammo_rounds", &Balance::extraAmmoRounds },
            { "player_health", &Balance::playerHealth },
            { "render_fps", &Balance::renderFps },
            { "audio_enabled", &Balance::audioEnabled },
            { "breach_max_responders", &Balance::breachMaxResponders },
            { "render_max_width", &Balance::renderMaxWidth },
            { "render_max_height", &Balance::renderMaxHeight },
            { "present_max_columns", &Balance::presentMaxColumns },
            { "present_max_rows", &Balance::presentMaxRows },
            { "ui_text_height", &Balance::uiTextHeight },
            { "terminal_font_height", &Balance::terminalFontHeight }
        };
        return fields;
    }

    // 값이 다른 키들을 **양쪽 값과 함께** 돌려준다.
    //
    // 값을 같이 들고 오는 이유는, 다음 할 일이 대개 한쪽을 다른 쪽에 복사하는 것이고
    // 키 이름만 늘어놓으면 그것들을 전부 다시 찾아봐야 하기 때문이다.
    std::vector<Balance::Difference> Balance::DifferencesFrom(const Balance& other) const
    {
        std::vector<Difference> differences;
        for (const auto& [key, member] : FloatFields())
        {
            if (std::fabs(this->*member - other.*member) > 1.0e-4f)
                differences.push_back({ key, this->*member, other.*member });
        }
        for (const auto& [key, member] : IntFields())
        {
            if (this->*member != other.*member)
                differences.push_back({ key, static_cast<double>(this->*member),
                    static_cast<double>(other.*member) });
        }
        return differences;
    }

    // 파일과 구워 넣은 사본을 **같은 파서 하나**가 읽는다. 파서가 둘이면 ini 의
    // 뜻에 대해 다른 말을 할 기회가 둘이 되는데, 구워 넣은 사본이 존재하는 이유가
    // 바로 다툴 거리를 없애는 것이다.
    std::vector<std::string> Balance::ReadInto(Balance& result, std::istream& stream)
    {
        std::vector<std::string> problems;
        // 이 result 에 묶이지만 키는 위의 공유 표에서 온다. 그래서 설정을 파서에만
        // 가르치고 일치 검사에서 빠뜨리는 일이 생길 수 없다.
        std::unordered_map<std::string, float*> floats;
        for (const auto& [key, member] : FloatFields()) floats.emplace(key, &(result.*member));
        std::unordered_map<std::string, int*> integers;
        for (const auto& [key, member] : IntFields()) integers.emplace(key, &(result.*member));

        std::string line;
        while (std::getline(stream, line))
        {
            const std::size_t comment = line.find_first_of(";#");
            if (comment != std::string::npos) line.erase(comment);
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, equals));
            const std::string value = Trim(line.substr(equals + 1));
            try
            {
                if (const auto found = floats.find(key); found != floats.end())
                {
                    std::size_t parsed = 0;
                    const float number = std::stof(value, &parsed);
                    if (parsed != value.size()) throw std::invalid_argument("trailing characters");
                    *found->second = number;
                }
                else if (const auto whole = integers.find(key); whole != integers.end())
                {
                    std::size_t parsed = 0;
                    const int number = std::stoi(value, &parsed);
                    if (parsed != value.size()) throw std::invalid_argument("trailing characters");
                    *whole->second = number;
                }
            }
            catch (const std::exception&)
            {
                // 기본값으로 조용히 돌아가면 방금 고친 ini가 먹힌 것처럼 보인다.
                // 값은 이전 값으로 남기되 시작 경계가 이 문제를 보고 실행을 막는다.
                problems.push_back(key + " has an invalid numeric value: " + value);
            }
        }
        return problems;
    }

    const Balance& Balance::Defaults()
    {
        // 한 번만 만든다. 텍스트가 컴파일 타임 상수라 파일 작업이 아니라 메모리에
        // 있는 문자열을 파싱하는 것뿐이다.
        static const Balance baked = []
        {
            Balance value;
            std::istringstream text{ std::string(DefaultBalanceIni) };
            const std::vector<std::string> parseProblems = ReadInto(value, text);
            ApplyLimits(value);
            value.Validate();
            value.problems.insert(value.problems.begin(), parseProblems.begin(), parseProblems.end());
            return value;
        }();
        return baked;
    }

    // 설정 파일을 읽는다. 파일이 없어도 실패가 아니다 — 출하 값으로 논다.
    Balance Balance::Load(const std::filesystem::path& path)
    {
        // **필드 초기값이 아니라 구워 넣은 설정에서 출발한다.** Balance 의 필드
        // 초기값은 전부 0이고 게임에 도달하지 않는다.
        //
        // 그래서 키 일부만 적은 파일이 누구나 예상하는 대로 동작한다 — 안 적은 키는
        // 출하 값을 그대로 유지한다.
        Balance result = Defaults();
        const std::vector<std::string> builtInProblems = result.problems;
        result.loadedFromFile = false;
        result.sourcePath.clear();

        std::ifstream file(path);
        if (!file)
        {
            return result;
        }
        result.problems.clear();
        result.loadedFromFile = true;
        result.sourcePath = path;
        const std::vector<std::string> parseProblems = ReadInto(result, file);
        ApplyLimits(result);
        result.Validate();
        result.problems.insert(result.problems.begin(), parseProblems.begin(), parseProblems.end());
        result.problems.insert(result.problems.begin(), builtInProblems.begin(), builtInProblems.end());
        return result;
    }
}
