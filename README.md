# Display Twin

Android 태블릿을 Windows PC의 **펜 디스플레이**로 쓰는 프로젝트입니다.
PC 화면을 H.264로 인코딩해 UDP로 태블릿에 보내고, 태블릿의 스타일러스(필압·기울기·지우개)와
손가락 터치를 Windows의 가상 펜/터치 입력으로 되돌려 줍니다.

*Use an Android tablet as a low-latency pen display for a Windows PC (UDP video + synthetic pen/touch input).*

## 기능

- **저지연 화면 전송** — DXGI 캡처 → NVENC(없으면 Media Foundation) H.264 → UDP, XOR FEC로 패킷 손실 복구, 손실·지연에 따른 QoS 자동 조절
- **펜 입력** — 필압, 기울기, 지우개, 손바닥 거부. 태블릿에서는 지연 보정용 로컬 잉크 오버레이
- **터치 입력** — 손가락 2점까지 Windows 터치로 주입 (탭, 길게 누르기, 드래그, 두 손가락 스크롤·핀치·회전). 3·4손가락 시스템 제스처는 의도적으로 제외
- **간편 연결** — USB 테더링 / Wi-Fi 모두 지원. 자동 검색(UDP 브로드캐스트), **QR 스캔**(테더링용·Wi-Fi용 QR을 PC가 표시), 수동 IP 입력 중 선택
- PC 쪽 진단 화면(fps, 인코드/디코드 시간, 손실률, RTT, PLI/IDR 등)

## 요구 사항

| | |
|---|---|
| PC | Windows 10 1809 이상 (가상 펜/터치 API), **관리자 권한으로 실행**. NVIDIA GPU(NVENC) 권장 — 없으면 CPU 인코딩(720p로 축소)으로 자동 전환 |
| 태블릿 | Android 7.0(API 24) 이상. 펜 기능은 스타일러스 지원 기기. QR 스캔은 Google Play 서비스 필요 |
| 네트워크 | 같은 Wi-Fi 또는 USB 테더링. 방화벽에서 UDP **5000, 5001, 5002, 9999** 인바운드 허용 |

## 사용법

1. PC에서 `DisplayTwin.exe`를 관리자 권한으로 실행합니다.
2. 태블릿에 APK를 설치하고 앱을 엽니다.
3. 연결합니다.
   - **QR**: PC의 *Connect by QR*를 누르고, 태블릿에서 상태 표시를 탭 → *IP 변경* → *QR 스캔*
   - 또는 *PC 검색*, 또는 PC IP를 직접 입력
   - USB 테더링이면 태블릿에서 USB 테더링을 켠 뒤 PC에서 *Detect tethering*
4. PC에서 *Start*를 누르면 화면이 나타납니다.

## 빌드

**PC** — Visual Studio (v145 툴셋, Windows SDK 10.0.26100) 에서 `host/PenDisplayPC.sln`을 x64로 빌드합니다. 명령줄에서는 PowerShell로:

```powershell
MSBuild "host\PenDisplayPC.sln" -p:Configuration=Release -p:Platform=x64
```

프로토콜 단위 테스트: `host/tests/ProtocolTests.vcxproj`

**Android** — Android Studio로 `android`를 열거나:

```bash
cd android
./gradlew assembleDebug
./gradlew testDebugUnitTest
```

## 구조

```
host/               PC 호스트 (C++, ImGui)
  src/ include/        캡처, 인코더, UDP 전송, 가상 펜/터치, GUI
  docs/Protocol.md     와이어 프로토콜·복구/QoS 정책 상세
  tests/               프로토콜 단위 테스트
android/            태블릿 앱 (Kotlin)
```

프로토콜 설명은 [`host/docs/Protocol.md`](host/docs/Protocol.md)를 보세요.

## 주의

- 현재 **암호화·인증이 없습니다.** 같은 네트워크의 누구든 호스트에 접속해 펜/터치 입력을 보낼 수 있으므로 신뢰할 수 있는 네트워크(또는 USB 테더링)에서만 사용하세요.
- 호스트는 관리자 권한이 필요합니다(가상 입력 장치 생성, 네트워크 경로 우선순위 조정).
- 캡처 대상은 주 모니터입니다.

## 라이선스

[MIT](LICENSE). 포함된 서드파티: [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[QR Code generator](https://github.com/nayuki/QR-Code-generator) (MIT),
NVIDIA Video Codec SDK 헤더 (MIT), Manrope / JetBrains Mono (SIL OFL, `third_party/fonts`).
