#include "Terminal.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

// Windows 10 1803 이전 SDK 에는 이 플래그가 없다. 값을 직접 적어 두면 그런 SDK 로도
// 빌드되고, 플래그를 모르는 **런타임**은 CreateWaitableTimerExW 를 실패시키므로
// 아래 폴백이 받는다. 안 되는 조합에서 조용히 잘못 도는 경로가 없다.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Zombie
{
    // 고해상도 대기 타이머 하나. 못 얻으면 nullptr 이고 그 자체가 답이다.
    FrameSleeper::FrameSleeper()
        : timer(CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
    }

    FrameSleeper::~FrameSleeper()
    {
        if (timer != nullptr) CloseHandle(timer);
    }

    void FrameSleeper::Sleep(std::chrono::microseconds duration) const
    {
        if (duration.count() <= 0) return;
        if (timer != nullptr)
        {
            // 단위는 100나노초이고 **음수가 상대 시간**이다. 부호를 빠뜨리면
            // 1601년 기준 절대 시각이 되어 이미 지난 시각을 기다리게 되고,
            // 그러면 즉시 돌아와 루프가 코어 하나를 태운다.
            LARGE_INTEGER due{};
            due.QuadPart = -(static_cast<LONGLONG>(duration.count()) * 10);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0)
            {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
        std::this_thread::sleep_for(duration);
    }

    namespace
    {
        // 지금 눌려 있는가. 콘솔 키 이벤트가 아니라 비동기 키 상태를 쓰는 이유는
        // 이동이 "눌린 동안"이지 "눌린 순간"이 아니기 때문이다.
        bool Held(int virtualKey)
        {
            return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        }

        // 콘솔 호스트 창인가. conhost 와 Windows Terminal 두 클래스만 인정한다.
        bool IsSupportedConsoleHostWindow(HWND window)
        {
            if (!window) return false;
            wchar_t className[128]{};
            if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0)
                return false;
            return std::wcscmp(className, L"ConsoleWindowClass") == 0
                || std::wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0;
        }

        struct WindowSearch
        {
            const char* titleToken = nullptr;
            HWND result = nullptr;
        };

        // EnumWindows 콜백. 제목에 우리 토큰이 든 보이는 창을 찾으면 멈춘다.
        BOOL CALLBACK FindConsoleHostWindow(HWND window, LPARAM parameter)
        {
            auto& search = *reinterpret_cast<WindowSearch*>(parameter);
            if (!IsWindowVisible(window)) return TRUE;
            char title[512]{};
            if (GetWindowTextA(window, title, static_cast<int>(std::size(title))) == 0) return TRUE;
            if (std::strstr(title, search.titleToken) == nullptr) return TRUE;
            search.result = window;
            return FALSE;
        }

        // 이 프로세스의 콘솔이 실제로 그려지고 있는 창. 마우스 좌표를 클라이언트
        // 영역 기준으로 환산할 때 필요하다.
        HWND FindConsoleHost(const std::string& titleToken)
        {
            HWND window = GetConsoleWindow();
            if (window && IsWindowVisible(window)) return window;

            WindowSearch search{ titleToken.c_str() };
            EnumWindows(FindConsoleHostWindow, reinterpret_cast<LPARAM>(&search));
            if (search.result) return search.result;

            // Windows Terminal 은 게임을 의사 콘솔(pseudoconsole)로 띄우므로
            // GetConsoleWindow() 가 보이는 최상위 창이 아니다. 방금 F5 로 띄운
            // 콘솔은 포그라운드에 있으니 그것을 마지막 수단으로 쓰되, 클래스 이름으로
            // 걸러서 **엉뚱한 프로그램의 창을 잡지 않게** 한다.
            window = GetForegroundWindow();
            return IsSupportedConsoleHostWindow(window) ? window : nullptr;
        }

    }

    // 콘솔을 게임용으로 바꾼다. UTF-8 출력, VT 처리, 대체 화면 버퍼, 커서 숨김.
    //
    // 창 크기와 글꼴은 건드리지 않는다. 자동 최대화는 세 번 시도해 세 번 실패했고,
    // 콘솔 폰트 API 는 Windows Terminal 이 무시한다. 창은 사용자 몫이다.
    TerminalSession::TerminalSession(int columns, int rows)
        : viewportColumns(columns), viewportRows(rows), mouseCell{ columns / 2, rows / 2 }
    {
        output = GetStdHandle(STD_OUTPUT_HANDLE);
        input = GetStdHandle(STD_INPUT_HANDLE);
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE)
        {
            return;
        }

        originalOutputCodePage = GetConsoleOutputCP();
        restoreOutputCodePage = SetConsoleOutputCP(CP_UTF8) != FALSE;
        // 제목에 프로세스 ID 를 심어둔다. 창을 이름으로 찾아야 할 때 쓰는 표식이다.
        windowTitleToken = "CZ-" + std::to_string(GetCurrentProcessId());
        SetConsoleTitleA(windowTitleToken.c_str());
        ResolveHostWindow();

        if (GetConsoleMode(output, &originalOutputMode)) restoreOutputMode = ApplyOutputMode();
        if (GetConsoleMode(input, &originalInputMode)) restoreInputMode = ApplyInputMode();
        valid = restoreOutputMode;
        if (valid)
        {
            Write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");
        }
    }

    // 바꿔놓은 것을 전부 되돌린다. 되돌리지 않으면 게임을 끝낸 터미널에 커서가
    // 사라진 채로 남는다.
    TerminalSession::~TerminalSession()
    {
        if (valid)
        {
            Write("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
        }
        if (restoreInputMode) SetConsoleMode(input, originalInputMode);
        if (restoreOutputMode) SetConsoleMode(output, originalOutputMode);
        if (restoreOutputCodePage) SetConsoleOutputCP(originalOutputCodePage);
    }

    // 이 프레임의 입력을 모은다. 키는 눌림 여부만 보고 판정은 Game 이 한다.
    InputState TerminalSession::PollInput()
    {
        ResolveHostWindow();
        RefreshViewportSize();

        InputState state;
        state.forward = Held('W');
        state.backward = Held('S');
        state.turnLeft = Held('A');
        state.turnRight = Held('D');
        state.run = Held(VK_SHIFT);
        state.roll = Held(VK_SPACE);
        state.interact = Held('E');
        state.lookBehind = Held('F');
        state.debugMap = Held(VK_F1);
        state.developerDebug = Held(VK_F2);
        state.debugTopDown = Held(VK_F3);
        state.debugCostField = Held(VK_F4);
        state.previousGenerationStep = Held(VK_OEM_4);
        state.nextGenerationStep = Held(VK_OEM_6);
        state.regenerate = Held('R');
        state.menu = Held(VK_ESCAPE);
        state.confirm = Held(VK_RETURN);
        state.newSeed = Held('N');
        state.reloadBalance = Held(VK_F5);

        // 마우스는 두 경로로 읽는다. 콘솔 이벤트 큐가 주 경로이고, 화면 좌표 계산이
        // Shift 를 누르고 있을 때의 유일한 경로다. 자세한 이유는 PollScreenMouse() 에.
        PollWin32Mouse(state);
        PollScreenMouse(state);
        state.mouseCell = mouseCell;
        state.contentCells = { ContentColumns(), ContentRows() };
        state.contentOrigin = { (viewportColumns - state.contentCells.x) / 2,
            (viewportRows - state.contentCells.y) / 2 };
        return state;
    }

    // 프레임이 쓸 수 있는 셀 상한. 0 은 제한 없음이다.
    void TerminalSession::SetOutputLimits(int maxColumns, int maxRows)
    {
        maxOutputColumns = std::max(0, maxColumns);
        maxOutputRows = std::max(0, maxRows);
    }

    // 실제로 그릴 셀 폭. 상한과 뷰포트 중 작은 쪽이다.
    int TerminalSession::ContentColumns() const
    {
        return maxOutputColumns > 0 ? std::min(viewportColumns, maxOutputColumns) : viewportColumns;
    }

    int TerminalSession::ContentRows() const
    {
        return maxOutputRows > 0 ? std::min(viewportRows, maxOutputRows) : viewportRows;
    }

    // 콘솔 셀 높이를 바꿔본다. conhost 는 따르고 Windows Terminal 은 무시한다.
    bool TerminalSession::TrySetFontCellHeight(int fontCellHeight)
    {
        if (fontCellHeight <= 0) return false;

        CONSOLE_FONT_INFOEX font{};
        font.cbSize = sizeof(font);
        if (!GetCurrentConsoleFontEx(output, FALSE, &font)) return false;
        if (font.dwFontSize.Y == static_cast<SHORT>(fontCellHeight)) return true;

        font.dwFontSize.X = 0;   // 폭은 글꼴이 알아서 맞추게 둔다
        font.dwFontSize.Y = static_cast<SHORT>(fontCellHeight);
        if (!SetCurrentConsoleFontEx(output, FALSE, &font)) return false;

        // **Windows Terminal 은 이 호출을 받아들이고 나서 무시한다.** 자기 프로필의
        // 글꼴로 그린다. 그래서 반환값을 믿지 않고 크기를 되읽어서, 정말로 뭔가
        // 바뀌었는지를 호출자에게 알려준다.
        CONSOLE_FONT_INFOEX applied{};
        applied.cbSize = sizeof(applied);
        if (!GetCurrentConsoleFontEx(output, FALSE, &applied)) return false;
        return applied.dwFontSize.Y == static_cast<SHORT>(fontCellHeight);
    }

    // VT 이스케이프를 해석하게 하고, 줄 끝에서 자동 개행하지 않게 한다.
    bool TerminalSession::ApplyOutputMode() const
    {
        const DWORD mode = originalOutputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        return SetConsoleMode(output, mode) != FALSE;
    }

    // 마우스 이벤트를 받고, 빠른 편집 모드를 끈다. 빠른 편집이 켜져 있으면 드래그가
    // 게임 입력이 아니라 텍스트 선택이 된다.
    //
    // VT 입력(1003·1006 마우스 캡처)은 **켜지 않는다.** 켜면 창의 최대화·닫기
    // 버튼까지 방해받는다.
    bool TerminalSession::ApplyInputMode() const
    {
        DWORD mode = originalInputMode | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT;
        mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
        return SetConsoleMode(input, mode) != FALSE;
    }

    // 설정이 요청한 셀 높이를 적용해 본다. 기본값 0 은 아무 일도 안 한다.
    bool TerminalSession::TryApplyFontHeight(int fontCellHeight)
    {
        if (!valid) return false;
        const bool applied = TrySetFontCellHeight(fontCellHeight);
        // 셀 크기가 달라지면 창에 들어가는 셀 수도 달라진다.
        if (applied) RefreshViewportSize();
        return applied;
    }

    // 호스트 창 핸들을 최신으로 유지한다. 창이 닫히거나 바뀔 수 있다.
    void TerminalSession::ResolveHostWindow()
    {
        if (!hostWindow || !IsWindow(hostWindow))
            hostWindow = FindConsoleHost(windowTitleToken);
    }

    // 지금 창에 셀이 몇 개 들어가는지 다시 잰다.
    //
    // 매 프레임 부른다. 사용자가 창을 최대화하는 순간 렌더 해상도가 따라가야 하는데,
    // 자동 최대화가 불가능하다는 결론의 대응책이 그것이다.
    void TerminalSession::RefreshViewportSize()
    {
        CONSOLE_SCREEN_BUFFER_INFO screenInfo{};
        if (!GetConsoleScreenBufferInfo(output, &screenInfo)) return;
        const int currentColumns = screenInfo.srWindow.Right - screenInfo.srWindow.Left + 1;
        const int currentRows = screenInfo.srWindow.Bottom - screenInfo.srWindow.Top + 1;
        if (currentColumns > 0) viewportColumns = currentColumns;
        if (currentRows > 0) viewportRows = currentRows;
    }

    // 화면 좌표에서 포인터 위치를 직접 계산하는 경로.
    void TerminalSession::PollScreenMouse(InputState& state)
    {
        const bool isActive = hostWindow && GetForegroundWindow() == hostWindow;
        POINT pointer{};
        RECT client{};
        if (isActive && GetCursorPos(&pointer) && GetClientRect(hostWindow, &client))
        {
            POINT clientOrigin{ client.left, client.top };
            ClientToScreen(hostWindow, &clientOrigin);

            // **Shift 를 누르고 있는 동안 남는 유일한 경로가 이것이다.** Shift+마우스는
            // Windows Terminal 자신의 선택 제스처라 그동안 마우스 이벤트를 앱에
            // 전달하지 않는다. 그래서 달리는 중에 조준선이 얼어붙는다.
            //
            // 따라서 셀 크기를 GetCurrentConsoleFont() 에 기대면 안 된다. Windows
            // Terminal은 이 폰트 API를 무시하고, 크기가 0 으로 오면 셀이 1픽셀로
            // 무너져서 모든 포인터가 범위 밖으로 보였다. 값이 제대로 올 때만 그것을
            // 쓰고, 아니면 클라이언트 영역을 뷰포트로 나눈다.
            CONSOLE_FONT_INFO fontInfo{};
            const bool haveFont = GetCurrentConsoleFont(output, FALSE, &fontInfo) != FALSE
                && fontInfo.dwFontSize.X > 0 && fontInfo.dwFontSize.Y > 0;
            const int clientSpanX = static_cast<int>(client.right - client.left);
            const int clientSpanY = static_cast<int>(client.bottom - client.top);
            const int cellWidth = haveFont
                ? static_cast<int>(fontInfo.dwFontSize.X)
                : std::max(1, clientSpanX / std::max(1, viewportColumns));
            const int cellHeight = haveFont
                ? static_cast<int>(fontInfo.dwFontSize.Y)
                : std::max(1, clientSpanY / std::max(1, viewportRows));
            const int viewportWidth = viewportColumns * cellWidth;
            const int viewportHeight = viewportRows * cellHeight;
            const int contentX = clientOrigin.x + std::max(0, (clientSpanX - viewportWidth) / 2);
            const int contentY = clientOrigin.y + std::max(0, clientSpanY - viewportHeight);
            const int cellX = (pointer.x - contentX) / cellWidth;
            const int cellY = (pointer.y - contentY) / cellHeight;
            const bool insideGame = pointer.x >= contentX
                && pointer.y >= contentY
                && cellX >= 0 && cellX < viewportColumns
                && cellY >= 0 && cellY < viewportRows;
            if (insideGame)
            {
                mouseCell.x = std::clamp(cellX, 0, std::max(0, viewportColumns - 1));
                mouseCell.y = std::clamp(cellY, 0, std::max(0, viewportRows - 1));
            }

            const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (insideGame && leftDown && !leftButtonDown) state.leftClicked = true;
            leftButtonDown = leftDown;
        }
        else
        {
            // 창 밖일 때도 버튼 상태는 따라간다. 안 그러면 밖에서 눌렀다 들어온
            // 버튼이 들어오는 순간 클릭 한 번으로 잡힌다.
            leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        }
    }

    // 콘솔 입력 큐에 쌓인 마우스 이벤트를 비운다. 주 경로다.
    void TerminalSession::PollWin32Mouse(InputState& state)
    {
        DWORD pending = 0;
        INPUT_RECORD records[64]{};
        CONSOLE_SCREEN_BUFFER_INFO screenInfo{};
        const bool hasScreenInfo = GetConsoleScreenBufferInfo(output, &screenInfo) != FALSE;
        const int viewportLeft = hasScreenInfo ? screenInfo.srWindow.Left : 0;
        const int viewportTop = hasScreenInfo ? screenInfo.srWindow.Top : 0;
        while (GetNumberOfConsoleInputEvents(input, &pending) && pending > 0)
        {
            DWORD read = 0;
            const DWORD count = std::min<DWORD>(pending, 64);
            if (!ReadConsoleInputW(input, records, count, &read)) break;
            for (DWORD index = 0; index < read; ++index)
            {
                if (records[index].EventType != MOUSE_EVENT) continue;
                const MOUSE_EVENT_RECORD& mouse = records[index].Event.MouseEvent;
                const int viewportX = static_cast<int>(mouse.dwMousePosition.X) - viewportLeft;
                const int viewportY = static_cast<int>(mouse.dwMousePosition.Y) - viewportTop;
                // **뷰포트 셀을 있는 그대로 돌려준다.** 프레임 좌표로의 환산은
                // Game::Update() 한 곳에서만 한다. 여기서도 스케일하면 이중 스케일이
                // 되어 조준이 큰 창의 왼쪽 일부에 갇힌다. 실제 회귀였다 (6c97938).
                mouseCell.x = std::clamp(viewportX, 0, std::max(0, viewportColumns - 1));
                mouseCell.y = std::clamp(viewportY, 0, std::max(0, viewportRows - 1));
                if (mouse.dwEventFlags == 0 && (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0)
                {
                    state.leftClicked = true;
                }
            }
        }
    }

    namespace
    {
        // snprintf 없이 십진수를 쓴다. 프레임당 수만 셀이면 포맷 파서가 프로파일에
        // 자기 이름으로 올라온다.
        char* WriteUInt(char* out, unsigned value)
        {
            char digits[10];
            int length = 0;
            do { digits[length++] = static_cast<char>('0' + value % 10u); value /= 10u; } while (value);
            while (length > 0) *out++ = digits[--length];
            return out;
        }

        // 트루컬러 SGR 의 몸통. 한 번만 적는다. 전경과 배경은 7바이트 도입부만
        // 다르고 나머지가 같아서, 다른 그것만 인자로 받는다.
        char* WriteColor(char* out, const char* introducer, Rgb color)
        {
            std::memcpy(out, introducer, 7); out += 7;
            out = WriteUInt(out, color.r); *out++ = ';';
            out = WriteUInt(out, color.g); *out++ = ';';
            out = WriteUInt(out, color.b); *out++ = 'm';
            return out;
        }

        // 전경과 배경을 **따로** 쓰는 이유는 함수가 둘이어서가 아니라, 셀 하나가
        // 둘 중 하나만 바꾸는 경우가 아주 흔하기 때문이다 — 벽의 윗절반은 그대로인데
        // 그 아래 바닥만 움직인다. 매번 쌍으로 내보내면 이스케이프 바이트가 공짜로
        // 두 배가 되고, 반블록 문자를 뺀 출력의 대부분이 그 이스케이프다.
        char* WriteForeground(char* out, Rgb color) { return WriteColor(out, "\x1b[38;2;", color); }
        char* WriteBackground(char* out, Rgb color) { return WriteColor(out, "\x1b[48;2;", color); }

        // 절대 커서 이동. 행마다 하나씩 들어간다.
        char* WriteCursorMove(char* out, int row, int column)
        {
            *out++ = '\x1b';
            *out++ = '[';
            out = WriteUInt(out, static_cast<unsigned>(row));
            *out++ = ';';
            out = WriteUInt(out, static_cast<unsigned>(column));
            *out++ = 'H';
            return out;
        }
    }

    // 프레임 하나를 이스케이프 문자열로 조립한다.
    //
    // 셀 하나가 픽셀 두 개다. 위 절반이 전경색, 아래 절반이 배경색이고 문자는 항상
    // 반블록(U+2580)이다.
    //
    // **여기에 셀당 힙 할당을 다시 넣지 마라.** 예전 조립기는 셀마다 std::string 을
    // 만들고 격자 크기의 vector<bool> 을 만들었다. 7만 셀에서 그 값이 그 셀들을 채운
    // 레이캐스트보다 비쌌다. QHD 최대화 22FPS 의 원인이 렌더도 터미널도 아닌 이것이었다.
    std::string TerminalSession::ComposeFrame(const PixelBuffer& pixels,
        int contentColumns, int contentRows, int originColumn, int originRow)
    {
        const int columns = std::max(0, contentColumns);
        const int rows = std::max(0, contentRows);
        if (columns == 0 || rows == 0) return {};

        // 프레임 사이에 재사용한다.
        thread_local std::vector<char> scratch;
        // 셀 하나의 최악: 트루컬러 코드 둘과 반블록. 행 하나당 절대 커서 이동 하나.
        const std::size_t capacity = static_cast<std::size_t>(columns) * rows * 44
            + static_cast<std::size_t>(rows) * 16 + 64;
        if (scratch.size() < capacity) scratch.resize(capacity);
        char* out = scratch.data();

        const int frameWidth = std::max(1, pixels.Width());
        const int frameHeight = std::max(1, pixels.Height());
        const Rgb* raw = pixels.Pixels().data();
        const int targetPixelHeight = rows * 2;
        Rgb previousTop{};
        Rgb previousBottom{};
        // 커서를 방금 옮겼고 호스트의 색 상태를 알 수 없으므로, 프레임의 첫 셀은
        // 언제나 자기 색을 쓴다. 이 false 가 없으면 프레임 첫 셀이 지난 프레임의
        // 색을 물려받는다.
        bool haveColor = false;
        for (int cellY = 0; cellY < rows; ++cellY)
        {
            const int topY = std::clamp(cellY * 2 * frameHeight / targetPixelHeight, 0, frameHeight - 1);
            const int bottomY = std::clamp((cellY * 2 + 1) * frameHeight / targetPixelHeight, 0, frameHeight - 1);
            const Rgb* topRow = raw + static_cast<std::size_t>(topY) * frameWidth;
            const Rgb* bottomRow = raw + static_cast<std::size_t>(bottomY) * frameWidth;
            // 행마다 절대 위치로 옮긴다. 그래서 상한이 걸린 프레임 바깥 여백에는
            // 한 바이트도 안 쓴다. 여백까지 칠하면 상한이 존재할 이유가 사라진다.
            out = WriteCursorMove(out, originRow + cellY + 1, originColumn + 1);
            for (int x = 0; x < columns; ++x)
            {
                const int sourceX = std::clamp(x * frameWidth / columns, 0, frameWidth - 1);
                const Rgb top = topRow[sourceX];
                const Rgb bottom = bottomRow[sourceX];
                // 색 코드는 **바뀔 때만** 쓴다.
                if (!haveColor || !(top == previousTop))
                {
                    out = WriteForeground(out, top);
                    previousTop = top;
                }
                if (!haveColor || !(bottom == previousBottom))
                {
                    out = WriteBackground(out, bottom);
                    previousBottom = bottom;
                }
                haveColor = true;
                std::memcpy(out, "\xE2\x96\x80", 3);
                out += 3;
            }
        }
        return std::string(scratch.data(), static_cast<std::size_t>(out - scratch.data()));
    }

    // 프레임을 화면에 낸다. 배치가 움직였을 때만 한 번 지운다.
    void TerminalSession::Present(const PixelBuffer& pixels)
    {
        RefreshViewportSize();
        const int contentColumns = ContentColumns();
        const int contentRows = ContentRows();
        const int originColumn = (viewportColumns - contentColumns) / 2;
        const int originRow = (viewportRows - contentRows) / 2;
        if (contentColumns != presentedColumns || contentRows != presentedRows
            || originColumn != presentedOriginColumn || originRow != presentedOriginRow)
        {
            // 배치가 바뀔 때 한 번이고 그 뒤로는 없다. 어떤 프레임도 여백에 쓰지
            // 않으므로 다른 것이 여백을 더럽힐 수 없다.
            Write("\x1b[0m\x1b[2J");
            presentedColumns = contentColumns;
            presentedRows = contentRows;
            presentedOriginColumn = originColumn;
            presentedOriginRow = originRow;
        }
        Write(ComposeFrame(pixels, contentColumns, contentRows, originColumn, originRow));
    }

    // 창 제목. 호스트 창을 못 찾았을 때만 찾기용 토큰을 뒤에 붙인다.
    void TerminalSession::SetTitle(const std::string& title)
    {
        const std::string suffix = hostWindow ? std::string{} : " | " + windowTitleToken;
        Write("\x1b]0;" + title + suffix + "\x07");
    }

    void TerminalSession::Write(std::string_view text) const
    {
        DWORD written = 0;
        WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
}
