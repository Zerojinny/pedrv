# 그래픽 드라이버 축소 규칙과 추가 후보

`/slim`·`/export`로 축소 사본을 만들 때 NVIDIA·AMD 디스플레이 패키지에서 비우는 파일, 남기는 파일, 그리고 더 비울 수 있는 파일의 목록입니다. 코드는 [src/pedrv.cpp](../src/pedrv.cpp)의 `AddSlimRules`·`SlimDrops`입니다.

측정한 패키지 (2026-11-16):

| 패키지 | 원본 | 축소 (기본) | 축소 + `/keep32` |
|---|---|---|---|
| NVIDIA 610.88 (`Display.Driver`, INF 45개) | 2,747 MB, 265개 파일 | 793 MB (115개 비움) | - |
| AMD 32.0.21045.16010 (RX 6600, `u0205305.inf`) | 1,171 MB, 136개 파일 | 491 MB (44개 비움) | 813 MB (7개 비움) |

## 축소 방식

- 파일을 지우지 않고 **0바이트 빈 파일**로 바꿉니다. 패키지 구조와 INF·카탈로그는 그대로입니다. INF의 `CopyFiles`가 가리키는 파일이 빠지면 설치가 실패하므로, 파일은 남겨 두고 내용만 비웁니다.
- 빈 파일은 카탈로그 해시와 맞지 않지만, 부팅한 WinPE는 이를 무시합니다. 그래서 축소 사본은 **부팅한 PE에서 올릴 때만** 동작하고, boot.wim에 미리 통합하면 동작하지 않습니다 (CLAUDE.md "드라이버 축소" 참고).
- `.inf`, `.cat`, `.sys`는 어떤 규칙에도 걸리지 않습니다.
- 규칙은 패키지 폴더 단위로 적용합니다. 그 폴더에서 선택된 INF의 제공자와 클래스로 NVIDIA·AMD 규칙을 켭니다.

## 현재 규칙

### 모든 패키지

| 규칙 | 판별 | 비고 |
|---|---|---|
| WSL/Linux용 | INF가 `lxss` 폴더로 복사하는 파일, `*.so`, `*.so.*` | PE에는 WSL이 없음 |
| 32비트 실행 파일 | PE 헤더의 Machine이 i386 | WoW64가 있는 PE나 x86 대상, `/keep32`면 남김 |

### NVIDIA (제공자에 `NVIDIA`, 클래스 `Display`)

파일 이름이 다음으로 시작하면 비웁니다.

| 접두어 | 내용 |
|---|---|
| `NVCUDA`, `NVCUBINS`, `NVPTXJIT` | CUDA |
| `NVOPTIX`, `NVRTUM` | OptiX, 레이 트레이싱 런타임 |
| `NVVM` | NVVM 컴파일러 |
| `NVOPENCL`, `OPENCL` | OpenCL |
| `NVCUVID`, `NVENCODEAPI` | 비디오 디코드/인코드 API |
| `NVDXDLKERNELS`, `NVTILEIRAS` | DirectML 커널 |
| `NVDLPPX`, `NVDIRECTSR`, `NVNGX`, `NVAIVPX` | DLSS·NGX, AI 업스케일 |
| `NVOBJECTLOADER` | |
| `NVTELEMETRY`, `NVGAMES`, `NVIDIA-PCC` | 텔레메트리, 게임 설정 |

그리고 `NvCamera\` 폴더 전체(Ansel)를 비웁니다.

일부러 남기는 것: 커널 드라이버와 펌웨어, D3D·OpenGL·Vulkan 드라이버, `nvgpucomp`(셰이더 컴파일러), 제어판, 컨테이너 서비스.

### AMD (제공자가 `Advanced Micro Devices` 또는 `AMD`로 시작, 클래스 `Display`)

| 접두어 | 파일 | 크기 (64+32비트) | 내용 |
|---|---|---|---|
| `AMDXCFFX` | `amdxcffx64.dll` | 82 MB | FidelityFX 라이브러리 |
| `AMDVLK_NEXUS` | `amdvlk_nexus.cab` | 68 MB | 대체 Vulkan 드라이버 (INF의 `VulkanDriverNameNexus`) |
| `AMFRTDRV` | `amfrtdrv64.dll`, `amfrtdrv32.dll` | 30 + 29 MB | AMF 비디오 런타임 |
| `AMDINSTALLMANAGER` | `AMDInstallManager.msi` | 27 MB | 소프트웨어 설치 파일 |

일반 Windows에서 실행 중인 프로세스의 로드된 모듈과 바이너리 안의 파일 이름 참조를 보고 정했습니다.

- `amdxcffx64.dll`은 `amdxc64.dll`(D3D12)과 `amdvlk64.dll`이 참조하지만, `amdxc64.dll`을 불러온 Firefox에도 올라와 있지 않았습니다. 필요할 때만 불러오는 것으로 봅니다.
- `amdvlk_nexus.cab`은 `regamdcomp.exe`와 INF만 참조하고, `AMDInstallManager.msi`는 `atieclxx.exe`만 참조합니다.

이 규칙으로 축소한 RX 6600 패키지를 부팅한 PE에서 올려 정상 동작하는 것을 확인했습니다 (2026-11-16).

**비우면 안 되는 파일**: D3D11 드라이버 `amdxx64.dll`은 D3D를 쓰는 모든 프로세스(explorer, 셸 호스트 등)에서 `amdenc64.dll`·`amdihk64.dll`을 함께 불러오고, `amdn1b64.dll`(19 MB)을 이름으로 참조합니다. `amdenc`는 이름이 인코더처럼 보여도 코덱 규칙에 넣으면 안 됩니다.

## 축소 후 남는 큰 파일

### NVIDIA 610.88 (793 MB)

| 크기 | 파일 | 설명 |
|---|---|---|
| 114.8 MB | `nvlddmkm.sys` | 커널 드라이버 |
| 94.8 MB | `nvgpucomp64.dll` | 셰이더 컴파일러 |
| 86.3 MB | `nvwgf2umx.dll` | D3D10/11/12 사용자 모드 드라이버 |
| 80.4 MB | `gsp_ga10x.bin` | GSP 펌웨어 |
| 46.2 MB | `nvoglv64.dll` | OpenGL ICD (Vulkan도 이 DLL) |
| 41.8 MB | `nvd3dumx.dll` | D3D9 드라이버 |
| 28.0 MB | `gsp_tu10x.bin` | GSP 펌웨어 |
| 52.6 MB | `*.inf` 45개 | 선택된 INF(`nv_dispsi.inf` 0.7 MB) 외 44개는 다른 제품군용 |
| 40.9 MB | `Display.NvContainer\` | 컨테이너 서비스와 플러그인 |
| 약 100 MB | 제어판·서버 DLL | 아래 후보 표 참고 |

### AMD 32.0.21045.16010 (491 MB)

| 크기 | 파일 | 설명 |
|---|---|---|
| 98.0 MB | `amdkmdag.sys` | 커널 드라이버 |
| 98.0 MB | `amdvlk64.dll` | Vulkan 드라이버 |
| 78.6 MB | `amdxc64.dll` | D3D12 드라이버 |
| 61.3 MB | `atio6axx.dll` | OpenGL 드라이버 |
| 48.1 MB | `amdxx64.dll` | D3D10/11 드라이버 |
| 38.8 MB | `amdxn64.dll` | D3D9 드라이버 |
| 19.3 MB | `amdn1b64.dll` | `amdxx64.dll`이 참조 (남겨야 함) |
| 약 25 MB | 그 밖의 파일 | 아래 후보 표 참고 |

## 추가로 비울 수 있는 파일 (미적용)

아직 규칙에 넣지 않은 후보입니다. **PE에서 시험한 것은 하나도 없습니다.** 위험도는 판단 근거를 함께 적은 추정입니다.

- 낮음: PE에서 화면 표시와 관계없는 부가 기능
- 중간: 일부 프로그램이나 서비스가 동작하지 않을 수 있음
- 높음: 화면 표시나 드라이버 시작에 영향을 줄 수 있음

### NVIDIA

| 후보 | 크기 | 위험도 | 근거와 영향 |
|---|---|---|---|
| 선택되지 않은 INF 44개 | 약 52 MB | 낮음 | drvload는 선택된 INF만 처리합니다. 비운 INF는 `pedrv /pe <폴더>`가 소스로 읽을 때 건너뜁니다. 규칙은 "선택된 INF 외의 `.inf`"로 만들어야 하므로 `SlimRules`에 선택된 INF 이름이 필요합니다 |
| `nvvkscv64.dll` | 11.2 MB | 낮음 | Vulkan SC(안전 인증용) 드라이버. INF의 `VulkanSCDriverName` |
| 캡처·관리 도구: `nvfbc64.dll`, `nvifr64.dll`, `nvml.dll`, `nvml_loader.dll`, `nvidia-smi.exe`, `nvencmfth264x.dll`, `nvencmfthevcx.dll`, `nvencmftav1x.dll`, `vulkaninfo-x64.exe` | 약 16 MB | 낮음 | 화면 캡처, 관리 도구, 인코더 MFT. 데스크톱 표시에는 쓰지 않음 |
| AI 화면 기능: `nvaihdrx.dll`, `nvaidvcx.dll`, `_nvngx.dll` | 약 8 MB | 낮음 | RTX HDR, AI 색감 보정, NGX 런타임. 기존 `NVNGX` 접두어 규칙이 앞의 `_` 때문에 `_nvngx.dll`을 놓침 |
| 제어판·서버: `nvcpl.dll`, `NVCPL\*.appx`, `nvwss.dll`, `nvwssr.dll`, `nvdisps.dll`, `nvdispsr.dll`, `nvvitvs.dll`, `nvvitvsr.dll`, `nvsvs.dll`, `nvsvsr.dll`, `nvdevtools.dll`, `nvlicensings.dll`, `nvui.dll`, `nvuir.dll`, `nvxdapix.dll`, `nvxdplcy.dll`, `nvxdbat.dll` | 약 100 MB | 중간 | PE에서는 제어판을 쓰지 않습니다. 다만 `nvxd*`·`nvui`는 "User Experience Driver Component"로 컨테이너 서비스가 불러오며, 현재 규칙은 제어판을 일부러 남깁니다 |
| 컨테이너 서비스: `Display.NvContainer\`, `NVWMI\`, `nvsvc64.dll`, `nvsvcr.dll` | 약 49 MB | 중간 | INF가 서비스로 등록하는 파일입니다. 비우면 서비스가 시작하지 못하는데, 이것이 drvload 결과나 장치 상태에 영향을 주는지 확인하지 않았습니다 |
| 다른 세대의 GSP 펌웨어: `gsp_tu10x.bin` 또는 `gsp_ga10x.bin` | 28 / 80 MB | 높음 | GPU 세대마다 하나만 씁니다. 하지만 어느 GPU가 어느 파일을 쓰는지 확인하지 않았고, 하드웨어 ID로 고르는 로직이 필요합니다. 잘못 비우면 커널 드라이버가 시작하지 못합니다 (코드 43 예상) |
| `nvd3dumx.dll` (D3D9) | 41.8 MB | 중간 | DWM은 D3D11을 씁니다. D3D9를 쓰는 PE 프로그램은 동작하지 않습니다 |
| `nvoglv64.dll` (OpenGL·Vulkan) | 46.2 MB | 중간 | OpenGL과 Vulkan을 함께 담고 있어 둘 다 사라집니다 |
| `nvpresent64.dll`, `nvppex.dll`, `nvcoproc.bin` | 8.0 / 7.2 / 12.1 MB | 미상 | 용도를 확인하지 못했습니다. 조사 전에는 비우지 않습니다 |

계속 남겨야 하는 것: `nvlddmkm.sys`, `nvldumdx.dll`(`UserModeDriverName`의 로더), `nvwgf2umx.dll`, `nvgpucomp64.dll`, `nvd3d12scx.dll`, `nvdrsdb.bin`(프로필 데이터베이스), `nvapi64_impl.dll`.

모두 적용하면(GSP·미상 제외) 약 793 → 510 MB, D3D9·OpenGL까지 비우면 약 420 MB로 추정합니다.

### AMD

| 후보 | 크기 | 위험도 | 근거와 영향 |
|---|---|---|---|
| FSR 업스케일 데이터: `H9_EASU_*.bin` 10개 | 3.6 MB | 낮음 | RSR(드라이버 업스케일)용 |
| 비디오 코덱: `AMDh264Enc64.dll`, `AMDh265Enc64.dll`, `amf-mft-mjpeg-decoder64.dll`, `amf-pa-ml64.dll`, `AMDhwDecoder_64.dll` | 약 6.5 MB | 낮음 | Media Foundation 인코더·디코더. `amfrtdrv`가 이미 비어 있어 PE에서는 동작하지 않음. **`amdenc64.dll`은 제외** |
| 비디오 디코드 데이터: `ativvaxy_*.dat` | 7.5 MB | 낮음~중간 | DXVA 디코드용으로 보이며 세대별로 나뉨. 용도를 확인하지는 않음 |
| Radeon 소프트웨어 부품: `amdadlx64.dll`, `atiadlxx.dll`, `amdsasrv64.dll`, `amdsacli64.dll`, `GameManager64.dll`, `amdlvr64.dll`(VR), `amdmiracast.dll`, `amdgfxinfo64.dll`, `featuresync.dll`, `vulkaninfo64.exe` | 약 15 MB | 중간 | Radeon Software가 불러오는 것들(`RadeonSoftware` 프로세스에서 확인). `atiadlxx.dll`(ADL)은 다른 프로그램도 쓸 수 있음 |
| 외부 이벤트 서비스: `atiesrxx.exe`, `atieclxx.exe`, `atieah64.exe`, `EEURestart.exe` | 약 2.6 MB | 중간 | INF가 `AMD External Events Utility` 서비스로 등록. 시작 실패의 영향은 확인하지 않음 |
| Vulkan: `amdvlk64.dll`, `vulkan64.dll`(System32의 `vulkan-1.dll`) | 약 100 MB | 중간 | Vulkan 프로그램이 동작하지 않음. 가장 효과가 큼 |
| OpenGL: `atio6axx.dll`, `atig6pxx.dll` | 약 61 MB | 중간 | OpenGL 프로그램이 동작하지 않음 (`steam` 프로세스가 불러옴) |
| D3D9: `amdxn64.dll`, `atidx9loader64.dll` | 약 39 MB | 중간~높음 | DWM은 쓰지 않지만 D3D9 프로그램이 꽤 많음 (`parsecd`, `MSPCManager`, `RadeonSoftware`가 불러옴) |
| D3D12: `amdxc64.dll` | 78.6 MB | 높음 | D3D12 프로그램과 WinUI 일부가 동작하지 않음. System32의 `amdxc64.dll`은 `amdxcstub64.dll` 사본이라 따로 판단해야 함 |

계속 남겨야 하는 것: `amdkmdag.sys`, `amdxx64.dll`, `amdn1b64.dll`, `amdenc64.dll`, `amdihk64.dll`, `atidxxstub64.dll`(System32의 `atidxx64.dll`, 모든 D3D 프로세스가 불러옴), `amdicdxx.dat`·`atiicdxx.dat`, `atiapfxx.blb`.

낮음 항목만 적용하면 약 491 → 475 MB로 효과가 작습니다. Vulkan·OpenGL까지 비우면 약 330 MB, D3D9까지 약 290 MB로 추정합니다.

## 후보를 적용하기 전에

1. 축소 사본을 PE에서 올려 장치가 정상(문제 코드 없음)이고 해상도를 바꿀 수 있는지 확인합니다.
2. 후보 묶음을 하나씩 더해 같은 시험을 반복합니다. 코드 43이나 DWM 재시작이 생기면 마지막에 더한 묶음이 원인입니다.
3. 규칙을 넣으면 이 문서의 해당 행을 "현재 규칙"으로 옮기고 측정 크기를 갱신합니다.
