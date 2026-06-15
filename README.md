# 🔥 SmokeBot — ESP32 Gas & Smoke Detection

ระบบตรวจจับแก๊สและควันด้วย ESP32 แจ้งเตือนผ่าน Telegram Bot

## Features
- 🔄 Self-calibration อัตโนมัติ 5-15 นาที
- 📊 Auto-recalibration ทุก 1 ชั่วโมง
- 🚨 แจ้งเตือน Telegram แบบ Real-time
- 🔇 Remote mute/unmute ผ่าน Bot
- 🛠️ Custom PCB ออกแบบและผลิตเอง

## Hardware
- ESP32 DevKit V1
- MQ-2 Gas Sensor
- LED x3 (Green/Red/Yellow)
- Active Buzzer

## Commands
| Command | Description |
|---------|-------------|
| /status | ดูค่าแก๊สปัจจุบัน |
| /mute | ปิดเสียง Buzzer |
| /unmute | เปิดเสียง Buzzer |
| /recalibrate | ปรับค่าเซนเซอร์ใหม่ |

## PCB
ออกแบบด้วย EasyEDA และผลิตด้วยวิธี Toner Transfer + Ferric Chloride Etching
