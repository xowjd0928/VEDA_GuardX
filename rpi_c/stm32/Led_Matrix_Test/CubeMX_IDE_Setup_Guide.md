# STM32CubeMX / CubeIDE 설정 가이드 라인

이 문서는 `Led_Matrix_Test` 프로젝트를 처음부터 재현하거나, `.ioc`를 다시 열어 재생성(regenerate)할 때
반드시 지켜야 하는 설정값을 정리한 것입니다. 특히 **GPIO 속도 설정은 과거 실제 장애 원인**이었으므로
재생성 시 가장 먼저 확인해야 합니다.

---

## 1. 프로젝트 개요

| 항목 | 값 |
|---|---|
| MCU | STM32F401RETx (Nucleo-F401RE) |
| CubeMX 버전 | 6.18.0 |
| Firmware Package | STM32Cube FW_F4 V1.28.3 |
| Toolchain | STM32CubeIDE |
| Heap Size | 0x200 |
| Stack Size | 0x400 |

`.ioc` 재생성 시 `ProjectManager.KeepUserCode=true`로 되어 있어 `/* USER CODE BEGIN ... END */`
블록 안의 코드(HUB75 드라이버 호출, 대시보드 그리기 로직 등)는 보존됩니다. **다만 USER CODE 블록
바깥에 실수로 코드를 넣으면 재생성 시 통째로 날아가므로, 항상 BEGIN/END 안에서만 작성할 것.**

---

## 2. 클럭 설정 (RCC)

| 항목 | 값 |
|---|---|
| Oscillator | HSI (내부 16MHz) |
| PLL Source | HSI |
| PLLM / PLLN / PLLP / PLLQ | 16 / 336 / DIV4 / 7 |
| SYSCLK Source | PLLCLK |
| SYSCLK | 84 MHz |
| AHB (HCLK) | 84 MHz |
| APB1 | 42 MHz (Timer 클럭은 x2 = 84MHz) |
| APB2 | 84 MHz (Timer 클럭도 84MHz) |
| Voltage Scale | Scale 2 |
| Flash Latency | 2 |

HSE(외부 크리스탈)는 쓰지 않고 HSI만 사용합니다. Nucleo 보드에 별도 외부 크리스탈을 안 쓰는 구성이라
재현 시 HSE를 켤 필요는 없습니다.

---

## 3. GPIO 핀맵 (HUB75 + USART2)

### 3.1 HUB75 데이터/제어 핀 — 전부 `GPIO_SPEED_FREQ_VERY_HIGH` 고정

> ⚠️ **여기가 과거 "출력이 아예 안 됨" 장애의 원인이었던 부분입니다.**
> HUB75는 CLK을 수백 kHz~MHz로 토글하는데, 기본값(LOW/MEDIUM)으로 두면 슬루레이트가 느려서
> 신호가 제때 안정되지 않고 패널이 아예 반응하지 않거나 노이즈가 낍니다.
> **CubeMX에서 아래 핀들을 다시 잡을 때는 Speed를 반드시 Very High로 설정할 것.**

| 라벨 | 핀 | 포트/핀 | 역할 | Mode | Speed | Pull |
|---|---|---|---|---|---|---|
| CLK | CLK_Pin | PA5 | 시프트 클럭 | Output PP | **Very High** | No pull |
| LAT | LAT_Pin | PB9 | 래치 | Output PP | **Very High** | No pull |
| OE  | OE_Pin  | PB8 | Output Enable(밝기 제어) | Output PP | **Very High** | No pull |
| A | A_Pin | PA9 | 스캔 어드레스 | Output PP | **Very High** | No pull |
| B | B_Pin | PC7 | 스캔 어드레스 | Output PP | **Very High** | No pull |
| C | C_Pin | PB6 | 스캔 어드레스 | Output PP | **Very High** | No pull |
| D | D_Pin | PA7 | 스캔 어드레스 | Output PP | **Very High** | No pull |
| E | E_Pin | PA6 | 스캔 어드레스 (1/16 scan에 필요) | Output PP | **Very High** | No pull |
| R1 | R1_Pin | PA10 | 위쪽 절반 Red | Output PP | **Very High** | No pull |
| G1 | G1_Pin | PB3  | 위쪽 절반 Green | Output PP | **Very High** | No pull |
| B1 | B1_Pin | PB5  | 위쪽 절반 Blue | Output PP | **Very High** | No pull |
| R2 | R2_Pin | PB4  | 아래쪽 절반 Red | Output PP | **Very High** | No pull |
| G2 | G2_Pin | PB10 | 아래쪽 절반 Green | Output PP | **Very High** | No pull |
| B2 | B2_Pin | PA8  | 아래쪽 절반 Blue | Output PP | **Very High** | No pull |

총 14핀. GPIOA(6핀), GPIOB(6핀), GPIOC(1핀: B) + PA9(A)로 나뉘어 있습니다.
드라이버(`Driver_RGBMatrix.c`)는 `GPIOx->BSRR`을 포트 단위로 한 번에 쓰는 방식이라
**핀을 다른 포트로 옮기면 `HUB75_DATA_PORTA_MASK` / `HUB75_DATA_PORTB_MASK` 등의 비트마스크 매크로도
같이 고쳐야 합니다.** CubeMX에서 핀 배치만 바꾸고 코드는 안 고치면 조용히 오동작합니다.

### 3.2 USART2 (디버그/향후 RPiC 통신용)

| 라벨 | 핀 | 역할 | Mode | Speed | Pull |
|---|---|---|---|---|---|
| USART_TX | PA2 | USART2_TX (AF7) | AF_PP | Low | No pull |
| USART_RX | PA3 | USART2_RX (AF7) | AF_PP | Low | No pull |

Baudrate 115200, 8N1, Flow control 없음, TX/RX 모드. Nucleo 보드의 ST-Link 가상 COM 포트와
그대로 연결되는 핀이라 별도 배선 없이 PC에서 시리얼 터미널로 확인 가능합니다.

> 참고: RPiC ↔ STM32 통신 구조를 붙이게 되면 이 USART2를 그대로 쓰는 게 자연스럽습니다.
> 다만 그때는 반드시 DMA + IDLE 라인 인터럽트 방식으로 바꿔서 수신이 메인 루프(HUB75 스캔)를
> 블로킹하지 않도록 해야 합니다. 지금은 초기화만 되어 있고 실제 송수신 코드는 없는 상태입니다.

---

## 4. 타이머 설정

### 4.1 TIM1 — HUB75 리프레시 타이밍용 (핵심)

| 항목 | 값 |
|---|---|
| Clock Source | Internal |
| Prescaler | 582 |
| Counter Period (ARR) | 4 (CubeMX 초기값 — 런타임에 덮어씀) |
| Clock Division | Div1 |
| Auto-reload preload | Enable |
| 인터럽트 | TIM1 Update (TIM1_UP_TIM10_IRQn), Priority 0,0 (최우선) |

APB2 Timer 클럭 84MHz 기준, Prescaler 582 → 약 144kHz 카운트 클럭.
실제 ARR 값은 `HUB75_SetRefreshRate()`가 `main()`에서 `HUB75_Init()` 직후 런타임에 다시 설정합니다
(`HUB75_SetRefreshRate(1)` → ARR=8). **CubeMX의 Period=4는 그냥 초기 placeholder이고,
실제 리프레시 속도를 바꾸려면 `.ioc`가 아니라 `main.c`의 `HUB75_SetRefreshRate()` 인자를 바꿔야 합니다.**

인터럽트 우선순위가 0,0(최고)인 이유: 이 인터럽트가 늦게 들어오면 `display_tick`이 밀려서
스캔 타이밍이 흔들리고 화면이 깜빡입니다. **다른 인터럽트(향후 USART DMA 등)를 추가할 때
이보다 낮은(숫자가 큰) 우선순위를 줘야 합니다.**

### 4.2 TIM4 — 현재 미사용, 초기화만 되어 있음

| 항목 | 값 |
|---|---|
| Clock Source | Internal |
| Prescaler | 83 |
| Period | 99 |
| 인터럽트 | 없음 (Start도 안 됨) |

`MX_TIM4_Init()`은 호출되지만 코드 어디에서도 `HAL_TIM_Base_Start()`가 없어 **실제로는 카운트가
시작되지 않는 죽은 설정**입니다. CubeMX 초기 템플릿에서 남은 것으로 보이며, 지금 당장 문제는
없지만 나중에 정리하거나 다른 용도(예: 센서 폴링 주기, PWM)로 재활용할 수 있습니다.

---

## 5. 빌드 설정 (CubeIDE .cproject)

| Configuration | Optimization | Debug Level |
|---|---|---|
| Debug | 기본값(사실상 -O0) | -g3 |
| Release | **-Os** (크기 최적화) | -g0 |

HUB75 스캔 루프처럼 타이밍이 민감한 코드는 최적화 레벨에 따라 체감 속도가 달라질 수 있습니다.
노이즈/타이밍 문제를 디버깅할 때는 **Debug 빌드(-O0)와 Release 빌드(-Os) 양쪽에서 실제로
패널에 띄워보고 비교**하는 걸 권장합니다 — 최적화가 타이밍을 앞당겨서 오히려 문제가 사라지거나
반대로 드러나는 경우가 있습니다.

---

## 6. `.ioc` 재생성 시 체크리스트

1. **HUB75 관련 14핀의 Speed가 전부 Very High인지 재확인** (가장 흔한 실수 지점)
2. GPIO 핀 배치를 바꿨다면 `Driver_RGBMatrix.c`의 포트 마스크 매크로(`HUB75_DATA_PORTA_MASK` 등)도 같이 수정
3. TIM1 인터럽트 우선순위가 여전히 최우선(0,0)인지 확인
4. Generate Code 이후 `main.c`, `gpio.c`, `tim.c`의 USER CODE 블록이 그대로 남아있는지 diff로 확인
5. Release 빌드로도 한 번 플래시해서 노이즈/타이밍이 Debug 빌드와 다르지 않은지 확인
