#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// --- Wi-Fi & Telegram ---
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
#define BOTtoken "YOUR_BOT_TOKEN"
#define CHAT_ID  "YOUR_CHAT_ID"

// --- Pins ---
#define MQ2_PIN    34
#define LED_GREEN  25
#define LED_RED    26
#define LED_YELLOW 27
#define BUZZER_PIN 33

// --- Calibration (ใหม่) ---
const int   STABLE_WINDOW       = 30;    // ตัวอย่างสุดท้าย 30 ค่า (~30 วินาที) ต้องนิ่ง
const int   STABLE_TOLERANCE    = 30;    // ค่าต่างสูงสุดใน window ที่ถือว่านิ่ง
const int   MIN_WARMUP_SAMPLES  = 300;   // อย่างน้อย 5 นาที (300 วิ) ก่อนตรวจความนิ่ง
const int   MAX_CALIB_SAMPLES   = 900;   // หยุดสูงสุดที่ 15 นาทีไม่ว่ายังไง
const int   CALIBRATION_OFFSET  = 400;
const int   THRESHOLD_HYSTERESIS = 150;
const int   BASELINE_DRIFT_TOLERANCE = 200;
const unsigned long AUTO_CALIB_INTERVAL = 3600000UL; // 1 ชั่วโมง

// --- State ---
bool          isDanger          = false;
bool          isMuted           = false;
int           thresholdHigh     = 1200;
int           thresholdLow      = 1050;
int           lastBaseline      = 0;  // เก็บ baseline ล่าสุดไว้เปรียบเทียบ
unsigned long dangerStartTime   = 0;
unsigned long lastBotCheck      = 0;
unsigned long lastAutoCalibTime = 0;  // จับเวลาคาลิเบรตอัตโนมัติ
const int     BOT_INTERVAL      = 1000;

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// =============================
// ฟังก์ชันช่วย
// =============================
String getDangerLevel(int value) {
  if (value > thresholdHigh * 2.5) return "⛔ วิกฤต";
  if (value > thresholdHigh * 1.7) return "🔴 สูงมาก";
  if (value > thresholdHigh * 1.2) return "🟠 สูง";
  return "🟡 ปานกลาง";
}

int toPercent(int value) {
  return constrain(map(value, 0, 4095, 0, 100), 0, 100);
}

void setLED(bool green, bool red, bool yellow) {
  ledcWrite(LED_GREEN,  green  ? 255 : 0);
  ledcWrite(LED_RED,    red    ? 255 : 0);
  ledcWrite(LED_YELLOW, yellow ? 255 : 0);
}

void buzzAlert(bool on) {
  if (on && !isMuted) {
    ledcAttach(BUZZER_PIN, 1000, 8);
    ledcWrite(BUZZER_PIN, 128); // duty 50%
  } else {
    ledcWrite(BUZZER_PIN, 0);
  }
}

// =============================
// 1. Self-Calibration
// =============================
void runCalibration() {
  Serial.println(">>> Starting Self-Calibration...");
  setLED(false, false, true);
  bot.sendMessage(CHAT_ID, "🔄 เริ่มปรับค่าเซนเซอร์ กำลังรอ Warm-up...", "");

  int  window[STABLE_WINDOW] = {};
  int  windowIdx   = 0;
  bool windowFull  = false;
  long runningSum  = 0;
  int  sampleCount = 0;
  int  baseline    = 0;

  while (sampleCount < MAX_CALIB_SAMPLES) {
    int val = analogRead(MQ2_PIN);
    
    // แจ้งความคืบหน้าทุก 5 นาที
    if (sampleCount == 300)
      bot.sendMessage(CHAT_ID, "⏳ Warm-up ผ่านไป 5 นาที...", "");
    if (sampleCount == 600)
      bot.sendMessage(CHAT_ID, "⏳ Warm-up ผ่านไป 10 นาที...", "");

    // อัปเดต sliding window
    runningSum -= window[windowIdx];
    window[windowIdx] = val;
    runningSum += val;
    windowIdx = (windowIdx + 1) % STABLE_WINDOW;
    sampleCount++;                          // ย้ายมาหลัง update index
    if (sampleCount >= STABLE_WINDOW)       // window เต็มจริงเมื่อเก็บครบ 30 ค่า
    windowFull = true;

    // ตรวจความนิ่งหลังผ่าน warm-up ขั้นต่ำ
    if (windowFull && sampleCount >= MIN_WARMUP_SAMPLES) {
      int minVal = window[0], maxVal = window[0];
      for (int j = 1; j < STABLE_WINDOW; j++) {
        minVal = min(minVal, window[j]);
        maxVal = max(maxVal, window[j]);
      }
      int spread = maxVal - minVal;

      Serial.printf("Sample %d | val=%d | spread=%d\n", sampleCount, val, spread);

      if (spread <= STABLE_TOLERANCE) {
        // ค่านิ่งแล้ว — ใช้ค่าเฉลี่ยใน window เป็น baseline
        baseline = runningSum / STABLE_WINDOW;
        Serial.printf("Stable! baseline=%d after %d samples\n", baseline, sampleCount);
        break;
      }
    }

    delay(1000); // 1 วินาที/sample
  }

  // ถ้าหมดเวลาแล้วยังไม่นิ่ง ใช้ค่าเฉลี่ย window สุดท้ายไปก่อน
  if (baseline == 0) {
    baseline = windowFull ? (runningSum / STABLE_WINDOW) : (runningSum / sampleCount);
    bot.sendMessage(CHAT_ID, "⚠️ หมดเวลา Warm-up แต่ค่ายังไม่นิ่งมาก ใช้ค่าประมาณแทน", "");
  }

  lastBaseline  = baseline;
  thresholdHigh = baseline + CALIBRATION_OFFSET;
  thresholdLow  = thresholdHigh - THRESHOLD_HYSTERESIS;

  String msg = "🔧 ปรับค่าเสร็จสิ้น\n";
  msg += "━━━━━━━━━━━━━━━━━━━━\n";
  msg += "📐 Baseline: " + String(baseline) + "\n";
  msg += "🎯 Threshold: " + String(thresholdHigh) + "\n";
  msg += "✅ ระบบพร้อมทำงาน";
  bot.sendMessage(CHAT_ID, msg, "");
}

// =============================
// 2. Conditional Auto-Recalibration
//    เรียกใน loop() ทุก 1 ชั่วโมง
// =============================
void tryAutoRecalibrate() {
  // เงื่อนไข 1: ต้องไม่อยู่ในสถานะอันตราย
  if (isDanger) {
    Serial.println("Auto-Calib skipped: danger active");
    String msg = "⏭️ ข้ามการปรับค่าอัตโนมัติ (ครบ 1 ชั่วโมง)\n";
    msg += "━━━━━━━━━━━━━━━━━━━━\n";
    msg += "❌ เหตุผล: ขณะนี้อยู่ในสถานะอันตราย\n";
    msg += "🔁 จะลองใหม่อีกครั้งใน 1 ชั่วโมง";
    bot.sendMessage(CHAT_ID, msg, "");
    return;
  }

  // เงื่อนไข 2: ค่าแก๊สตอนนี้ต้องใกล้เคียง baseline เดิม
  int currentGas = analogRead(MQ2_PIN);
  int drift      = abs(currentGas - lastBaseline);

  if (drift > BASELINE_DRIFT_TOLERANCE) {
    Serial.printf("Auto-Calib skipped: drift=%d exceeds tolerance=%d\n", drift, BASELINE_DRIFT_TOLERANCE);
    String msg = "⏭️ ข้ามการปรับค่าอัตโนมัติ (ครบ 1 ชั่วโมง)\n";
    msg += "━━━━━━━━━━━━━━━━━━━━\n";
    msg += "❌ เหตุผล: ค่าแก๊สเบี่ยงเบนจาก Baseline มากเกินไป\n";
    msg += "📊 ค่าปัจจุบัน: " + String(currentGas) + " | Baseline เดิม: " + String(lastBaseline) + "\n";
    msg += "📏 ส่วนต่าง: " + String(drift) + " (เกินเกณฑ์ " + String(BASELINE_DRIFT_TOLERANCE) + ")\n";
    msg += "🔁 จะลองใหม่อีกครั้งใน 1 ชั่วโมง";
    bot.sendMessage(CHAT_ID, msg, "");
    return;
  }

  // ผ่านทุกเงื่อนไข — คาลิเบรตได้เลย
  Serial.println("Auto-Calib: conditions met, recalibrating...");
  bot.sendMessage(CHAT_ID, "🔄 เริ่มปรับค่าเซนเซอร์อัตโนมัติ (ครบ 1 ชั่วโมง + ผ่านเงื่อนไขแล้ว)...", "");
  runCalibration();
}

// =============================
// 3. รับคำสั่งจาก Telegram
// =============================
void handleCommands() {
  int numMsg = bot.getUpdates(bot.last_message_received + 1);
  while (numMsg) {
    for (int i = 0; i < numMsg; i++) {
      String text    = bot.messages[i].text;
      String from    = bot.messages[i].from_name;
      String chat_id = bot.messages[i].chat_id;

      Serial.println("Command: " + text);

      if (text == "/help") {
        String help = "";
        help += "📋 คำสั่งที่รองรับทั้งหมด\n";
        help += "━━━━━━━━━━━━━━━━━━━━\n";
        help += "  /status       — ดูค่าแก๊สปัจจุบัน\n";
        help += "  /mute         — ปิดเสียง Buzzer\n";
        help += "  /unmute       — เปิดเสียง Buzzer\n";
        help += "  /recalibrate  — ปรับค่าเซนเซอร์ใหม่\n";
        help += "  /resetwifi    — เปลี่ยนการตั้งค่า Wi-Fi\n";
        help += "  /help         — ดูคำสั่งทั้งหมด";
        bot.sendMessage(chat_id, help, "");
      }

      else if (text == "/status") {
        int gasValue = analogRead(MQ2_PIN);
        // คำนวณเวลาจนกว่าจะคาลิเบรตรอบถัดไป
        unsigned long elapsed     = millis() - lastAutoCalibTime;
        unsigned long remaining   = (elapsed < AUTO_CALIB_INTERVAL)
                                    ? (AUTO_CALIB_INTERVAL - elapsed) / 1000
                                    : 0;
        String nextCalib = remaining > 0
          ? (String(remaining / 60) + " นาที " + String(remaining % 60) + " วินาที")
          : "ถึงเวลาแล้ว (รอรอบ loop ถัดไป)";

        String reply = "";
        reply += "📊 สถานะปัจจุบัน\n";
        reply += "━━━━━━━━━━━━━━━━━━━━\n";
        reply += "🌡️ ค่าแก๊ส/ควัน: " + String(gasValue) + " (" + String(toPercent(gasValue)) + "%)\n";
        reply += "🎯 เกณฑ์แจ้งเตือน: " + String(thresholdHigh) + "\n";
        reply += "📐 Baseline ล่าสุด: " + String(lastBaseline) + "\n";
        reply += "⏱️ คาลิเบรตอัตโนมัติถัดไปใน: " + nextCalib + "\n";
        reply += "🔔 สถานะ: " + String(isDanger ? "⚠️ อันตราย" : "✅ ปกติ") + "\n";
        reply += "🔇 เสียง: " + String(isMuted ? "ปิดอยู่ (Muted)" : "เปิดอยู่") + "\n";
        reply += "━━━━━━━━━━━━━━━━━━━━\n";
        reply += "💡 คำสั่งที่ใช้ได้: /status /mute /unmute /recalibrate /resetwifi";
        bot.sendMessage(chat_id, reply, "");
      }

      else if (text == "/mute") {
        isMuted = true;
        noTone(BUZZER_PIN);
        bot.sendMessage(chat_id, "🔇 ปิดเสียงแจ้งเตือน (Mute) แล้ว\nพิมพ์ /unmute เพื่อเปิดเสียงอีกครั้ง", "");
      }

      else if (text == "/unmute") {
        isMuted = false;
        bot.sendMessage(chat_id, "🔔 เปิดเสียงแจ้งเตือนแล้ว", "");
      }

      else if (text == "/recalibrate") {
        bot.sendMessage(chat_id, "🔄 กำลังปรับค่าเซนเซอร์ใหม่ กรุณารอ 5 วินาที...", "");
        runCalibration();
        lastAutoCalibTime = millis(); // รีเซ็ต timer เพื่อไม่ให้คาลิเบรตซ้ำเร็วเกินไป
      }

      else if (text == "/resetwifi") {
        bot.sendMessage(chat_id, "🔄 กำลังรีเซ็ต Wi-Fi...\nกรุณาเชื่อมต่อ Wi-Fi ชื่อ SmokeBot-Setup (รหัส: 12345678)", "");
        delay(2000);
        WiFiManager wifiManager;
        wifiManager.resetSettings();
        ESP.restart();
      }

      else {
        String help = "";
        help += "❓ ไม่รู้จักคำสั่งนี้\n";
        help += "━━━━━━━━━━━━━━━━━━━━\n";
        help += "📋 คำสั่งที่รองรับ:\n";
        help += "  /status       — ดูค่าแก๊สปัจจุบัน\n";
        help += "  /mute         — ปิดเสียง Buzzer\n";
        help += "  /unmute       — เปิดเสียง Buzzer\n";
        help += "  /recalibrate  — ปรับค่าเซนเซอร์ใหม่\n";
        help += "  /resetwifi    — เปลี่ยนการตั้งค่า Wi-Fi";
        bot.sendMessage(chat_id, help, "");
      }
    }
    numMsg = bot.getUpdates(bot.last_message_received + 1);
  }
}

void setup() {
  Serial.begin(115200);

  ledcAttach(LED_GREEN,  5000, 8);
  ledcAttach(LED_RED,    5000, 8);
  ledcAttach(LED_YELLOW, 5000, 8);

  setLED(false, false, true);

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);

  if (!wifiManager.autoConnect("SmokeBot-Setup", "12345678")) {
    Serial.println("Wi-Fi Failed! Rebooting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Wi-Fi Connected: " + WiFi.SSID());
  client.setInsecure();

  bot.sendMessage(CHAT_ID, "📡 เชื่อมต่อ Wi-Fi สำเร็จ กำลังปรับค่าเซนเซอร์... ใช้เวลา 5–15 นาที", "");
  runCalibration();   // ← แทนลูปทั้งหมดที่เคยเขียนซ้ำไว้ใน setup()
  lastAutoCalibTime = millis();
  setLED(true, false, false);

  String bootMsg = "";
  bootMsg += "✅ ระบบตรวจจับแก๊สและควัน พร้อมใช้งาน\n";
  bootMsg += "━━━━━━━━━━━━━━━━━━━━\n";
  bootMsg += "📡 สถานะ: ออนไลน์และตรวจสอบต่อเนื่อง\n";
  bootMsg += "⚠️ เกณฑ์แจ้งเตือน: " + String(thresholdHigh) + " (Auto-Calibrated)\n";
  bootMsg += "━━━━━━━━━━━━━━━━━━━━\n";
  bootMsg += "💡 พิมพ์ /status เพื่อดูค่าปัจจุบันได้เลย\n";
  bootMsg += "💡 พิมพ์ /help เพื่อดูคำสั่งทั้งหมด";
  bot.sendMessage(CHAT_ID, bootMsg, "");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setLED(false, false, true);
    buzzAlert(isDanger);
    Serial.println("Warning: Wi-Fi Disconnected!");
    delay(1000);
    return;
  }

  if (millis() - lastBotCheck > BOT_INTERVAL) {
    lastBotCheck = millis();
    handleCommands();
  }

  // --- Conditional Auto-Recalibration ทุก 1 ชั่วโมง ---
  if (millis() - lastAutoCalibTime >= AUTO_CALIB_INTERVAL) {
    lastAutoCalibTime = millis(); // รีเซ็ต timer ก่อนเสมอ ไม่ว่าจะผ่านเงื่อนไขหรือเปล่า
    tryAutoRecalibrate();
  }

  int gasValue   = analogRead(MQ2_PIN);
  int gasPercent = toPercent(gasValue);
  Serial.printf("Gas: %d (%d%%) | Threshold: %d | Baseline: %d\n",
                gasValue, gasPercent, thresholdHigh, lastBaseline);

  if (gasValue > thresholdHigh) {
    setLED(false, true, false);
    buzzAlert(true);

    if (!isDanger) {
      isDanger = true;
      dangerStartTime = millis();

      String msg = "";
      msg += "🚨 แจ้งเตือนฉุกเฉิน: ตรวจพบแก๊ส/ควัน\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "⚠️ ระดับอันตราย: " + getDangerLevel(gasValue) + "\n";
      msg += "📊 ค่าที่ตรวจวัด: " + String(gasValue) + " (" + String(gasPercent) + "%)\n";
      msg += "🎯 เกณฑ์แจ้งเตือน: " + String(thresholdHigh) + "\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "🔴 คำแนะนำ:\n";
      msg += "  • เปิดประตูหน้าต่างระบายอากาศทันที\n";
      msg += "  • ปิดแหล่งก๊าซหรืออุปกรณ์ที่อาจเป็นต้นเหตุ\n";
      msg += "  • ออกจากพื้นที่หากค่ายังคงสูงต่อเนื่อง\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "💡 พิมพ์ /mute เพื่อปิดเสียงหลังเข้าแก้ไขแล้ว";
      bot.sendMessage(CHAT_ID, msg, "");
    }
    delay(500);
  }

  else if (gasValue < thresholdLow) {
    setLED(true, false, false);
    buzzAlert(false);

    if (isDanger) {
      isDanger = false;
      isMuted  = false;

      unsigned long duration = (millis() - dangerStartTime) / 1000;
      String durationStr = duration >= 60
        ? String(duration / 60) + " นาที " + String(duration % 60) + " วินาที"
        : String(duration) + " วินาที";

      String msg = "";
      msg += "✅ สถานการณ์กลับสู่ภาวะปกติ\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "📊 ค่าปัจจุบัน: " + String(gasValue) + " (" + String(gasPercent) + "%)\n";
      msg += "⏱️ ระยะเวลาที่เกิดเหตุ: " + durationStr + "\n";
      msg += "🔔 เสียงแจ้งเตือน: รีเซ็ตแล้ว\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "🟢 ระบบกลับสู่การตรวจสอบปกติ";
      bot.sendMessage(CHAT_ID, msg, "");
    }
  }

  delay(1000);
}