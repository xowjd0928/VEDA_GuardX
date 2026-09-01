// guardx_mqttd 진입점. src/main.cpp(guardx_poller)와 같은 꼴로 둔다 —
// 본체는 src/MqttDb/mqtt_service.cpp.
//
// 데몬화는 하지 않는다 (fork 없음). systemd Type=simple 로 foreground
// 실행하고, stdout/stderr 는 journald 가 그대로 받는다. mqttInit 이
// loop_start 로 네트워크 스레드를 띄우기 때문에, 그 뒤에 fork 하면
// 자식에는 그 스레드가 없어 구독도 발행도 조용히 멎는다.
int runMqttService();
int main() { return runMqttService(); }
