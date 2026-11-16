# Windows 11 24H2 이후로 빌드한 PhoenixPE에서 디스플레이 드라이버가 코드 43이 되는 문제

2026-11-14에 확인한 내용입니다.

## 요약

- **증상**: 26H2(커널 10.0.26100)로 빌드한 PhoenixPE에서 NVIDIA 디스플레이 드라이버를 `drvload`로 올리면 설치는 성공하지만 해상도가 바뀌지 않고, 장치 관리자에 "이 장치에서 문제가 보고되었기 때문에 장치를 중지했습니다. (코드 43)"이 표시됩니다.
- **원인**: PE에 `Windows\System32\DDisplay.dll`이 없습니다.
- **해결**: PE 빌드에 `DDisplay.dll`을 추가합니다.
- 23H2(10.0.22621)로 빌드한 PE에서는 같은 드라이버가 문제없이 동작합니다.

## 해결 방법

### PE 빌드에서 (권장)

PhoenixPE 스크립트에 다음 두 줄을 넣어 원본 이미지에서 파일을 가져옵니다. `phoenixpe\DisplayDriverSupport.script`에 들어 있습니다.

```
RequireFileEx,AppendList,\Windows\System32\DDisplay.dll
RequireFileEx,ExtractList
```

같은 스크립트가 `GraphicsDrivers\ForceBddFallbackOnly`도 0으로 설정합니다. 이 값이 1이면 기본 디스플레이 드라이버 외에는 거부되어 코드 31이 됩니다. 두 조건은 별개이며 둘 다 필요합니다.

### 실행 중인 PE에서

드라이버를 올리기 전에 같은 빌드의 Windows에서 파일을 복사합니다.

```
copy Y:\Windows\System32\DDisplay.dll X:\Windows\System32\
```

`pedrv /fromwin`은 PE에 이 파일이 없으면 오프라인 Windows(아키텍처와 빌드 번호가 같을 때)에서 자동으로 복사합니다. 복사할 수 없으면 경고를 출력하고 디스플레이 드라이버를 건너뜁니다.

확인한 조합은 PE 10.0.26100.9457에 Windows 10.0.26100.9818의 `DDisplay.dll`을 복사한 경우입니다.

## 원인 설명

코드 43은 결과이고, 실제로 일어나는 일은 다음과 같습니다.

1. `nvlddmkm.sys`가 로드되고 어댑터가 정상적으로 시작됩니다. GPU 메모리 관리가 동작하고 첫 화면까지 출력됩니다.
2. 약 60ms 뒤 `dwm.exe`가 죽습니다. 예외 코드는 `0xC0000409`, 매개변수는 `0xA`(잘못된 간접 호출), 주소는 0입니다.
3. `dwm.exe`가 약 1초 간격으로 다시 시작되고 매번 같은 방식으로 죽습니다(7~8회).
4. Windows가 기본 디스플레이로 되돌리고(`BddFallback`) 어댑터를 정지시킨 뒤 드라이버를 언로드합니다. 이때 장치가 코드 43(`CM_PROB_FAILED_POST_START`)이 됩니다.

`dwm.exe`가 죽는 호출 경로입니다.

```
dwmcore!CDDisplayRenderTarget::TryEnsureSwapChain
dwmcore!CDDisplayManager::CreateDDisplaySwapchainForSource
dwmcore!CDDisplayManager::EnsureDevice
DispBroker!Windows::Devices::Display::Core::DisplayManager::CreateDisplayDevice
```

`DispBroker.dll`의 마지막 함수는 `LoadLibrary("DDisplay.dll")`을 호출하고, 그 DLL의 100번 함수 주소를 얻어 검사 없이 호출합니다. 파일이 없으면 주소가 0이 되고, 0번지 호출을 Windows가 감지해 프로세스를 즉시 종료합니다.

24H2 이후의 DWM은 제조사 그래픽 드라이버가 시작되면 이 경로를 사용합니다. 기본 디스플레이 드라이버만 있을 때는 사용하지 않으므로, 드라이버를 올리기 전에는 문제가 드러나지 않습니다. 23H2의 DWM은 이 경로를 쓰지 않으며, 23H2 PE에는 `DispBroker.dll`과 `DDisplay.dll`이 둘 다 없습니다.

PhoenixPE(2026.10.25)는 `DispBroker.dll`은 PE에 넣지만 `DDisplay.dll`은 넣지 않습니다. 이름으로 직접 불러오는 DLL이라 import 표를 따라가는 의존성 검사에는 나타나지 않습니다.

`DDisplay.dll`이 필요로 하는 DLL(`dxgi.dll`, `d3d11.dll`, `DispBroker.dll` 등)은 PE에 이미 있습니다.

## 원인이 아니었던 것

조사하면서 확인하고 배제한 것들입니다. 같은 증상을 다시 볼 때 이 순서로 헤매지 않도록 남깁니다.

| 의심한 것 | 결과 |
|---|---|
| RAM 디스크 공간 부족 | 별개의 문제입니다. 공간이 모자라면 설치 자체가 실패합니다(오류 112). 공간을 늘린 뒤에도 코드 43은 그대로였습니다. |
| 드라이버 패키지 축소 | 축소하지 않은 2.7 GB 원본도 26H2 PE에서 똑같이 코드 43이었고, 축소 사본은 23H2 PE에서 정상 동작했습니다. |
| PE에 빠진 커널 드라이버나 그래픽 스택 파일 | 두 PE의 파일 목록과 로드된 드라이버 목록에 의미 있는 차이가 없었습니다. |
| `GraphicsDrivers` 등 레지스트리 설정 | 두 PE가 동일했습니다. `ForceBddFallbackOnly`는 이미 0이었습니다. |
| 디스플레이 정책 서비스(`DispBrokerDesktopSvc`) 미등록 | PE에 서비스 키가 없는 것은 사실이지만, 서비스를 등록해 `RUNNING` 상태로 만들어도 `dwm.exe`는 똑같이 죽었습니다. |
| `display.inf`, `displayoverride.inf` 없음 | 23H2 PE에도 똑같이 없습니다. |

## 진단 방법

PE에서는 일반적인 도구가 동작하지 않아 다음 방법을 썼습니다.

- **`drvload`의 종료 코드는 믿을 수 없습니다.** 디스크가 가득 찬 경우와 재시작이 필요한 경우 모두 3을 돌려줍니다. 실제 결과는 `X:\Windows\INF\setupapi.dev.log`의 `Exit status`와 `!!!` 줄에 있습니다.
- **`wevtutil` 조회와 `wpr`은 PE에서 동작하지 않습니다.** `wpr`은 추적을 시작해도 공급자 이벤트가 기록되지 않았습니다.
- **ETW는 API를 직접 호출하면 동작합니다.** `StartTrace`와 `EnableTraceEx2`로 `Microsoft-Windows-DxgKrnl`, `Microsoft-Windows-Kernel-PnP` 공급자를 기록하고, Windows에서 `tracerpt -of XML -lr`로 해석했습니다. 여기서 "드라이버는 시작됐고 프로세스가 반복해서 죽는다"는 것이 드러났습니다.
- **크래시 덤프는 WER LocalDumps로 받을 수 있습니다.** `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\dwm.exe`에 `DumpFolder`를 지정하면 됩니다.
- **심벌**은 `msdl.microsoft.com`에서 PDB를 받아 dbghelp로 풀었습니다. 덤프의 스택 주소를 함수 이름으로 바꾼 뒤에야 `DispBroker.dll`의 `LoadLibrary("DDisplay.dll")` 호출이 보였습니다.

## 남은 한계

- 확인한 것은 NVIDIA 드라이버 32.0.16.1742, RTX 3060 Ti 한 대, PhoenixPE 2026.10.25입니다. AMD·Intel 드라이버에서는 시험하지 않았습니다. 다만 죽는 위치가 DWM과 Windows 구성요소 안이라 제조사와 무관하게 같은 문제가 생길 것으로 예상합니다.
- 수정한 `DisplayDriverSupport.script`로 PE를 실제로 다시 빌드해 확인하는 것은 아직 하지 않았습니다. 실행 중인 PE에 파일을 복사하는 방법으로만 확인했습니다.
