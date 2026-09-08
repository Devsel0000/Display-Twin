# Display Twin v4.1 프로토콜 기준

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
`NV_ENC_CONFIG`를 먼저 얻어온 뒤, 그 위에 rate control(CBR_LOWDELAY_HQ,
목표 비트레이트/VBV)과 `repeatSPSPPS=1`, `idrPeriod`만 덮어써서 사용한다.
직접 처음부터 만든 `NV_ENC_CONFIG`는 드라이버가 `NV_ENC_ERR_INVALID_PARAM (8)`로
거부했기 때문이며, preset에서 얻은 구조체를 기반으로 하면 필수 필드가 모두
채워져 있어 초기화가 통과된다. `sliceMode`는 현재 0(슬라이스 미사용)으로 두고
있으며, 전송 계층이 NVENC 출력 buffer를 안전한 최대 1280바이트 fragment로
나누고, 각 fragment의 실제 길이를 `PayloadLength`에 기록한다. 수신 측은 같은
FrameID의 fragment를 모두 모은 뒤 원래 순서대로 연결해 H.264 decoder에 전달한다.
NVENC 자체 slice 분할(`sliceMode=1/3`)로 전환하더라도 이 재조립 로직은 그대로
동작한다.

PC의 실제 캡처 해상도(모니터 해상도)가 1920x1080이 아닐 수 있으므로, Android
쪽은 `MediaCodec`의 `INFO_OUTPUT_FORMAT_CHANGED`에서 실제 스트림 폭/높이를
읽어 letterbox 및 터치 좌표 정규화 기준을 그 값으로 갱신한다.

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

## 복구 및 지연 정책

- NACK 기반 영상 재전송은 사용하지 않는다.
- fragment/frame 손실 시 Android가 PLI를 보낸다.
- PC는 PLI 수신 후 강제 IDR을 출력한다.
- `repeatSPSPPS=1`을 항상 활성화해 IDR마다 SPS/PPS를 함께 출력한다.
- Android는 IDR 수신 전 P-frame을 decoder에 넣지 않는다.
- 부분 프레임 timeout은 50~80ms 범위로 둔다.
- PLI 최소 간격은 100ms다.
- IDR 여부, IDR 크기, 인코딩 시간, 송신 시간, 손실률을 별도 통계로 기록한다.

## 현재 구현 상태

- PC 화면 캡처 및 NVENC 초기화: 성공
- PC v2 영상 packetizer: 적용
- PC NVENC 비트레이트/CBR/SPS-PPS 반복 설정: preset config 기반으로 적용 (`NvencEncoder::ConfigureEncoder`)
- PC 펜 입력 채널 source-address lock: 적용 (`UdpReceiver::EnableSourceLock`) — 세션 핸드셰이크가
  없는 상태에서 최소한의 스푸핑 방지. 첫 발신자 IP:port에 고정된다.
- PC 펜 세션 자동 리셋: 적용 (`HostController::resetPenSession`) — 고정된 발신자로부터
  3초 이상 무응답이면 자동으로 source lock 해제 + `VirtualPen::ReleaseIfDown()`으로 눌려있던
  펜 tip 해제 + sequence 추적 초기화. USB 테더링이 끊겼다 새 IP로 재연결되는 상황을
  사용자 개입 없이 스스로 복구한다 (원래 명세의 3초 Teardown을 펜 채널에 한해 구현).
- PC 영상 목적지 실시간 재지정: 적용 (`UdpSender::SetDestination`, `HostController::UpdateTabletIp`) —
  스트리밍 중에 GUI의 "Detect tethering"이 새 태블릿 주소를 찾으면 Stop/Start 없이
  바로 그 주소로 영상 전송을 이어간다. 같은 호출이 펜 세션도 즉시 리셋시켜, 수동으로
  Detect를 누르면 두 채널 다 곧바로 복구된다.
- PC v2 펜 parser: 적용
- Android v2 parser/reassembler/PLI 로깅: 적용. 단, PLI 패킷을 실제로 PC에 전송하는 제어
  채널(5002)은 아직 미구현 — 프레임 폐기 시 로그만 남기고 재전송 요청은 나가지 않음.
- Android 동적 해상도 대응: 적용 (`H264Decoder`가 `INFO_OUTPUT_FORMAT_CHANGED`로 실제
  스트림 크기를 감지해 `PenSurfaceView`의 letterbox/터치 매핑을 갱신)
- Android PC IP 설정: 앱 내 GUI(상태 텍스트 탭 → 다이얼로그)로 변경 가능, SharedPreferences에
  저장되어 재하드코딩/재빌드 불필요
- FEC, handshake, clock sync, QoS 자동 조절: 후속 Phase에서 구현
