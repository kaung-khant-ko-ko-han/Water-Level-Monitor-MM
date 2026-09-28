1. Arduino IDE → Board: ESP32 Dev Module → Port ရွေး → Upload
2. Serial Monitor: 115200 baud
3. First boot → AP "WM-SETUP-XXXXXX" ပေါ်မယ်
4. Phone နဲ့ AP ချိတ် → http://192.168.4.1 → WiFi + MQTT ဖြည့်
5. Save → Reboot → Normal mode
6. MQTT client (MQTTX / HA) နဲ့ subscribe:
       wm/v1/WM-XXXXXX/telemetry
       wm/v1/WM-XXXXXX/availability