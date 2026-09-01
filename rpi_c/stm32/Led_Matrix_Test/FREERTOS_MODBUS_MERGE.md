# FreeRTOS + HUB75 + Modbus 통합 메모

`RTOS_1/Led_Matrix_Test`의 FreeRTOS 뼈대를 현재 `Led_Matrix_Test`에 병합했다.
원본 `RTOS_1`은 비교용으로 그대로 보존한다.

## 실행 구조

| 실행 주체 | 역할 | 우선순위/주기 |
|---|---|---|
| TIM1 ISR | HUB75 스캔 스텝 적립. RTOS API는 호출하지 않음 | IRQ 0 |
| Render 태스크 | 적립된 HUB75 스캔 처리 및 화면 다시 그리기 | AboveNormal, 1 ms |
| UI 태스크 | Modbus 센서·화재·화면 레지스터를 표시 상태로 반영 | Normal, 100 ms |
| Modbus 태스크 | UART 큐에서 프레임 수신, CRC/명령 처리, 응답 송신 | Normal |
| USART6/DMA ISR | 수신 프레임을 RTOS 큐에 복사하고 즉시 복귀 | IRQ 5 |
| TIM2 ISR | HAL 1 ms 시간 기준 | IRQ 15 |

## Modbus와 화면 연결

- 100~107: Zone 1~4 온도(x10), 습도(%RH)
- 120: 화재 Zone bitmap. bit0~3이 Zone 1~4이며 화재 화면이 다른 화면보다 우선한다.
- 130: 화면 선택. `0=화면1/2 자동 순환`, `1=화면1`, `2=화면2`, `3=화면3`
- 1: HUB75 밝기(0~255)에 즉시 반영
- 2: HUB75 주사율 단계(1~4)에 즉시 반영

## 빌드 확인

- STM32CubeIDE 2.2.0 Debug: 0 errors
- STM32CubeIDE 2.2.0 Release: 0 errors
- Debug 크기: text 37,976 / data 344 / bss 28,936 bytes
- Release 크기: text 21,432 / data 344 / bss 28,920 bytes

`User/image/image.c`의 기존 배열 초기화 중괄호 경고 1개는 병합 전 코드에서 온 것이며 실행 파일 생성에는 영향이 없다.
