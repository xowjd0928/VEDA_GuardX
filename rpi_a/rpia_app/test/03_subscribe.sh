#!/bin/bash
# 03_subscribe.sh - RPi A가 발행하는 전 토픽 구독 모니터링
#
# guardx/sensor/rpia        <- 1Hz 센서 데이터 (QoS 0)
# guardx/sensor/rpia/button <- 비상 버튼 이벤트 (QoS 2)
#
# -v: 토픽명 함께 출력, -q 2: QoS2 메시지도 정확히 수신
mosquitto_sub -h localhost -p 1883 -t 'guardx/#' -v -q 2
