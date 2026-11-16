# pedrv

WinPE에서 실행하여 **현재 PC의 하드웨어에 맞는 드라이버만** 골라 오프라인 Windows(예: `C:\`)에 설치하는 콘솔 프로그램입니다.

- 드라이버 소스: INF가 들어 있는 폴더, `.zip`, `.7z`, `.wim`
- 런타임 의존성 없는 단일 exe (정적 CRT, .NET/PowerShell 불필요)
- 콘솔 메시지는 영어입니다 (WinPE에는 보통 한글 글꼴이 없음)

## 문서 구성

- `README.md`: 사용자용 요약 (빠른 시작, 옵션 표, 주의 사항). 세부 동작은 이 문서로 링크합니다. 저장소의 기본 README는 한국어입니다.
- `README.en.md`: `README.md`의 영어판. 두 문서 첫머리에서 서로 링크합니다. README를 고치면 영어판도 같이 고칩니다.
- `CLAUDE.md` (이 문서): 전체 동작 설명, 설계 결정과 근거, 개발·배포 규칙.
- `docs/`: 조사 기록과 긴 목록 (`pe-24h2-display-driver.md`, `slim-display-drivers.md`).
- 동작이나 옵션을 바꾸면 이 문서를 먼저 고치고, 사용자에게 보이는 부분(옵션 표, 주의 사항, 대표 수치)은 README와 README.en.md에도 반영합니다. 코드의 `Usage()` 도움말도 함께 맞춥니다.

## 사용법

```
pedrv [옵션] <소스> [<소스> ...]
```

| 옵션 | 설명 |
|---|---|
| `/image:<경로>` | 오프라인 Windows 루트 (예: `C:\`). 생략하면 고정 디스크에서 자동 탐지 |
| `/pe` | 오프라인 Windows 대신 **실행 중인 WinPE**에 설치 (`drvload` 사용). `/image`와 함께 쓸 수 없음 |
| `/fromwin[:<경로>]` | 오프라인 Windows의 드라이버 저장소를 소스로 써서 WinPE에 설치 (`/pe` 포함). `/inbox`를 붙이면 Windows 기본 드라이버도 포함 |
| `/list` | 매칭되는 드라이버만 표시하고 설치하지 않음 |
| `/all` | 하드웨어 매칭 없이 소스의 모든 드라이버 설치 |
| `/allmatches` | 매칭되는 드라이버를 전부 설치. 기본은 장치마다 가장 적합한 드라이버 하나만 (ID 순위 → 날짜 → 버전). 예전 `/best`는 이제 기본 동작이며 붙여도 무방 |
| `/hwonly` | 하드웨어 ID만 매칭 (호환 ID 무시) |
| `/exclude:<클래스,...>` | 지정한 장치 클래스 제외 (예: `/exclude:Display,Media`). 기본은 제외 없음 |
| `/slim` | WinPE에 올릴 때 항상 축소 사본을 사용. 지정하지 않으면 RAM 디스크에 들어가지 않는 패키지만 축소 (아래 "드라이버 축소" 참고) |
| `/slim:max` | `/slim`에 더해 NVIDIA·AMD의 OpenGL·Vulkan 드라이버도 비움. `/export`에도 적용 (`/slim`은 `/export`에서 효과 없음) |
| `/export:<폴더>` | 설치하지 않고, 선택된 드라이버 패키지의 축소 사본을 폴더에 저장. `/keep32`를 붙이면 32비트 파일을 남김. 부팅한 PE에서 올리는 용도이며, PE 이미지에 미리 통합하면 동작하지 않음 (아래 "드라이버 축소" 참고) |
| `/forceunsigned` | 서명 없는 드라이버 허용 |
| `/scratch:<폴더>` | 작업 폴더 (기본: `<image>\$PeDrv`, `/fromwin`은 그 Windows 드라이브의 `$PeDrv`, `/pe`는 PE를 부팅한 드라이브의 `\Temp\PeDrv`, 그 외 또는 쓸 수 없으면 `%TEMP%\PeDrv`. 종료 시 삭제) |
| `/7z:<경로>` | 7z.exe / 7za.exe / 7zr.exe 파일 또는 그 파일이 있는 폴더. 생략하면 ① pedrv.exe와 같은 폴더, ② 대상 Windows(`/image`, `/fromwin`) 드라이브의 `Program Files\7-Zip`(실행 중인 시스템과 아키텍처가 같을 때만), ③ 시스템 드라이브의 `Program Files\7-Zip` 순으로 검색 |
| `/log:<파일>` | DISM 로그 파일 |
| `/version` | 버전 표시. 실행할 때마다 첫 줄에도 버전과 빌드 날짜를 출력합니다 |

현재 모드에서 효과가 없는 옵션(예: `/fromwin` 없는 `/inbox`, `/pe`에서의 `/log`)은 `WARNING: ... has no effect`로 알려 줍니다. `/fromwin /all`은 그 Windows의 제조사 드라이버를 전부 PE에 올리므로 경고를 출력합니다.

소스를 생략하면 exe 옆의 `Drivers` 폴더를 사용합니다. 소스 폴더 바로 아래에 있는 압축 파일도 자동으로 소스에 포함됩니다. WIM의 이미지 번호는 `drivers.wim:2`처럼 지정합니다(기본 1).

드라이브 문자 대신 `?:`를 쓰면(`?:\PEDrivers`, `?:\packs\drivers.wim:2`) C:부터 Z:까지 차례로 찾아, 그 경로가 있는 첫 드라이브를 씁니다. 빈 드라이브에서 "디스크를 넣으세요" 창이 뜨지 않게 막습니다. 어느 드라이브에도 없으면 오류(종료 코드 2)입니다. 부팅 매체의 드라이브 문자가 PC마다 달라서 PE 시작 스크립트가 경로를 고정할 수 없기 때문에 만들었습니다 (`phoenixpe\PedrvDriverLoader.script`가 사용).

## PhoenixPE 스크립트

- `phoenixpe\DisplayDriverSupport.script`: `ForceBddFallbackOnly`를 0으로 설정하고 `DDisplay.dll`을 넣습니다 (아래 `/pe` 절 참고).
- `phoenixpe\PedrvDriverLoader.script`: pedrv를 PE에 넣고, 부팅한 PE에서 `pedrv /pe`로 드라이버를 올립니다.
  - 드라이버 위치는 둘 중 하나입니다. 부팅 매체나 디스크의 폴더(기본 `\PEDrivers` → `?:\PEDrivers`)이거나, PE 이미지에 파일로 포함(`%PEPrograms%\pedrv\<폴더 이름>`)입니다. 포함 방식은 DISM 통합이 아니라 파일 복사이므로 축소 사본도 동작합니다.
  - 자동 실행은 PhoenixAPI의 `AddAutoRun`(PreShell은 기다림, PostShell은 기다리지 않음)을 씁니다. 창 표시 방식은 네 가지입니다.
    - 최소화(기본): 숨긴 `cmd.exe /c start "" [/wait] /min "pedrv.exe" ...`로 실행해, pedrv 창이 작업 표시줄 단추로만 보입니다. `AddAutoRun`의 창 상태는 보통/숨김뿐이라 `start /min`을 씁니다.
    - 숨김: `cmd.exe /c "... > X:\Windows\Temp\pedrv.log 2>&1"`로 실행하고 출력은 로그 파일에 남깁니다.
    - 보통: `pedrv.exe`를 직접 실행합니다. 이전 버전 스크립트에 저장된 값(`Console - ...`)도 이 경우로 처리합니다.
    - 보통 + 키 입력 대기: `cmd.exe /c "... & pause"`로 실행합니다.
  - 제외할 장치 클래스는 체크 상자 12개(Display, Media, Net, Bluetooth, Camera, Biometric, HIDClass, USB, SCSIAdapter, HDC, System, SoftwareComponent)와 "기타" 입력란으로 고릅니다. `/exclude:Display;Media`처럼 `;`로 이어 붙입니다. 쉼표를 쓰면 PEBakery·AutoIt·cmd 중 어디서든 인수가 갈릴 수 있어, pedrv가 함께 받아 주는 `;`를 씁니다. 추가 옵션란에 `/exclude:`를 또 넣으면 나중 것만 적용되므로, 그 칸에는 넣지 않습니다.
  - "외부 pedrv.exe 사용"을 선택하면 지정한 파일을 `pedrv.exe`로 복사하고, 아키텍처 확인은 하지 않습니다. 새 빌드를 다시 인코딩하지 않고 쓰거나, x86·arm64 PE에 쓸 때 사용합니다.
  - 바로가기는 `AddShortcut`으로 `cmd.exe /k "..."`를 만들어, 실행 결과를 볼 수 있게 창을 남깁니다.
  - `/fromwin` 바로가기를 따로 만들 수 있습니다(바탕 화면 / 시작 메뉴, 이름 따로, 시작 메뉴 폴더는 공통). 명령은 `pedrv.exe /fromwin [/inbox] <제외 클래스> <추가 옵션>`입니다. 디스크의 Windows는 pedrv가 찾고, 여러 개면 목록을 보여 주고 멈춥니다. 창이 남아 있으므로 `/fromwin:C:\`로 다시 실행하면 됩니다.
  - 일반 바로가기와 `/fromwin` 바로가기에 각각 **디스플레이 제외** 바로가기를 추가할 수 있습니다("Also a shortcut without display drivers", 이름 따로). 같은 위치(바탕 화면 / 시작 메뉴 선택을 따름)에 만들어지며, 제외 클래스 목록에 `Display`를 더한 명령입니다 (`/exclude:Media;Display`처럼). 디스플레이 드라이버 때문에 화면이 멈추거나, 그래픽 드라이버는 이미 올렸고 나머지만 올리고 싶을 때 씁니다. 위의 제외 클래스에서 Display를 이미 골랐으면 같은 바로가기가 되므로 만들지 않고 로그에만 남깁니다.
  - `pedrv.exe`는 스크립트에 인코딩되어 있습니다(`[EncodedFolders]`의 `pedrv` 폴더, x64 빌드). `ExtractFile,%ScriptFile%,pedrv,pedrv.exe,...`로 풀며, `%SourceArch%`가 x64가 아니면 외부 pedrv.exe를 고르라는 메시지와 함께 빌드를 멈춥니다. pedrv를 고치면 `build.cmd`로 다시 빌드하고, PEBakery에서 스크립트의 `pedrv.exe`를 다시 인코딩해야 합니다. 7-Zip은 `%Tools%\%SourceArch%`의 것을 씁니다.
  - **버전 규칙**: 동작이 바뀌면 `src\pedrv.cpp`의 `PEDRV_VERSION`을 올립니다. PE 화면의 첫 줄(`pedrv 1.2 (built ...)`)로 어느 빌드인지 구분하기 위해서입니다. `pedrv.exe`를 다시 인코딩할 때는 스크립트 `[Main]`의 `Version=`을 `<pedrv 버전>.<스크립트 수정 번호>.0` 형식(예: pedrv 1.2를 넣은 첫 스크립트는 `1.2.0.0`)으로 함께 바꿉니다. 그러면 스크립트 버전만 보고도 들어 있는 pedrv를 알 수 있습니다. 인코딩하지 않은 동안에는 스크립트 버전을 바꾸지 않습니다.
  - PEBakery 명령은 쉼표로 인수를 나누므로 콤보 상자 값에 쉼표를 쓰지 않습니다.

```
pedrv D:\Drivers
pedrv /image:C:\ D:\packs\chipset.7z D:\packs\net.zip D:\drivers.wim
pedrv /list D:\Drivers
```

종료 코드: `0` 성공, `1` 일부 드라이버 실패(또는 `/pe`에서 장치가 여전히 문제 상태), `2` 사용법/환경 오류, `3` 중단됨

Ctrl+C를 누르면 진행 중인 드라이버 하나를 마친 뒤 DISM 세션 닫기, WIM 마운트 해제, 작업 폴더 삭제를 하고 종료 코드 3으로 끝냅니다. 한 번 더 누르면 기다리지 않고 바로 종료합니다. `drvload`·`pnputil`·`dism.exe`가 30분 안에 끝나지 않으면 강제로 종료하고 그 드라이버를 실패로 처리합니다.

## 동작 방식

1. SetupAPI로 현재 장치의 하드웨어 ID / 호환 ID를 수집합니다.
2. 소스의 INF를 읽어 대상 Windows에 해당하는 모델 섹션의 ID와 대조합니다.
   - Windows와 같은 방식으로 제조사 항목마다 **가장 가까운 장식 하나**만 씁니다 (OS 버전이 가장 높은 것 → 아키텍처를 지정한 것 → 제품 유형을 지정한 것). 아키텍처, OS 버전, 빌드 번호, 제품 유형(클라이언트 `.1` / 서버 `.3`)이 맞아야 합니다. 대상의 제품 유형은 오프라인 Windows의 SYSTEM 하이브 사본에서 읽고, WinPE와 `/export`는 클라이언트로 봅니다.
   - INF는 한 번만 파싱하고, `/pe`의 반복 패스에서는 장치 ID와의 대조만 다시 합니다.
   - 260자를 넘는 깊은 경로도 처리합니다 (SetupAPI가 긴 경로를 열지 못하므로 그런 INF는 작업 폴더에 사본을 만들어 읽습니다). 정션·심볼릭 링크 폴더는 따라가지 않습니다.
   - 폴더: 그대로 검색
   - WIM: 읽기 전용 마운트 (실패하면 7-Zip으로 추출)
   - zip / 7z: 먼저 `*.inf`만 추출해 매칭한 뒤, 선택된 드라이버 폴더만 추가 추출 (RAM 디스크 공간 절약)
3. DISM API(`dismapi.dll`)로 오프라인 이미지에 추가합니다. API를 쓸 수 없으면 `dism.exe /Add-Driver`를 사용하고, 실패하면 dism.exe가 출력한 오류 줄을 `FAILED` 아래에 보여 줍니다.

## WinPE에 설치 (`/pe`)

```
pedrv /pe D:\Drivers
pedrv /pe /list D:\Drivers
```

- 매칭된 INF를 `drvload.exe`로 실행 중인 WinPE에 올립니다 (`drvload`가 없으면 `pnputil /add-driver /install`).
- 컨트롤러·버스 드라이버가 올라가면 그 뒤의 장치가 새로 보이므로, 장치를 다시 검색해 더 맞는 드라이버가 없을 때까지 최대 5회 반복합니다.
- 먼저 `/pe`로 WinPE에 드라이버를 올린 뒤 `/pe` 없이 한 번 더 실행하면, 새로 보인 장치의 드라이버까지 오프라인 Windows에 설치할 수 있습니다.
- 디스플레이 드라이버도 함께 올립니다. 낮은 해상도로 고정된 PE에서 외부 그래픽 드라이버를 불러오는 것이 이 모드의 주 용도입니다.
- 드라이버를 올린 뒤 장치 상태를 다시 확인해, 여전히 문제가 있는 장치는 `WARNING: device not working (code N)`으로 표시하고 종료 코드 1을 반환합니다.
- PE의 레지스트리에 `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\ForceBddFallbackOnly=1`이 들어 있으면 기본 디스플레이 드라이버 외에는 거부되어 장치가 코드 31이 됩니다. 원본 Windows 이미지에 따라 이 값이 들어 있는 경우가 있습니다. 실행 중에 값을 바꾸는 것은 효과가 없고(시험으로 확인), PE 빌드 단계에서 0으로 바꿔야 합니다. PhoenixPE라면 스크립트에 다음 줄을 추가합니다.

  ```
  RegWrite,HKLM,REG_DWORD,"Tmp_System\ControlSet001\Control\GraphicsDrivers","ForceBddFallbackOnly",0
  ```

  이 값이 1인 PE에서 `/pe`를 실행하면 경고를 출력하고 디스플레이 드라이버는 건너뜁니다.
- Windows 11 24H2 이후로 만든 PE에서 DWM(`dwm.exe`)이 들어 있으면 `System32\DDisplay.dll`이 필요합니다. 제조사 디스플레이 드라이버가 시작되면 DWM이 `DispBroker.dll`을 통해 이 DLL을 불러오는데, 없으면 DWM이 반복해서 죽고 Windows가 기본 디스플레이로 되돌려 장치가 코드 43이 됩니다. PhoenixPE는 기본으로 이 파일을 넣지 않으므로 PE 빌드에 추가해야 합니다 (`phoenixpe\DisplayDriverSupport.script`가 `ForceBddFallbackOnly`와 함께 처리합니다).

  ```
  RequireFileEx,AppendList,\Windows\System32\DDisplay.dll
  RequireFileEx,ExtractList
  ```

  이 파일이 없는 PE에서 `/fromwin`을 쓰면 같은 빌드의 오프라인 Windows에서 `DDisplay.dll`을 PE로 복사한 뒤 진행합니다. 복사할 수 없으면 경고를 출력하고 디스플레이 드라이버는 건너뜁니다. 원인과 조사 과정은 [docs/pe-24h2-display-driver.md](docs/pe-24h2-display-driver.md)에 정리했습니다.
- 드라이버 로드가 실패하면 `FAILED` 줄 아래에 PE의 `setupapi.dev.log`에 기록된 오류(`!!!` 줄)를 함께 보여 줍니다. 드라이버 패키지는 PE 시스템 드라이브(RAM 디스크)의 드라이버 저장소로 통째로 복사되므로, 폴더 크기가 남은 공간의 절반을 넘으면 크기와 남은 공간도 표시합니다.
- `/pe`의 작업 폴더 기본값은 **PE를 부팅한 드라이브**(boot.wim이 있는 드라이브)의 `\Temp\PeDrv`입니다. WinPE가 부팅할 때 기록하는 `HKLM\SYSTEM\CurrentControlSet\Control\PEBootRamdiskSourceDrive` 값으로 찾습니다 (PhoenixPE로 만든 PE에서 동작 확인, 2026-11-15). 압축 해제 사본과 drvload의 임시 사본(pedrv가 drvload의 `TEMP`를 작업 폴더로 바꿈)이 모두 여기에 놓이므로 RAM 디스크를 쓰지 않습니다. 실행할 때 `Scratch:` 줄로 실제 위치를 보여 줍니다.
  - 그 값이 없거나, 드라이브가 읽기 전용(CD, ISO, Ventoy 등)이라 폴더를 만들 수 없으면 `%TEMP%\PeDrv`(RAM 디스크)를 씁니다.
  - 새로 만든 `Temp`·`PeDrv` 폴더는 끝날 때 비어 있으면 지웁니다.
  - 이렇게 바꾼 이유: AIM "2GB Dynamic"·NTFS 압축 램디스크(B:)에 작업 폴더와 `%TEMP%`가 함께 있을 때, NVIDIA 축소 패키지(765 MB)가 zip 압축 해제 사본과 임시 사본으로 두 벌 놓였습니다. 남은 공간이 580 MB로 표시되는데도 `0x70`(공간 부족)으로 실패했습니다 (2026-11-14). 램디스크 쪽 정확한 원인은 확인하지 못했습니다.
  - 다른 위치를 쓰려면 `/scratch:`로 지정합니다 (예: `pedrv /pe /scratch:D:\Temp D:\Drivers`). `/fromwin`은 기본 작업 폴더가 이미 오프라인 Windows 드라이브에 있습니다. 어느 경우든 PE 시스템 드라이브에는 패키지 크기만큼의 공간이 필요하며, 모자라면 축소 사본으로 올립니다 (아래 "드라이버 축소" 참고).
- `/forceunsigned`와 `/log`는 이 모드에서 적용되지 않습니다.
- WinPE가 아닌 일반 Windows에서는 `/pe /list`(미리 보기)만 동작하고 설치는 거부합니다.

## 오프라인 Windows의 드라이버를 WinPE에 설치 (`/fromwin`)

```
pedrv /fromwin
pedrv /fromwin:C:\
pedrv /fromwin /list
```

- 디스크에 설치된 Windows의 드라이버 저장소(`Windows\System32\DriverStore\FileRepository`)를 소스로 삼아, 현재 장치에 맞는 드라이버를 실행 중인 WinPE에 올립니다. `/pe`를 포함하므로 따로 붙일 필요가 없고, 소스를 지정하지 않아도 됩니다.
- 경로를 생략하면 오프라인 Windows를 자동으로 찾습니다. 여러 개면 목록을 보여 주고 멈춥니다.
- 기본으로 그 Windows에 나중에 설치된 드라이버(제조사 드라이버)만 대상으로 합니다. Windows에 기본 포함된 드라이버까지 쓰려면 `/inbox`를 붙이세요.
  - 판별 방법: 제조사 드라이버는 `Windows\INF\oemN.inf`에 INF 사본으로 등록됩니다. 그래서 `Windows\INF`에 같은 이름의 INF가 있더라도, 내용이 어떤 `oemN.inf`와 같으면 제조사 드라이버로 봅니다. AMD `amdgpio2.inf`나 Realtek `rt640x64.inf`처럼 기본 드라이버와 이름이 같은 제조사 패키지도 빠지지 않습니다.
- 같은 장치의 드라이버가 여러 버전 쌓여 있어도 장치마다 가장 적합한 것 하나만 올립니다.
- 다른 소스를 함께 지정할 수 있습니다 (예: `pedrv /fromwin D:\Drivers`).
- BitLocker로 잠긴 드라이브는 읽을 수 없어 찾지 못합니다. 먼저 잠금을 해제하세요.

## 드라이버 축소 (`/slim`, `/export`)

최근 그래픽 드라이버는 패키지 하나가 수 GB입니다(NVIDIA 약 2.7 GB). WinPE에 올리면 `%TEMP%`와 시스템 드라이브(둘 다 RAM 디스크)에 통째로 복사되므로 메모리가 적은 PC에서는 들어가지 않습니다. 축소 사본은 패키지 구조는 그대로 두고, PE에서 화면을 띄우는 데 필요 없는 파일만 **0바이트 빈 파일**로 바꾼 것입니다. INF와 카탈로그는 고치지 않습니다. 빈 파일은 카탈로그 해시와 맞지 않지만 WinPE는 이를 무시합니다.

비우는 파일:

| 규칙 | 대상 |
|---|---|
| WSL/Linux용 | 모든 패키지. INF가 `lxss` 폴더로 복사하는 파일과 `*.so` |
| 32비트 실행 파일 | 모든 패키지. 파일 헤더로 판별. WoW64가 있는 PE나 x86 대상에서는 남김 |
| 다른 장치용 INF | 모든 패키지. 선택된 INF와 그 INF가 `CopyINF`로 지정한 INF(NVIDIA `nvppc.inf`, AMD `.\amdxe\amdxe.inf` 등)만 남김. NVIDIA 패키지에는 INF가 45개(약 52 MB) 있음 |
| CUDA, OptiX, NVVM, OpenCL, 비디오 코덱, DirectML, NGX/DLSS(`_nvngx.dll` 포함), Vulkan SC, NvCamera, 텔레메트리 | NVIDIA 디스플레이 패키지만 |
| FidelityFX 라이브러리(`amdxcffx*`), 대체 Vulkan 드라이버(`amdvlk_nexus.cab`), AMF 비디오 런타임(`amfrtdrv*`), 소프트웨어 설치 파일(`AMDInstallManager.msi`) | AMD 디스플레이 패키지만 |
| OpenGL·Vulkan 드라이버 (NVIDIA `nvoglv*`, AMD `amdvlk*`·`atio6axx`·`atioglxx`·`atig6pxx`·`atiglpxx`), `vulkaninfo*` | `/slim:max`일 때만. Vulkan 로더(`vulkan-1.dll`)는 남김 |

선택된 INF, `.cat`, `.sys`는 항상 남깁니다.

**패키지 범위**: 축소 사본과 공간 계산은 INF가 있는 폴더를 대상으로 하되, 자체 INF가 들어 있는 하위 폴더는 다른 패키지로 보고 뺍니다 (Intel 그래픽 패키지의 `CtaChildDriver`, `DisplayAudio` 등). 선택된 INF가 `CopyINF`, `SourceDisksNames`, `SourceDisksFiles`로 참조하는 하위 폴더는 남깁니다. 예전에는 INF 폴더 아래를 전부 복사해서, 소스 최상위에 INF가 있으면 그 아래 드라이버를 모두 복사했습니다. `/pe`에서 나중 패스에 같은 폴더의 다른 INF가 선택되면, 앞서 만든 사본에서는 그 INF가 비어 있으므로 사본을 새로 만듭니다.

측정값 (2026-11-16, pedrv 1.2):

| 패키지 | 원본 | 축소 | `/slim:max` |
|---|---|---|---|
| NVIDIA 610.88 (`/all`로 INF 45개 모두 선택, INF 정리 효과 없음) | 2,746 MB | 780 MB | 731 MB |
| AMD 32.0.21045.16010 (RX 6600) | 1,171 MB | 491 MB (`/keep32`면 813 MB) | 329 MB |

예전 측정: NVIDIA 32.0.16.1742는 2,718 MB가 약 750 MB가 되었습니다.

AMD 규칙은 일반 Windows에서 프로세스들이 실제로 불러오는 DLL을 보고 정했습니다 (2026-11-16). D3D11 드라이버(`amdxx64.dll`)는 프로세스마다 `amdenc64.dll`·`amdihk64.dll`을 함께 불러오고 `amdn1b64.dll`을 참조하므로, 이 파일들은 이름과 달리 비우면 안 됩니다. D3D9/11/12·OpenGL·Vulkan 드라이버와 `.dat` 펌웨어도 남깁니다. 축소한 AMD 패키지(RX 6600)를 부팅한 PE에서 올려 정상 동작하는 것을 확인했습니다 (2026-11-16). 규칙별 파일 목록과, 더 비울 수 있는 후보 파일은 [docs/slim-display-drivers.md](docs/slim-display-drivers.md)에 정리했습니다.

- **자동 축소**: `/pe`·`/fromwin`에서 패키지가 작업 폴더나 PE 시스템 드라이브의 남은 공간에 들어가지 않을 때만 축소 사본으로 올립니다. 공간이 충분하면 원본 그대로 올립니다. `/slim`을 붙이면 항상 축소합니다.
- 작업 폴더가 원본과 같은 NTFS 볼륨이면 남기는 파일은 복사하지 않고 하드링크로 겁니다. `/fromwin`의 기본 작업 폴더는 오프라인 Windows 드라이브의 `$PeDrv`이므로(쓸 수 없으면 `%TEMP%`), RAM 디스크는 PE 드라이버 저장소에 들어가는 축소된 크기만큼만 씁니다.
- **추출**: `/export:<폴더>`는 아무것도 설치하지 않고, 이 PC에 맞는 드라이버 패키지의 축소 사본을 폴더에 저장합니다. WinPE가 아닌 일반 Windows에서도 동작합니다. 나중에 PE에서 `drvload`나 `pedrv /pe <폴더>`로 올릴 수 있습니다.

  ```
  pedrv /fromwin:C:\ /export:D:\PEDrivers
  pedrv /export:D:\PEDrivers D:\Drivers\nvidia
  ```

  대상 PE를 알 수 없으므로 32비트 파일은 기본으로 비웁니다. WoW64가 들어 있는 PE에서 쓸 거라면 `/keep32`를 붙이세요.
- 축소 사본은 **WinPE 전용**입니다. 오프라인 Windows에 설치할 때는 축소하지 않으며, 추출한 축소 사본을 실제 Windows에 설치해서도 안 됩니다.
- 축소 사본은 **부팅한 WinPE에서 올릴 때만** 동작합니다 (`pedrv /pe`, `drvload`). **PE 이미지(boot.wim)에 미리 통합하면 동작하지 않습니다.** DISM `/Add-Driver /Image:`나 PhoenixPE `DriverIntegration.script`로 통합하는 경우가 여기에 해당합니다.
  - 통합 자체는 성공으로 끝납니다. 하지만 이미지를 만드는 일반 Windows가 서명을 확인하면서 비운 파일의 해시 불일치를 찾아냅니다. 그 결과 패키지가 서명 없는 드라이버로 등록됩니다. 이미지의 `Windows\INF\setupapi.offline.log`에 다음과 같이 남습니다.

    ```
    !    sto: Unable to verify driver package signature. Error = 0x000003EE
         sig: Signer Score  = 0x0D000004 (Unclassified)
    ```

  - 이 등급은 통합할 때 정해져 이미지에 그대로 남습니다. 그래서 PE로 부팅해도 PnP가 그 드라이버를 장치에 설치하지 않습니다. 디스플레이라면 기본 디스플레이 드라이버가 그대로 쓰입니다.
  - 부팅 후에 올리면 실행 중인 WinPE가 서명을 확인하는데, WinPE는 해시 불일치를 무시하므로 동작합니다.
  - PE 이미지에 미리 넣어야 한다면 **전체 패키지**를 통합하세요. 크기를 줄이려면 축소 사본을 boot.wim 밖(부팅 매체)에 두고, PE가 시작할 때 `pedrv /pe <매체>\Drivers`를 실행하세요.
- 비운 파일 일부는 PE의 `System32`에 0바이트로 놓입니다(`OpenCL.dll`, `nvcuda.dll` 등). PE에서 CUDA·OpenCL을 쓰는 프로그램은 동작하지 않습니다.

## 요구 사항 / 제한

- zip·7z 소스에는 7-Zip이 필요합니다. `7z.exe`+`7z.dll`(또는 `7za.exe`)을 exe와 같은 폴더에 두세요. 7-Zip이 없으면 zip은 `tar.exe`가 있을 때만 처리됩니다.
- WinPE에서 드라이버가 없어 열거되지 않는 장치(예: 드라이버 없는 컨트롤러 뒤의 장치)는 매칭되지 않습니다. 이런 경우 `/all`을 쓰거나, 먼저 `drvload`로 해당 컨트롤러 드라이버를 PE에 올린 뒤 실행하세요.
- 실행 중인 Windows 자체에는 설치하지 않습니다(오프라인 전용).

## 빌드

Visual Studio(C++ 워크로드)가 설치된 PC에서:

```
build.cmd          (x64, 결과: bin\x64\pedrv.exe)
build.cmd x86
build.cmd arm64
```

x64 빌드 시 `C:\Program Files\7-Zip`이 있으면 `7z.exe`, `7z.dll`을 함께 복사합니다.

MSVC 툴셋은 자동으로 고릅니다. Visual Studio의 기본 툴셋이 그 아키텍처의 컴파일러(`bin\Hostx64\<arch>\cl.exe`)와 정적 런타임(`lib\<arch>\libcpmt.lib`)을 갖고 있으면 그것을 쓰고, 없으면 설치된 툴셋 중 가진 것의 가장 새 버전을 씁니다. 빌드할 때 `Toolset:` 줄로 실제 버전을 보여 줍니다. 버전을 직접 지정하려면 `build.cmd arm64 14.44`처럼 두 번째 인수로 줍니다. 그 툴셋으로 빌드할 수 없으면 링크 오류 대신 설치된 툴셋 목록을 보여 주고 멈춥니다.

이렇게 바꾼 이유 (2026-11-16): Visual Studio 2026의 기본 툴셋 14.52에는 ARM64 빌드 도구가 없고, ARM64 도구는 v143 툴셋(14.44)에만 설치되어 있습니다. Visual Studio Installer는 이 상태를 최신으로 표시합니다. 기본 툴셋만 쓰면 arm64 빌드가 `LNK1104: libcpmt.lib`로 실패했습니다. 14.44에는 반대로 x64·x86용 정적 런타임이 없어서 툴셋 하나로 고정할 수 없습니다.

`build.cmd`는 CRLF 줄바꿈이어야 합니다. LF만 쓰면 cmd가 `call :레이블`을 찾지 못하는 경우가 있습니다.
