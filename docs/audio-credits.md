# 오디오 자산 출처와 라이선스

이 문서는 `Assets/Sound/`의 모든 파일에 대한 출처 기록이다. **자산을 추가할 때마다
아래 표에 한 행을 추가한다.** 출처가 확인되지 않은 파일은 표에 `미확인 — 사용자 확인
필요`로 남기고 **라이선스를 임의로 단정하지 않는다.**

게임 안에서 쓰는 파일명은 큐 이름이라 고정이고 원본 파일명과 다르다. 그래서 이 표가
없으면 어떤 소리가 어디서 왔는지 되짚을 방법이 사라진다.

## 1. 자산 목록

| 게임 내 파일명 | 원본 파일명 | 제목 | 출처 URL | 업로더 | 원저작자 | 라이선스 | 크레딧 의무 | 확인일 |
|---|---|---|---|---|---|---|---|---|
| `gunshot.wav` | `flutie8211-gun-shot-and-echo-549910.mp3` | gun shot and echo | https://pixabay.com/sound-effects/film-special-effects-gun-shot-and-echo-549910/ | flutie8211 | flutie8211 | Pixabay Content License | 없음 (권장) | 2026-08-28 |
| `ammo_pickup.wav` | `freesound_community-pistol-cock-6014.mp3` | Pistol Cock | https://pixabay.com/sound-effects/film-special-effects-pistol-cock-6014/ | freesound_community | nebulasnails (Freesound) | Pixabay Content License | 없음 (권장) | 2026-08-28 |
| `window_break.wav` | Pixabay 다운로드 | glass breaking | https://pixabay.com/ko/sound-effects/영화-및-특수-효과-glass-breaking-386153/ | 미확인 — 사용자 확인 필요 | 미확인 — 사용자 확인 필요 | Pixabay Content License | 없음 (권장) | 2026-08-29 |
| `door_open.wav` | `soundreality-opening-door-411632.mp3` | opening door | https://pixabay.com/ko/sound-effects/가정-opening-door-411632/ | soundreality | soundreality (파일명 기준) | Pixabay Content License | 없음 (권장) | 2026-08-29 |
| `zombie_growl.wav` | Pixabay 다운로드 | zombie sound | https://pixabay.com/ko/sound-effects/공포-zombie-sound-357975/ | 미확인 — 사용자 확인 필요 | 미확인 — 사용자 확인 필요 | Pixabay Content License | 없음 (권장) | 2026-08-29 |
| `footstep.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `key_pickup.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `escaped.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `dead.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `breach_knock.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `door_break.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |
| `bgm_loop.wav` | — | — | Google Gemini 로 생성 (아래 1.1) | — | — | MIT License | MIT 고지 유지 | 2026-09-03 |

### 1.1 생성 자산 일곱 개 (2026-08-29)

`footstep` · `key_pickup` · `escaped` · `dead` · `breach_knock` · `door_break` ·
`bgm_loop` 은 **Google Gemini(gemini.google.com)로 2026-08-29에 생성**했다.

생성에 쓴 대화 링크는 개인 계정 기록이므로 저장소에 포함하지 않는다. 필요한 경우
원본 계정의 기록에서 확인한다.

2026-09-03에 확인한 [Google Terms of Service](https://policies.google.com/terms?hl=ko)는
Google 서비스가 생성한 원본 콘텐츠에 Google이 소유권을 주장하지 않는다고 명시한다.
Gemini 웹·모바일 앱에서 만든 위 일곱 파일은 상업적 프로젝트에 사용할 수 있다.
이 저장소의 저작권자가 보유할 수 있는 권리의 범위에서 위 일곱 파일을 저장소의
MIT License로 함께 제공한다.

단, 이용자는 Google 약관과
[생성형 AI 금지 사용 정책](https://policies.google.com/terms/generative-ai/use-policy?hl=ko)을
준수하고 제3자의 저작권·상표권·초상권 등 다른 권리를 침해하지 않을 책임이 있다.
이 기록은 해당 책임을 대신하거나 제3자 권리가 없음을 보증하지 않는다.

프롬프트 조건은 공통으로 이랬다 — 16비트 PCM WAV 48kHz, 페이드인 없음, 앞 무음 없음,
멜로디 없음, 드라이하고 근접, 피크 −3 dBFS 정규화. **일곱 개 전부 정확히 −3.0 dBFS로
나왔다.** 조건이 그대로 지켜졌다는 뜻이다.

### 큐 열두 개 규격 — 전부 파일이 있다 (2026-08-29)

빈 슬롯이 없다. 아래는 각 큐가 무엇이고 언제 나는지의 표다. 자산을 갈아끼울 때
종류와 게인은 이 표가 기준이다. 이름 그대로 `Assets/Sound/`에
떨궈 넣으면 그 큐가 소리를 내기 시작하고, **코드는 아무것도 안 고쳐도 된다.**

| 파일명 | 종류 | 큐 게인 | 언제 |
|---|---|---|---|
| `footstep.wav` | Ui | 0.45 | 보폭마다. **걷기와 달리기가 같은 파일**을 쓰고, 크기는 그 걸음이 낸 소음에 비례한다 |
| `key_pickup.wav` | Ui | 0.90 | 열쇠 획득 |
| `escaped.wav` | Ui | 1.00 | 탈출 |
| `dead.wav` | Ui | 1.00 | 사망 |
| `breach_knock.wav` | World | 1.00 | 좀비가 침입구를 두드릴 때 |
| **`door_open.wav`** | **World** | **0.85** | **문이 열릴 때. 플레이어가 `E` 로 연 것과 좀비가 밀고 나온 것 둘 다** |
| `door_break.wav` | World | 1.00 | 막힌 문 파손 |
| `window_break.wav` | World | 1.00 | 창문 파손 |
| `zombie_growl.wav` | World | 0.90 | 좀비가 소리를 듣고 깨어날 때 |
| `bgm_loop.wav` | Music | 1.00 | 판 시작부터 루프 |

**`World` 큐는 로드할 때 모노로 다운믹스된다.** 메모리가 아니라 패닝 때문이다 —
`SetOutputMatrix`가 `1x2`여야 좌우 이득을 줄 수 있다. 스테레오로 넣어도 알아서 합쳐지지만
모노 소스면 그만큼 손실이 없다. `Ui`와 `Music`은 스테레오 그대로 쓴다.

형식은 **PCM 16비트 WAV**. 샘플레이트는 큐마다 달라도 된다 — 보이스 풀이 `(레이트, 채널)`
별로 나뉘어 있다.

**`ammo_pickup`의 업로더와 원저작자가 다르다.** `freesound_community`는 Pixabay가
Freesound 음원을 일괄 등록할 때 쓰는 계정이지 저작자가 아니다. 실제 저작자는
`nebulasnails`이고 이 정보는 **파일명에 나오지 않는다.** 지우지 마라.

두 파일 모두 원본은 mp3였고 ffmpeg(Lavf58.76.100)으로 PCM wav 변환했다.
**리샘플·다운믹스 없음** — 원본의 샘플레이트와 채널이 그대로다.

## 2. 실측 포맷

헤더를 직접 뜯어 확인한 값이다. 파서 설계가 여기서 나왔다.

| 파일 | 태그 | 채널 | 레이트 | 비트 | data 크기 | 길이 | data 페이로드 위치 |
|---|---|---|---|---|---|---|---|
| `gunshot.wav` | 1 (PCM) | 2 | 48000 | 16 | 967,680 B | 5.04초 | `0x4E` |
| `ammo_pickup.wav` | 1 (PCM) | 2 | 24000 | 16 | 80,640 B | 0.84초 | `0x4E` |
| `footstep.wav` | 1 (PCM) | 1 | 48000 | 16 | 19,200 B | 0.20초 | `0x2C` |
| `key_pickup.wav` | 1 (PCM) | 1 | 48000 | 16 | 38,400 B | 0.40초 | `0x2C` |
| `escaped.wav` | 1 (PCM) | 1 | 48000 | 16 | 192,000 B | 2.00초 | `0x2C` |
| `dead.wav` | 1 (PCM) | 1 | 48000 | 16 | 192,000 B | 2.00초 | `0x2C` |
| `breach_knock.wav` | 1 (PCM) | 1 | 48000 | 16 | 57,600 B | 0.60초 | `0x2C` |
| `door_break.wav` | 1 (PCM) | 1 | 48000 | 16 | 96,000 B | 1.00초 | `0x2C` |
| `door_open.wav` | 1 (PCM) | 1 | 48000 | 16 | 91,200 B | 0.95초 | `0x4E` |
| `window_break.wav` | 1 (PCM) | 1 | 44100 | 16 | 88,200 B | 1.00초 | `0x4E` |
| `zombie_growl.wav` | 1 (PCM) | 1 | 44100 | 16 | 176,400 B | 2.00초 | `0x4E` |
| `bgm_loop.wav` | 1 (PCM) | 1 | 48000 | 16 | 2,592,000 B | 27.00초 | `0x2C` |

**둘 다 `fmt`와 `data` 사이에 26바이트 `LIST` 청크가 있다.** 그래서 `data`가 흔히
가정하는 `0x2C`가 아니라 `0x4E`에서 시작한다. 고정 오프셋 파서는 이 두 파일에서 즉시
깨진다.

생성 자산 일곱 개는 `LIST` 없이 `data`가 `0x2C`에서 바로 시작한다. Pixabay 다섯 개는 전부
`0x4E`다. **한 저장소 안에 두 배치가 공존하므로 청크 순회는 선택이 아니다.**

**샘플레이트가 서로 다르다(48k / 44.1k / 24k).** XAudio2 소스 보이스는 생성 시 포맷이 고정이라
48k 보이스에 24k 버퍼를 넣을 수 없다. 그래서 보이스 풀이 포맷별로 나뉘어 있다.

## 3. Pixabay Content License 요약

2026-09-03에 [Pixabay Content License](https://pixabay.com/service/license-summary/)에서
다시 확인했다.

**허용**

- 무료 사용
- 저작자 표시 **불필요**(권장일 뿐)
- 수정·개작
- 상업적 사용

**금지**

- 콘텐츠를 실질적으로 변경하지 않은 채 **그 자체로 판매·배포**
- 오해를 부르는 사용
- 상표·상호로의 사용

게임에 효과음으로 넣어 배포하는 것은 허용 범위다. 다만 Pixabay 콘텐츠를 실질적인
변경 없이 파일 자체로 판매하거나 단독 배포할 수는 없다. 크레딧 의무는 없지만 이 문서를
유지하는 것은 의무와 무관하게 필요하다 — 자산이 늘면 출처를 되짚을 수 없게 된다.

## 3.5 받은 뒤 다듬은 것 (2026-08-29)

Pixabay 두 파일은 **받은 그대로는 게임에 맞지 않아서 다듬었다.** 원본을 어떻게 바꿨는지
남긴다 — 나중에 소리가 이상하면 여기가 첫 용의자다.

### `window_break.wav`

원본은 2.12초인데 **앞 0.27초가 무음**이었다. 창문이 깨진 순간 소리가 그만큼 늦게 나온다.
유리 깨지는 것은 즉발이라 이 지연은 체감된다. 뒤 1.14초도 무음이라, 2.12초 중 실제 소리는
0.71초뿐이었다.

```bash
ffmpeg -i 원본.wav -ss 0.25 -t 1.0 -ac 1 -c:a pcm_s16le 출력.wav
ffmpeg -i 출력.wav -af "volume=3.47dB" -c:a pcm_s16le window_break.wav
```

### `zombie_growl.wav`

원본은 **8.10초**에 피크가 **−11 dBFS**로 자산 중 가장 조용했다. 둘 다 문제였다.

- 좀비가 깨어날 때마다 재생되는데, 한 발 쏘면 여러 마리가 동시에 깬다. 8초짜리가 겹친다.
- 게다가 **볼륨은 재생 시점의 거리로 고정되고 갱신되지 않는다.** 8초 동안 플레이어가
  움직이면 그 볼륨은 이미 낡은 값이다.
- World 큐라 음향 필드로 또 깎인다. 가장 조용한 파일이 가장 많이 깎이는 자리에 있었다.

**큐 게인으로는 못 고친다.** `sfx_volume`이 1.0이고 `SetVolume`이 1.0에서 잘리므로
표에서 올릴 여지가 없다. 파일을 정규화하는 것 외에 방법이 없었다.

```bash
ffmpeg -i 원본.wav -ss 0.10 -t 2.0 -ac 1 -af "afade=t=out:st=1.75:d=0.25" -c:a pcm_s16le 출력.wav
ffmpeg -i 출력.wav -af "volume=12.21dB" -c:a pcm_s16le zombie_growl.wav
```

끝의 0.25초 페이드아웃은 2초에서 잘라낸 자리를 덮는다. 없으면 끊긴 소리가 툭 멎는다.

### `door_open.wav`

원본은 2.04초 스테레오인데 **실제 소리는 0.91초**뿐이었다. 앞 0.14초가 무음이고 **뒤
0.99초가 통째로 무음**이다. 앞 무음은 창문과 같은 문제다 — `E` 를 누른 순간 소리가 그만큼
늦게 나온다.

0.1초 구간별 피크를 보면 소리가 **두 덩어리**다. 0.3~0.5초에 큰 것(손잡이), 0.7~0.9초에
작은 것(경첩). 문이 열리는 사건은 그 둘 다이므로 **둘을 모두 살려서** 잘랐다.

피크가 **−0.1 dBFS** 로 자산 중 가장 뜨거웠다. 다른 것들이 −3 에 서 있으므로 **내렸다.**
정규화는 올리기만 하는 일이 아니다.

```bash
ffmpeg -i 원본.mp3 -ac 1 -c:a pcm_s16le 모노.wav
ffmpeg -i 모노.wav -ss 0.13 -t 0.95 -c:a pcm_s16le 잘라낸.wav
ffmpeg -i 잘라낸.wav -af "volume=-2.90dB" -c:a pcm_s16le door_open.wav
```

### 게인은 짐작이 아니라 실측이다

`3.47dB`와 `12.21dB`는 **자르고 모노로 만든 뒤 그 결과의 피크를 다시 재서** 계산했다.
모노 다운믹스는 채널을 평균하므로 피크가 내려간다 — 원본 피크로 계산했다면 둘 다 목표보다
어두웠을 것이다.

목표는 **−3.0 dBFS**, 생성 자산 일곱 개가 서 있는 그 값이다. 결과는 각각 −3.0 dBFS로
맞았다. 자산이 서로 다른 크기로 도착하면 큐 게인 표가 아니라 **여기서** 맞추는 편이 낫다.
게인 표는 "이 소리는 다른 것보다 작아야 한다"는 의도를 적는 자리이지, 자산 편차를 메우는
자리가 아니다.

**원본은 저장소에 없다.** 되돌리려면 Pixabay 페이지에서 다시 받아 위 명령을 다시 돌린다.

### 안 건드린 것 둘

전수 측정에서 눈에 띄었지만 **손대지 않기로 한 것**도 적어둔다. 다음 사람이 같은 표를 보고
같은 의심을 하게 될 것이므로.

- **`gunshot.wav` 는 피크가 0.0 dBFS 다.** 왜곡처럼 보이지만 풀스케일에 닿은 샘플이
  483,840개 중 **4개**다. 들리는 왜곡이 아니라 천장을 스친 것이고, 소리는 멀쩡하다.
  정규화하면 멀쩡한 파일을 바꾸는 것이 된다.
- **`ammo_pickup.wav` 는 −4.6 dBFS 로 조금 조용하다.** 큐 게인이 0.90 이라 충분히 들린다.
  자산 편차를 여기서 메우는 것은 실제로 안 들릴 때의 이야기다.

## 4. 새 자산을 추가하는 방법

### 4.1 변환

```bash
ffmpeg -i 입력.mp3 -c:a pcm_s16le 출력.wav
```

```bash
ffmpeg -i 입력.mp3 -ac 1 -c:a pcm_s16le 출력.wav
```

첫 번째는 `Ui`·`Music` 큐용이다. 두 번째는 `World` 큐용으로 모노로 받는다 — 코드가
로드할 때 어차피 다운믹스하므로 처음부터 모노면 파일이 절반이다.

파서가 받는 것은 **PCM 16비트뿐**이다(`fmt` 태그 `1` 또는 `0xFFFE`). 샘플레이트와
채널 수는 무엇이든 된다.

### 4.2 규칙

1. 파일을 `Assets/Sound/`에 **큐 이름 그대로** 넣는다. 이름은
   `ConsoleZombie/Game/AudioCues.cpp`의 표에 있다.
2. **위 1절 표에 한 행을 추가한다.** 출처 URL, 업로더, 원저작자, 라이선스, 확인일.
3. **빌드한다.** 자산은 `$(OutDir)Assets\Sound\`로 복사되고, exe 옆 사본이 저장소
   원본보다 먼저 발견된다. 파일만 바꾸고 빌드하지 않으면 낡은 소리가 계속 난다.
   `Config/GameBalance.ini`와 똑같은 함정이다.

### 4.3 출처를 모르는 자산

표에 `미확인 — 사용자 확인 필요`로 남긴다. **라이선스를 추측해서 적지 않는다.**
출처가 불분명한 자산을 배포물에 넣는 것과, 넣어 놓고 출처를 안다고 적는 것은
다른 문제이고 후자가 더 나쁘다.
