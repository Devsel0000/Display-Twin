# Display Twin v4.2 프로토콜 기준

PC 호스트와 Android 앱이 공유하는 현재 기준이다. 프로토콜 v2부터 모든 다바이트
필드는 네트워크 바이트 순서(big-endian)로 직렬화한다.

## 공통값

- Magic: `0xABBA`
- Version: `0x02`
- 영상 UDP: `5000`
- 펜 UDP: `5001`
- 제어 UDP: `5002`
- Discovery UDP: `9999`
- 안전 MTU: `1400`
- 영상 fragment payload: 최대 `1280`바이트
- 소켓 버퍼: 송·수신 양쪽 `4MB`
- 기준 영상: H.264/AVC, FHD, 60fps

## 영상 패킷

헤더는 19바이트다.

```text
0   2  Magic
2   1  Version
3   4  SessionID
7   2  FrameID
9   4  Timestamp (monotonic milliseconds)
13  2  FragmentIndex
15  2  FragmentCount
17  2  PayloadLength
19  N  H.264 payload
```

NVENC 초기화는 `nvEncGetEncodePresetConfigEx`로 P1/저지연 preset의 기본
`NV_ENC_CONFIG`를 먼저 얻어온 뒤, 그 위에 rate control(CBR, 목표 비트레이트/VBV)과
`repeatSPSPPS=1`, `idrPeriod`만 덮어써서 사용한다. 직접 처음부터 만든
`NV_ENC_CONFIG`는 드라이버가 `NV_ENC_ERR_INVALID_PARAM (8)`로 거부했기 때문이며,
preset에서 얻은 구조체를 기반으로 하면 필수 필드가 모두 채워져 있어 초기화가
통과된다. `sliceMode`는 현재 0(슬라이스 미사용)으로 두고 있으며, 전송 계층이
NVENC 출력 buffer를 안전한 최대 1280바이트 fragment로 나누고, 각 fragment의 실제
길이를 `PayloadLength`에 기록한다.

PC의 실제 캡처 해상도(모니터 해상도)가 1920x1080이 아닐 수 있으므로, Android
쪽은 `MediaCodec`의 `INFO_OUTPUT_FORMAT_CHANGED`에서 실제 스트림 폭/높이를
읽어 letterbox 및 터치 좌표 정규화 기준을 그 값으로 갱신한다.

### 인코더 선택

NVENC 초기화가 실패하면(= NVIDIA GPU가 없는 PC) Windows 자체 H.264 인코더
(Media Foundation 소프트웨어 MFT)로 자동 전환한다. 이 경로는 CPU로 돌기 때문에
**720p로 축소**해서 보내고(`MediaFoundationEncoder::kMaxHeight`) 비트레이트도
8Mbps로 제한한다. 캡처 프레임을 GPU에서 읽어와 박스 필터로 축소하고 NV12로
변환하는 비용이 있어 NVENC보다 지연이 확연히 크다 — "돌아가게 하는" 경로다.
태블릿은 SPS에서 실제 해상도를 읽으므로 별도 처리가 필요 없고, 펜 좌표는
정규화되어 있어 축소와 무관하다. IDR 정책(주기적 IDR 없음, PLI로만)은 동일하다.

캡처 대상은 **주 모니터**다. DXGI의 출력 순서는 모니터 배치와 무관하므로
`EnumOutputs(0)`이 아니라 데스크톱 원점(0,0)에 놓인 출력을 찾아 선택하고, 그
출력의 데스크톱 사각형을 `VirtualPen::SetTargetRect()`에 넘긴다. 펜 좌표는 이
사각형에 매핑되므로 다중 모니터에서도 태블릿에 보이는 화면과 펜이 찍히는 화면이
항상 일치한다.

### XOR FEC (v4.2 신규)

데이터 fragment 8개마다 XOR 패리티 1개를 추가로 보낸다. 패리티는 같은 19바이트
헤더를 쓰고 `FragmentIndex = FragmentCount + 그룹번호`로 주소를 매긴다. 즉
`FragmentIndex >= FragmentCount`면 패리티다. FEC를 모르는 수신기는 이 패킷을
버리기만 하면 되고 데이터 fragment만으로 그대로 재조립된다.

- **프레임의 마지막 fragment는 보호하지 않는다.** 마지막 fragment만 길이가 짧기
  때문인데, 그룹 구성원을 전부 1280바이트로 고정해야 유실된 fragment의 길이를
  모르고도 복원할 수 있다.
- 그룹 구성원이 2개 미만이면 패리티를 만들지 않는다(복원할 게 없다).
- 한 그룹에서 정확히 1개가 비고 패리티가 도착하면 나머지 구성원과 패리티를
  XOR해서 복원한다. 2개 이상 비면 복원 불가 → PLI 경로로 넘어간다.
- 대역폭 비용은 큰 프레임에서 약 12.5%다. `HostSettings::enableFec`로 끌 수 있다.

## 펜 입력 패킷

헤더는 23바이트다.

```text
0   2  Magic
2   1  Version
3   4  SessionID
7   2  Sequence
9   1  Action (DOWN=0, MOVE=1, UP=2, CANCEL=3)
10  1  Flags
11  2  Pressure (1..4095)
13  1  TiltX (+90 오프셋, 0..180)
14  1  TiltY (+90 오프셋, 0..180)
15  2  X (0..32767)
17  2  Y (0..32767)
19  4  Timestamp (monotonic milliseconds)
```

Flags bit 0은 eraser, bit 1은 palm rejection, bit 2는 stylus, bit 3은 UP 패킷에
마지막 MOVE 좌표가 포함됐음을 뜻한다.

- **Tilt 변환**: 전송 측은 도(°)를 `+90` 오프셋해 1바이트에 담고, PC는
  `(byte - 90) * π/180`로 라디안을 복원한 뒤 주입 직전 `* 180/π`로 되돌린다.
  (v4.1까지 마지막 단계가 `* 90/π`여서 모든 기울기가 정확히 절반으로
  들어갔다.)
- **Eraser**: Flags bit 0이 서면 PC는 `PEN_FLAG_ERASER | PEN_FLAG_INVERTED`로
  주입한다. Android 쪽 트리거(측면 버튼/토글)는 아직 없어 항상 0을 보낸다.
- **전송 빈도 제한 없음**: 디지타이저가 만드는 샘플은 그대로 전부 보낸다.
  v4.1까지 `PenSurfaceView`(2ms)와 `PenInputSender`(2ms)에 이중 스로틀이 있어서,
  `MotionEvent`의 historical 샘플을 루프로 내보내면 첫 개만 남고 나머지가 전부
  버려졌다.
- **상태 변화만 ACK**: DOWN/UP/CANCEL은 제어 채널의 PENACK을 받을 때까지
  `max(RTT×2, 40ms)` 간격으로 최대 4회 재전송한다. MOVE는 재전송하지 않는다 —
  도착할 즈음이면 이미 늦었고, PC가 sequence gap을 보간한다.
- **Sequence gap 보간**: PC는 MOVE 사이가 벌어지면 마지막 수락 샘플과 새 샘플
  사이를 선형 보간해 최대 8개까지 채워 넣는다. 그 이상 벌어지면 실제로 긋지
  않은 선을 만들어내게 되므로 채우지 않는다.

## 제어 채널 (UDP 5002)

공통 헤더 10바이트:

```text
0   2  Magic
2   1  Version
3   4  SessionID
7   1  Type
8   2  PayloadLength
10  N  Payload
```

| Type | 이름 | 방향 | Payload |
|---|---|---|---|
| `0x01` | PING | 태블릿 → PC | t1 (8B) |
| `0x02` | PONG | PC → 태블릿 | t1, t2, t3 (각 8B) |
| `0x11` | PLI | 태블릿 → PC | 없음 |
| `0x20` | HELLO | 태블릿 → PC | 클라이언트 버전 문자열 |
| `0x21` | WELCOME | PC → 태블릿 | SessionID (4B) + PC 버전 문자열 |
| `0x22` | BUSY | PC → 태블릿 | 없음 |
| `0x30` | STATUS | 태블릿 → PC | 12B (아래) |
| `0x40` | PENACK | PC → 태블릿 | Sequence (2B) |

STATUS payload:

```text
0   1  DecodeQueueLength
1   1  (reserved)
2   2  LossPermille
4   2  CompletedFps
6   2  DroppedFrames
8   2  DecodeMs ×10
10  2  RttMs
```

### 세션 생명주기

```
[Discovery] (선택)
  태블릿 → 브로드캐스트 255.255.255.255:9999  "DTWIN-DISCOVER/1"
  PC     → 유니캐스트                        "DTWIN-OFFER/1|<hostname>|<version>"

[Handshake]
  태블릿 → HELLO      (SessionID=0)
  PC     → WELCOME    (랜덤 SessionID 발급, 0 제외)
         · PC는 HELLO가 온 주소로 영상 목적지를 즉시 재지정한다
         · 활성 세션이 살아 있고 다른 주소면 BUSY (동시 1대)
         · 펜 세션 리셋 + 강제 IDR

[Clock Sync]
  최초 8회 연속 PING/PONG, 이후 1초마다 1회, 30초마다 다시 8회
  RTT    = (t4 - t1) - (t3 - t2)
  Offset = ((t2 - t1) + (t3 - t4)) / 2      (= PC 시계 - 태블릿 시계)
  최소 RTT 샘플의 Offset을 채택하고, 갱신은 (new - cur)/4 만큼만 slew

[Teardown]
  제어 채널 3초 무응답 → 세션 해제, 가상 펜 강제 릴리즈, 다음 HELLO 개방
```

SessionID는 영상·펜 패킷 헤더에 모두 실린다. PC는 세션이 있으면 그 세션의
펜 패킷만 받고, 태블릿은 세션이 있으면 그 세션의 영상만 받는다. 세션 이전
단계에서 오는 SessionID=0 패킷은 양쪽 다 허용한다(핸드셰이크 직전 구간).

태블릿의 영상 수신 포트는 추가로 **설정된 PC IP에서 온 패킷만** 처리한다.
암호화·인증은 여전히 없다 — 같은 LAN에서의 우발적 충돌과 장난을 막는 수준이다.

## 복구 및 지연 정책

- NACK 기반 영상 재전송은 사용하지 않는다. 왕복이 지연 목표보다 크다.
- 1차 방어는 FEC다. 그룹당 1개 손실은 왕복 없이 로컬 복원한다.
- 복원 불가 손실 → Android가 PLI 전송(최소 간격 100ms).
- PC는 PLI 수신 시 다음 프레임을 강제 IDR로 낸다(`NvencEncoder::RequestKeyframe`).
  PC 쪽에서도 100ms로 합쳐서 IDR 폭주를 막는다.
- `repeatSPSPPS=1`을 항상 활성화해 IDR마다 SPS/PPS를 함께 출력한다.
- Android는 IDR 수신 전 P-frame을 디코더에 넣지 않는다(`DecoderPipeline`의
  keyframe gate). 손실 직후·큐 오버플로·디코더 오류 후 모두 이 상태로 돌아간다.
- 부분 프레임 timeout은 80ms다.
- 마지막으로 완성된 FrameID와 **같은** FrameID의 패킷은 버린다. 패리티가 데이터
  뒤에 오기 때문에, 이게 없으면 방금 완성된 프레임이 빈 프레임으로 되살아나
  timeout → PLI를 매 프레임 일으킨다(v4.2 초기 빌드의 IDR 폭주 원인).
- 디코더 입력 큐는 4프레임이며, 가득 차면 **가장 최근 프레임을 버린다**(이미
  큐에 있는 프레임을 뒤 프레임들이 참조하기 때문).
- 클럭 동기가 끝난 뒤 **250ms**보다 오래된 프레임만 버리고 PLI를 보낸다. 명세의
  50ms는 폐기가 공짜라는 전제였지만, P-frame 하나를 버리면 IDR이 필요하고
  저지연 CBR의 IDR은 눈에 띄게 흐리다. 약간 늦은 프레임은 늦게라도 보여주는 편이
  낫고, 지연 누적은 4프레임 큐가 이미 막는다. 동기 전에는 이 검사를 건너뛴다.
- MediaCodec이 예외를 내면 `flush()` → 실패 시 재생성으로 복구하고 keyframe을
  다시 기다린다.
- 정지 화면에서도 PC가 1초에 한 번 마지막 화면을 **일반 P-frame**으로 다시
  보낸다(변화 없는 화면이라 거의 전부 skip 블록). 완전한 무음은 태블릿 입장에서
  연결 끊김과 구분되지 않는다.

### IDR 정책

저지연 CBR(VBV = 1프레임)에서는 IDR이 한 프레임 예산으로 화면 전체를 다시
그려야 하므로 주변 P-frame보다 확연히 흐리고, 이후 P-frame이 다시 선명하게
만든다. 화면이 "숨 쉬는" 것처럼 보이므로 IDR은 필요할 때만 낸다.

- NVENC `idrPeriod = NVENC_INFINITE_GOPLENGTH` — **주기적 IDR 없음**
  (v4.1까지는 `idrPeriod = framerate`로 매초 IDR이 있었다).
- IDR을 내는 경우: 인코더 시작 직후 5프레임, HELLO 수신, PLI 수신(100ms 합침).
- **세션이 없을 때만** 2초마다 IDR. 제어 채널을 쓸 수 없는 클라이언트(5002
  차단, 구버전 앱)는 PLI를 보낼 방법이 없어 이게 유일한 복구 수단이다.
- 호스트가 재시작되면 태블릿은 PONG의 SessionID가 자기 것과 다른 것을 보고
  세션을 버리고 HELLO부터 다시 한다.

## QoS 자동 조절

PC가 2초마다 평가한다. 입력은 자신의 인코딩 시간과 태블릿 STATUS(손실률,
디코더 큐 길이)다.

- **강등**(2회 연속): 인코딩 > 15ms **또는** 손실 > 2.0% **또는** 큐 > 4
  1. 비트레이트 ×0.7 (하한 4Mbps)
  2. 그래도 나쁘면 30fps
  3. 그래도 나쁘면 비트레이트 하한
- **손실 전용 강등**(3회 연속, 손실 > 3.0%): 비트레이트 ×0.7
- **복귀**(5회 연속): 인코딩 < 8ms **그리고** 손실 < 0.5% **그리고** 큐 < 2
  → framerate 먼저 복구, 그 다음 비트레이트 ×1.3 (기준값 상한)

`nvEncReconfigureEncoder`를 `resetEncoder=0`으로 호출하므로 QoS 단계 변경
자체는 IDR을 유발하지 않는다. `HostSettings::enableQos`로 끌 수 있다.

## 계측

- PC는 매초 capture/encode(submit·readback)/send 평균, 프레임 크기, idle/capped,
  IDR 개수와 크기, PLI 누계, 태블릿 STATUS(fps·손실·큐·디코드·RTT)를 로그와
  GUI 타일에 낸다.
- 세션 로그는 `logs/session_YYYYMMDD_HHMMSS.log`로 남는다.
- 태블릿은 상태 패널에 세션/RTT/클럭 동기 여부/손실률/FEC 복원 수/PLI 수/
  디코더 큐·시간·드롭/펜 재전송 수를 보여준다.

여기까지가 실측 가능한 구간이다. **캡처부터 태블릿 표시까지의 종단 지연(A~G)은
아직 하나의 값으로 측정하지 않는다.** RTT와 디코드 시간이 생겼으므로 다음
단계에서 합산할 수 있다.

## 현재 구현 상태

구현됨:

- PC: DXGI 주 모니터 캡처, NVENC 초기화/CBR/SPS-PPS 반복, v2 packetizer, XOR FEC,
  제어 채널(핸드셰이크·클럭·PLI·STATUS·PENACK), Discovery 응답, 강제 IDR,
  정지 화면 keepalive, QoS 자동 조절, 펜 source lock + 세션 검증 + gap 보간 +
  eraser 주입 + tilt 정정, 세션 로그, ImGui 진단 타일
- Android: v2 파서/재조립기 + FEC 복원, 세션·소스 제한, 디코더 파이프라인
  (전용 스레드·4프레임 큐·keyframe gate·50ms 폐기·flush 복구), 제어 채널
  (HELLO/PING/PLI/STATUS/PENACK), Discovery 검색 UI, 펜 ACK 재전송, 팜 리젝션,
  WifiLock/WakeLock, 측정된 RTT 기반 잉크 오버레이 hold
- 테스트: `tests/ProtocolTests.vcxproj`(PC 프로토콜), `app/src/test`(재조립기·FEC·
  손실 집계·keyframe 판정)

아직 없음:

- 종단 지연(A~G) 단일 측정값
- 암호화/인증 (페어링 시크릿)
- Android 쪽 eraser 트리거 UI
- 해상도 강등(720p)까지 포함한 4단계 QoS
- USB `ITransport` 구현
