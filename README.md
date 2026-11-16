# pedrv

**WinPE에서 이 PC의 하드웨어에 맞는 드라이버만 골라 설치하는 도구**

드라이버 폴더나 압축 파일을 주면, 현재 장치의 하드웨어 ID와 INF를 대조해 필요한 드라이버만 골라 설치합니다. 설치 대상은 오프라인 Windows(예: 복구·배포 중인 `C:\`)이거나 실행 중인 WinPE입니다.

- 드라이버 소스: INF 폴더, `.zip`, `.7z`, `.wim`, 디스크에 설치된 Windows의 드라이버 저장소
- 장치마다 가장 적합한 드라이버 하나만 선택 (ID 순위 → 날짜 → 버전)
- 실행 파일 하나로 동작 (정적 CRT, .NET·PowerShell 불필요)
- 수 GB짜리 그래픽 드라이버를 WinPE의 RAM 디스크에 들어가도록 축소
- 콘솔 메시지는 영어 (WinPE에는 보통 한글 글꼴이 없음)

## 빠른 시작

```bat
:: 오프라인 Windows에 이 PC용 드라이버 설치 (대상 Windows는 자동 탐지)
pedrv D:\Drivers

:: 설치하지 않고 어떤 드라이버가 선택되는지만 확인
pedrv /list D:\Drivers

:: 실행 중인 WinPE에 드라이버 올리기 (예: 그래픽 드라이버로 해상도 올리기)
pedrv /pe D:\Drivers

:: 디스크에 설치된 Windows의 드라이버를 그대로 WinPE에 올리기
pedrv /fromwin
```

소스를 생략하면 `pedrv.exe` 옆의 `Drivers` 폴더를 사용합니다.

## 설치 대상별 사용법

| 하려는 일 | 명령 |
|---|---|
| 오프라인 Windows에 설치 | `pedrv [/image:C:\] <소스...>` |
| 실행 중인 WinPE에 설치 | `pedrv /pe <소스...>` |
| 디스크의 Windows에 있던 드라이버를 WinPE에 설치 | `pedrv /fromwin[:C:\]` |
| PE용 축소 드라이버를 폴더로 저장 | `pedrv /export:<폴더> <소스...>` |

### 오프라인 Windows (`/image`)

DISM API로 오프라인 이미지에 드라이버를 추가합니다. `/image`를 생략하면 고정 디스크에서 Windows를 찾고, 여러 개면 목록을 보여 주고 멈춥니다.

### 실행 중인 WinPE (`/pe`)

`drvload`로 드라이버를 올립니다. 컨트롤러 드라이버가 올라가면 그 뒤의 장치가 새로 보이므로, 더 올릴 드라이버가 없을 때까지 최대 5번 반복합니다. 올린 뒤에도 장치에 문제가 남아 있으면 `WARNING: device not working (code N)`으로 알려 줍니다.

### 디스크의 Windows → WinPE (`/fromwin`)

디스크에 설치된 Windows의 드라이버 저장소를 소스로 씁니다. 기본으로 제조사 드라이버만 대상으로 하며, Windows 기본 드라이버까지 쓰려면 `/inbox`를 붙입니다. 그 PC에서 쓰던 드라이버를 그대로 PE에 올릴 수 있어, 드라이버 파일을 따로 준비하지 않아도 됩니다.

## 옵션

| 옵션 | 설명 |
|---|---|
| `/image:<경로>` | 오프라인 Windows 루트. 생략하면 자동 탐지 |
| `/pe` | 실행 중인 WinPE에 설치 |
| `/fromwin[:<경로>]` | 디스크의 Windows 드라이버 저장소를 소스로 WinPE에 설치. `/inbox`로 기본 드라이버 포함 |
| `/list` | 선택될 드라이버만 표시 |
| `/all` | 하드웨어 매칭 없이 소스의 모든 드라이버 설치 |
| `/allmatches` | 장치마다 하나가 아니라 매칭되는 드라이버를 전부 설치 |
| `/hwonly` | 하드웨어 ID만 매칭 (호환 ID 무시) |
| `/exclude:<클래스,...>` | 장치 클래스 제외 (예: `/exclude:Display,Media`) |
| `/slim` | WinPE에 올릴 때 항상 축소 사본 사용 |
| `/slim:max` | `/slim`에 더해 NVIDIA·AMD의 OpenGL·Vulkan 드라이버도 비움. `/export`에도 적용 |
| `/export:<폴더>` | 설치하지 않고 축소 사본을 폴더에 저장. `/keep32`로 32비트 파일 유지 |
| `/forceunsigned` | 서명 없는 드라이버 허용 (오프라인 설치) |
| `/scratch:<폴더>` | 작업 폴더. 기본은 오프라인 설치면 대상 Windows 드라이브, `/pe`면 PE를 부팅한 드라이브(쓸 수 없으면 RAM 디스크의 `%TEMP%`) |
| `/7z:<경로>` | 7-Zip 위치 |
| `/log:<파일>` | DISM 로그 파일 |
| `/version` | 버전 표시 |

WIM의 이미지 번호는 `drivers.wim:2`처럼 지정합니다. 소스 폴더 바로 아래의 압축 파일도 자동으로 소스에 포함됩니다. 드라이브 문자 대신 `?:`를 쓰면(`?:\PEDrivers`) C:부터 Z:까지 차례로 찾아 그 경로가 있는 첫 드라이브를 씁니다. PC마다 드라이브 문자가 바뀌는 부팅 매체에 유용합니다.

종료 코드: `0` 성공 · `1` 일부 실패 또는 장치 문제 남음 · `2` 사용법/환경 오류 · `3` 중단됨 (Ctrl+C)

## 대용량 그래픽 드라이버 축소

NVIDIA 드라이버 패키지 하나가 약 2.7 GB입니다. WinPE에 올리면 RAM 디스크에 통째로 복사되므로 메모리가 적은 PC에서는 들어가지 않습니다.

pedrv는 PE에서 화면을 띄우는 데 필요 없는 파일만 0바이트로 비운 **축소 사본**을 만들어 올립니다. 비우는 파일은 WSL/Linux용 파일, 32비트 파일, 다른 장치용 INF, NVIDIA 패키지의 CUDA·OptiX·DLSS·비디오 코덱 등, AMD 패키지의 FidelityFX·AMF 런타임·설치 파일 등입니다. 이렇게 하면 NVIDIA 패키지(약 2.7 GB)가 약 730~780 MB, AMD 패키지(약 1.2 GB)가 약 490 MB가 됩니다. 선택된 INF, `.cat`, `.sys`는 그대로 둡니다. 자체 INF가 있는 하위 폴더(Intel의 자식 드라이버 등)는 별도 패키지로 보고 사본에 넣지 않습니다.

- 기본으로는 RAM 디스크에 들어가지 않는 패키지만 축소합니다. `/slim`을 붙이면 항상 축소합니다.
- `/slim:max`는 OpenGL·Vulkan 드라이버까지 비웁니다 (AMD 약 330 MB). 그 PE에서는 OpenGL·Vulkan 프로그램이 동작하지 않습니다.
- `/export:<폴더>`로 축소 사본을 미리 만들어 둘 수 있습니다.
- 규칙별 파일 목록과 측정값은 [docs/slim-display-drivers.md](docs/slim-display-drivers.md)에 있습니다.

> [!WARNING]
> 축소 사본은 **부팅한 WinPE에서 올릴 때만** 동작합니다 (`pedrv /pe`, `drvload`).
> - **PE 이미지(boot.wim)에 미리 통합하면 동작하지 않습니다.** DISM `/Add-Driver /Image:`나 PhoenixPE의 드라이버 통합 기능을 쓰는 경우입니다. 이미지를 만드는 Windows가 서명 불일치를 찾아 서명 없는 드라이버로 등록하기 때문에, 부팅해도 장치에 설치되지 않습니다.
> - PE 이미지에 미리 넣으려면 전체 패키지를 쓰세요. 아니면 축소 사본을 부팅 매체에 두고, PE가 시작할 때 `pedrv /pe`를 실행하세요.
> - 일반 Windows에 설치해서도 안 됩니다.

## PhoenixPE 스크립트

[`phoenixpe/PedrvDriverLoader.script`](phoenixpe/PedrvDriverLoader.script)는 PhoenixPE로 만드는 PE에 pedrv를 넣어, 부팅한 PE에서 드라이버를 올릴 수 있게 합니다.

- **드라이버 위치**: 둘 중 하나를 고릅니다.
  - 부팅 매체나 디스크의 폴더 (기본값 `\PEDrivers`: 모든 드라이브에서 찾음)
  - PE 이미지에 파일로 포함 (`X:\Program Files\pedrv`로 복사)
- **부팅 시 자동 실행** (선택): 바탕 화면 전/후를 고르고, 창 표시 방식을 고릅니다. 최소화(기본, 작업 표시줄 단추만), 숨김(출력은 `X:\Windows\Temp\pedrv.log`), 보통 창, 보통 창 + 키 입력까지 대기 중 하나입니다.
- **제외할 장치 클래스**: Display, Media, Net, Bluetooth, USB, SCSIAdapter 등 12개 체크 상자와 "기타" 입력란으로 `/exclude:`를 만듭니다.
- **바로가기**: 바탕 화면, 시작 메뉴. 실행 결과를 볼 수 있도록 콘솔 창이 열린 채로 남습니다.
- **`/fromwin` 바로가기**: 디스크에 설치된 Windows의 드라이버를 PE에 올리는 바로가기를 따로 만듭니다. `/inbox`(Windows 기본 드라이버 포함)도 고를 수 있습니다.
- **추가 옵션**: `/slim`, `/scratch:` 등 pedrv 옵션을 덧붙일 수 있습니다.
- **필요한 파일**: `pedrv.exe`(x64)는 스크립트 안에 인코딩되어 있어 따로 준비할 필요가 없습니다. 새 빌드나 x86·arm64 PE용 빌드를 쓰려면 "외부 pedrv.exe"를 선택하고 파일을 지정하세요. 7-Zip은 PhoenixPE에 들어 있는 것을 넣습니다.

`Projects\PhoenixPE\Drivers`처럼 원하는 위치에 복사해 쓰면 됩니다. 이 방식은 PE 실행 중에 드라이버를 올리므로 축소 사본(`/export`)도 동작합니다. 그래픽 드라이버를 쓰려면 `DisplayDriverSupport.script`도 함께 선택하세요.

## WinPE에서 그래픽 드라이버가 동작하지 않을 때

PE를 만드는 방식에 따라 제조사 그래픽 드라이버가 막혀 있을 수 있습니다. pedrv는 아래 두 경우를 감지하면 경고를 출력하고 디스플레이 드라이버를 건너뜁니다.

| 증상 | 원인 | 해결 |
|---|---|---|
| 장치 코드 31 | PE 레지스트리의 `GraphicsDrivers\ForceBddFallbackOnly=1` | PE 빌드 단계에서 0으로 설정 (실행 중에 바꾸면 효과 없음) |
| 장치 코드 43, DWM 반복 재시작 | Windows 11 24H2 이후 PE에 `System32\DDisplay.dll`이 없음 | PE 빌드에 `DDisplay.dll` 추가. `/fromwin`이면 같은 빌드의 Windows에서 자동 복사 |

PhoenixPE를 쓴다면 [`phoenixpe/DisplayDriverSupport.script`](phoenixpe/DisplayDriverSupport.script)가 두 가지를 모두 처리합니다. 조사 과정은 [docs/pe-24h2-display-driver.md](docs/pe-24h2-display-driver.md)에 있습니다.

## 요구 사항 및 제한

- Windows 10 / 11 기반 WinPE (x64, x86, ARM64)
- `.zip`·`.7z` 소스에는 7-Zip이 필요합니다. `7z.exe`와 `7z.dll`(또는 `7za.exe`)을 `pedrv.exe`와 같은 폴더에 두세요. 7-Zip이 없으면 zip은 `tar.exe`가 있을 때만 처리합니다.
- 드라이버가 없는 컨트롤러 뒤의 장치처럼 PE에서 보이지 않는 장치는 매칭할 수 없습니다. 이럴 때는 먼저 `pedrv /pe`로 컨트롤러 드라이버를 올린 뒤 다시 실행하세요.
- 실행 중인 Windows 자체에는 설치하지 않습니다 (`/pe`는 WinPE에서만 설치하고, 일반 Windows에서는 `/list`만 동작).
- BitLocker로 잠긴 드라이브는 읽을 수 없습니다.

## 빌드

Visual Studio(C++ 워크로드)가 설치된 PC에서 실행합니다.

```bat
build.cmd          :: x64 → bin\x64\pedrv.exe
build.cmd x86
build.cmd arm64
```

x64 빌드 때 `C:\Program Files\7-Zip`이 있으면 `7z.exe`와 `7z.dll`도 함께 복사합니다.

## 더 자세한 설명

옵션별 세부 동작, INF 선택 규칙, 작업 폴더와 RAM 디스크 사용량, 설계 결정의 근거는 [CLAUDE.md](CLAUDE.md)에 정리되어 있습니다. 조사 기록은 [docs/](docs/)에 있습니다.

## 라이선스

[MIT License](LICENSE)

7-Zip은 이 저장소에 포함되지 않습니다. 7-Zip을 함께 배포할 때는 7-Zip의 라이선스(GNU LGPL 등)를 따라야 합니다.
