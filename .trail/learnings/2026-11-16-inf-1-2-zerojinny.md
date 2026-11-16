---
title: "슬림 사본의 패키지 범위와 INF 정리 (1.2)"
author: Zerojinny
date: 2026-11-16
branch: master
cost: { minutes: 19, tokens: 115659 }
id: l-d31f6843
scope: repo
status: active
src: agent
touches:
  - "src/pedrv.cpp#SkipSubdir"
  - "src/pedrv.cpp#AddSlimRules"
  - "src/pedrv.cpp#SlimDrops"
---

## Decided
- 슬림 사본의 패키지 범위 = INF 폴더에서 '자체 INF가 있는 하위 폴더'를 뺀 것 (SkipSubdir). 단, 선택된 INF가 CopyINF / SourceDisksNames(필드 4) / SourceDisksFiles(필드 2)로 참조하는 하위 폴더는 남긴다. 이유: Intel 그래픽 패키지(gfx_win_101.8864)의 CtaChildDriver, DisplayAudio\12.1 등은 기본 INF가 참조하지 않는 독립 패키지인데, AMD 설치본 구성은 CopyINF = .\amdxe\amdxe.inf처럼 하위 폴더 INF를 참조한다. 하위 폴더 INF를 일괄 제외하면 AMD가 깨진다.
- 선택된 INF와 그 CopyINF 대상 외의 .inf는 비운다. NVIDIA nv_dispsi.inf는 같은 폴더의 nvppc.inf를 CopyINF로 지정하고, AMD u0205305.inf는 같은 폴더의 amduw23e.inf를 지정한다. 이것들을 비우면 안 된다.
- /pe 반복 패스에서 같은 폴더의 다른 INF가 나중에 선택되면, 앞서 만든 사본에서는 그 INF가 비어 있으므로 슬림 사본을 새로 만든다 (SlimCopyInfo.infs로 판별).
- /slim:max는 OpenGL·Vulkan 드라이버만 추가로 비우고 Vulkan 로더(vulkan-1.dll)는 남긴다. 0바이트 로더가 System32에 있으면 로더를 찾는 프로그램이 이상하게 실패할 수 있어서다.
## Watch out
- NVIDIA는 이 PC에 장치가 없어 /all로만 측정했다. 그래서 INF 정리 효과가 측정값에 빠져 있다. 다른 장치용 INF 규칙과 /slim:max는 PE에서 시험하지 않았다.
- 검증 방법: 스크래치에 AMD INF + other.inf + CopyINF 대상 + 자식 INF 폴더로 만든 가짜 패키지를 /export해 확인했다.
