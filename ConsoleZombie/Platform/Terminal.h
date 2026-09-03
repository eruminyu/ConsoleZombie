#pragma once

#include <Core/Types.h>
#include <Render/PixelBuffer.h>
#include <Windows.h>
#include <chrono>
#include <string>
#include <string_view>

namespace Zombie
{
    // 한 프레임분의 입력. 폴링 결과를 그대로 담고 있고, 판정은 하지 않는다.
    struct InputState
    {
        bool forward = false;
        bool backward = false;
        bool turnLeft = false;
        bool turnRight = false;
        bool run = false;
        bool roll = false;
        bool interact = false;
        // 누르고 있는 동안만. 토글이 아니다 — 토글로 두면 플레이어가 뒤를 본 채로
        // 남아서 앞이 왜 앞이 아닌지 의아해하게 된다.
        bool lookBehind = false;
        bool debugMap = false;
        bool developerDebug = false;
        bool debugTopDown = false;
        // 탑다운의 비용 히트맵을 한 칸 돌린다. 끄기 → 이동 비용 → 음향 비용.
        bool debugCostField = false;
        bool previousGenerationStep = false;
        bool nextGenerationStep = false;
        bool regenerate = false;
        // 화면 흐름이 도는 키 셋. **Esc 는 더 이상 프로세스를 끝내지 않는다.**
        // "한 화면 뒤로"를 뜻하고, 그것을 종료로 바꾸는 것은 메인 메뉴 하나뿐이다.
        // 어디서든 종료되는 키는 오타 하나로 한 판을 날리는 키이고, 일시정지 화면이
        // 막으려는 것이 정확히 그것이다.
        bool menu = false;
        bool confirm = false;
        bool newSeed = false;
        // Config/GameBalance.ini 를 다시 읽는다. 튜닝은 고치고 해보는 반복인데,
        // 중간에 빌드가 끼면 지금 판단하고 있는 게임이 고치기 전의 게임이 된다.
        bool reloadBalance = false;
        bool leftClicked = false;
        Int2 mouseCell{};
        // 프레임이 실제로 그려지는 터미널 셀 블록과 그 시작 위치.
        // 출력 셀 상한이 없으면 뷰포트 전체와 같다. **조준은 뷰포트가 아니라 이쪽으로
        // 매핑해야 한다.** 아니면 여백 폭만큼 조준선이 포인터에서 밀린다.
        Int2 contentOrigin{};
        Int2 contentCells{};
    };

    // 게임 루프가 한 바퀴 사이에 쉬는 것.
    //
    // **`std::this_thread::sleep_for(1ms)` 로는 안 된다.** 프로세스의 기본 타이머
    // 해상도가 15.625ms 이고 `Sleep` 은 다음 틱까지 기다리므로, 1밀리초를 부탁하면
    // 이 기기에서 **실측 15.63ms** 를 잔다. 그러면 루프가 초당 예순네 번밖에 못 돌고,
    // 프레임 하나에 쓰는 3.4ms 를 더하면 천장이 52FPS 다 — `render_fps` 를 60 으로
    // 올려도 60 이 안 나오는 이유가 그것이다. 30 은 33ms 라 그 밑에 있어서 잘 나왔고,
    // 그래서 이 고장은 **"올릴 때만"** 보였다.
    //
    // 고해상도 대기 타이머는 프로세스 전역 타이머 해상도를 건드리지 않는다.
    // `timeBeginPeriod(1)` 은 같은 문제를 풀지만 **시스템 전체의 타이머를 바꾸는
    // 일**이고, 그건 발표용 노트북의 배터리에 게임이 낼 값이 아니다.
    class FrameSleeper
    {
    public:
        FrameSleeper();
        ~FrameSleeper();
        FrameSleeper(const FrameSleeper&) = delete;
        FrameSleeper& operator=(const FrameSleeper&) = delete;

        // 부탁한 만큼 잔다. 타이머를 못 얻었으면 예전 방식으로 떨어진다 —
        // 거친 대신 **여전히 돈다.** 옛 Windows 에서 게임이 안 켜지는 것보다 낫다.
        void Sleep(std::chrono::microseconds duration) const;

        // 고해상도 타이머를 실제로 얻었는가. 다른 기기에서 프레임이 안 나올 때
        // 처음 물어볼 질문이라 자체 테스트가 이것을 찍는다.
        bool IsHighResolution() const { return timer != nullptr; }

    private:
        void* timer = nullptr;
    };

    // Win32 콘솔 입출력. 반블록 문자로 셀 하나에 픽셀 두 개를 낸다.
    class TerminalSession
    {
    public:
        TerminalSession(int columns, int rows);
        ~TerminalSession();

        bool IsValid() const { return valid; }

        // 콘솔 셀을 줄여서 뷰포트가 더 많은 셀을 담게 한다. 변경이 실제로 먹혔을
        // 때만 true 다. **여기서 창을 최대화하지 않는다.**
        // 시도하면 마우스 입력과 창 조작 버튼이 같이 망가진다.
        bool TryApplyFontHeight(int fontCellHeight);

        // 프레임이 차지할 수 있는 셀 수의 상한. 0 이면 제한 없음.
        // **비용의 단위가 셀이다** — 써야 하는 바이트 수도, 호스트가 그려야 하는
        // 글리프 수도 셀에 비례하고 둘 다 렌더 해상도가 얼마인지는 신경 쓰지 않는다.
        // 상한을 넘으면 프레임을 가운데 두고 여백은 검게 남긴다.
        void SetOutputLimits(int maxColumns, int maxRows);

        InputState PollInput();
        void Present(const PixelBuffer& pixels);

        // 프레임 하나 전체의 이스케이프 시퀀스. Present() 에서 떼어낸 이유는
        // 자체 테스트가 콘솔 없이 조립 결과를 검사할 수 있게 하려는 것이다.
        //
        // 모든 행이 절대 커서 이동으로 시작한다. 그래서 상한이 걸린 프레임 바깥의
        // 여백에는 한 바이트도 쓰지 않는다. 매 프레임 여백을 칠하면 상한이 존재할
        // 이유가 사라진다.
        static std::string ComposeFrame(const PixelBuffer& pixels,
            int contentColumns, int contentRows, int originColumn = 0, int originRow = 0);

        void SetTitle(const std::string& title);
        int ContentColumns() const;
        int ContentRows() const;

    private:
        void PollWin32Mouse(InputState& state);
        void PollScreenMouse(InputState& state);
        void ResolveHostWindow();
        bool ApplyOutputMode() const;
        bool ApplyInputMode() const;
        bool TrySetFontCellHeight(int fontCellHeight);
        void RefreshViewportSize();
        void Write(std::string_view text) const;

        HANDLE output = INVALID_HANDLE_VALUE;
        HANDLE input = INVALID_HANDLE_VALUE;
        DWORD originalOutputMode = 0;
        DWORD originalInputMode = 0;
        UINT originalOutputCodePage = 0;
        bool restoreOutputMode = false;
        bool restoreInputMode = false;
        bool restoreOutputCodePage = false;
        bool valid = false;
        bool leftButtonDown = false;
        int viewportColumns = 0;
        int viewportRows = 0;
        int maxOutputColumns = 0;
        int maxOutputRows = 0;
        // 지난 프레임이 어디에 어떤 크기로 놓였는지. 값이 바뀌었다는 것은 여백이
        // 움직였다는 뜻이고, 다음 프레임을 놓기 전에 화면을 한 번 지워야 한다.
        int presentedColumns = 0;
        int presentedRows = 0;
        int presentedOriginColumn = 0;
        int presentedOriginRow = 0;
        Int2 mouseCell{};
        std::string windowTitleToken;
        HWND hostWindow = nullptr;
    };
}
