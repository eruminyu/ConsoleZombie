#include "Input.h"
#include <cassert>
#include <Windows.h>

namespace Craft
{
	// static 변수 초기화.
	Input* Input::instance = nullptr;

	Input::Input()
	{
		// 시작할 때 instance 값은 null이어야 함.
		assert(!instance && "instance should be null here.");
		instance = this;

		// 콘솔 입력 버퍼의 핸들을 가져옴.
		inputHandle = GetStdHandle(STD_INPUT_HANDLE);

		// 콘솔 입력 핸들을 가져오지 못했으면 종료.
		if (inputHandle == INVALID_HANDLE_VALUE || inputHandle == nullptr)
		{
			return;
		}

		// 현재 콘솔 입력 모드를 가져옴.
		if (GetConsoleMode(inputHandle, &originalConsoleMode))
		{
			// 변경할 콘솔 입력 모드 설정.
			DWORD inputMode = originalConsoleMode;

			// 마우스 이벤트 입력 활성화.
			inputMode |= ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT;

			// 빠른 편집 모드가 활성화되어 있으면 마우스 입력이
			// 콘솔의 드래그 선택 기능으로 처리되므로 비활성화.
			inputMode &= ~ENABLE_QUICK_EDIT_MODE;

			// 변경한 콘솔 입력 모드를 적용하고 성공 여부 저장.
			shouldRestoreConsoleMode = SetConsoleMode(inputHandle, inputMode) != FALSE;
		}
	}

	Input::~Input()
	{
		// 입력 모드 변경에 성공했으면 기존 콘솔 입력 모드로 복구.
		if (shouldRestoreConsoleMode)
		{
			SetConsoleMode(inputHandle, originalConsoleMode);
		}

		// 전역 접근 변수 정리.
		instance = nullptr;
	}

	bool Input::GetKeyDown(int keyCode) const
	{
		return !keyStates[keyCode].wasKeyDown
			&& keyStates[keyCode].isKeyDown;
	}

	bool Input::GetKeyUp(int keyCode) const
	{
		return keyStates[keyCode].wasKeyDown
			&& !keyStates[keyCode].isKeyDown;
	}

	bool Input::GetKey(int keyCode) const
	{
		return keyStates[keyCode].isKeyDown;
	}

	Input& Input::Get()
	{
		// 여기에서 instance는 null이면 안됨.
		assert(instance && "instance should not be null here");
		return *instance;
	}

	void Input::ProcessInput()
	{
		// 콘솔 입력 핸들이 유효하지 않으면 입력 처리 종료.
		if (inputHandle == INVALID_HANDLE_VALUE || inputHandle == nullptr)
		{
			return;
		}

		// 한 번에 읽어올 콘솔 입력 이벤트 배열.
		INPUT_RECORD records[128] = { };

		// 콘솔 입력 버퍼에 대기 중인 이벤트 수.
		DWORD pendingEventCount = 0;

		// 한 프레임 동안 입력 버퍼에 쌓인 이벤트를 모두 처리.
		while (GetNumberOfConsoleInputEvents(inputHandle, &pendingEventCount)
			&& pendingEventCount > 0)
		{
			// 실제로 읽어온 이벤트 수를 저장할 변수.
			DWORD readEventCount = 0;

			// 배열 크기를 넘지 않도록 한 번에 읽을 이벤트 수 결정.
			const DWORD readCount = pendingEventCount < 128 ? pendingEventCount : 128;

			// 콘솔 입력 버퍼에서 이벤트 읽기.
			if (!ReadConsoleInput(inputHandle, records, readCount, &readEventCount))
			{
				break;
			}

			// 읽어온 입력 이벤트를 순서대로 처리.
			for (DWORD ix = 0; ix < readEventCount; ++ix)
			{
				// 현재 처리할 입력 이벤트.
				const INPUT_RECORD& record = records[ix];

				// 입력 이벤트 종류에 따라 처리.
				switch (record.EventType)
				{
				case KEY_EVENT:
				{
					// 키보드 이벤트 정보 가져오기.
					const KEY_EVENT_RECORD& keyEvent = record.Event.KeyEvent;

					// 입력된 키의 가상 키 코드 가져오기.
					const WORD keyCode = keyEvent.wVirtualKeyCode;

					// 관리하는 키 배열 범위 안에 있는지 확인.
					if (keyCode < keyCount)
					{
						// 키가 눌렸는지 또는 해제됐는지 현재 상태에 저장.
						KeyState& state = keyStates[keyCode];
						const bool isKeyDown = keyEvent.bKeyDown != FALSE;
						state.isKeyDown = isKeyDown;
					}
					break;
				}

				case MOUSE_EVENT:
				{
					// 마우스 이벤트 정보 가져오기.
					const MOUSE_EVENT_RECORD& mouseEvent = record.Event.MouseEvent;

					// 마우스 포인터의 콘솔 셀 좌표 저장.
					mousePosition.x = mouseEvent.dwMousePosition.X;
					mousePosition.y = mouseEvent.dwMousePosition.Y;

					// 마우스 버튼과 가상 키 코드를 연결하기 위한 구조체.
					const struct MouseButton
					{
						// 키 상태 배열에서 사용할 가상 키 코드.
						int keyCode;

						// 마우스 이벤트에서 버튼 상태를 확인할 비트 값.
						DWORD buttonMask;
					} mouseButtons[] = {
						{ VK_LBUTTON, FROM_LEFT_1ST_BUTTON_PRESSED },
						{ VK_RBUTTON, RIGHTMOST_BUTTON_PRESSED },
						{ VK_MBUTTON, FROM_LEFT_2ND_BUTTON_PRESSED }
					};

					// 왼쪽, 오른쪽, 가운데 마우스 버튼 상태 처리.
					for (const MouseButton& button : mouseButtons)
					{
						// 버튼이 눌렸는지 비트 연산으로 확인한 후 키 상태에 저장.
						KeyState& state = keyStates[button.keyCode];
						const bool isKeyDown =
							(mouseEvent.dwButtonState & button.buttonMask) != 0;
						state.isKeyDown = isKeyDown;
					}
					break;
				}

				case FOCUS_EVENT:
					// 콘솔 창이 입력 포커스를 잃었는지 확인.
					if (!record.Event.FocusEvent.bSetFocus)
					{
						// 포커스를 잃는 동안 KeyUp 이벤트가 누락되어
						// 키가 계속 눌린 상태로 남는 것을 방지.
						for (KeyState& state : keyStates)
						{
							state.isKeyDown = false;
						}
					}
					break;
				}
			}
		}
	}

	void Input::SavePreviousStates()
	{
		// 이전 프레임 입력 값 저장.
		for (KeyState& state : keyStates)
		{
			// 현재 프레임 입력 값을 이전 프레임 값으로 저장.
			state.wasKeyDown = state.isKeyDown;
		}
	}
}
