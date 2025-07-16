#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>

// --------------------------------
// Wifi Ağı ve Telegram Bilgileri
// --------------------------------
const char *ssid = "aa";
const char *password = "12345678";
String BOTtoken = "8075037025:AAHkhy1Y2ldZ5pBxwX9cYnff0QRyWT7jubs";
String CHAT_ID = "7203548251";

// Yedek Kurtarma Şifre Sistemi
String recoveryCodes[10];
bool recoveryUsed[10] = {false};
bool recoveryCodesSent = false;

// WiFi/Offline durum bayrakları
bool wifiIsUp = true;
bool offlineModeNow = false;

// Diğer sabitler ve durumlar
#define PIR_SENSOR_PIN 14
bool sendPhoto = false;
bool cameraEnabled = true;
bool pirEnabled = false;
bool rebootRequested = false;
bool motionDetected = false;

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(BOTtoken, clientTCP);

#define FLASH_LED_PIN 4
bool flashState = LOW;

int botRequestDelay = 1000; // Her 1 saniyede yeni mesajları kontrol eder
unsigned long lastTimeBotRan;
unsigned long lastMotionTime = 0; // Son hareket zamanı
const unsigned long motionCooldown = 30000; // 30 saniye

unsigned long lastBleMotionTimes[5] = {0,0,0,0,0};
int bleMotionCount = 0;

// NTP ayarları
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3 * 3600; // Türkiye GMT+3
const int   daylightOffset_sec = 0;

// Şifre ve Güvenlik (Ana Şifre Sistemi)
#define MAX_ATTEMPTS 3
int passwordAttempt = 0;
bool isLocked = false;
String mainPassword = "";

unsigned long passwordStartTime = 0;
bool passwordActive = false;  // Şifre üretildi mi ve aktif mi
const unsigned long PASSWORD_TIMEOUT = 30000; // 30 sn

// ESP32-CAM Pinleri (AI-THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void configInitCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Kamera başlatılamadı, hata kodu: 0x%x", err);
    delay(1000);
    ESP.restart();
  }
}

int getCurrentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Saat alınamadı!");
    return -1;
  }
  return timeinfo.tm_hour;
}

void sendRecoveryCodesToTelegramAndArduino() {
  if (!recoveryCodesSent) {
    for (int i = 0; i < 10; i++) {
      int num = random(0, 10000);
      char yedek[5];
      sprintf(yedek, "%04d", num);
      recoveryCodes[i] = String(yedek);
      recoveryUsed[i] = false;
    }
    String codeList = "Kurtarma şifreleriniz (WiFi yoksa, her biri 1 kez kullanılabilir):\n";
    for (int i = 0; i < 10; i++) {
      codeList += String(i + 1) + ". " + recoveryCodes[i] + "\n";
    }
    bot.sendMessage(CHAT_ID, "PostaX - Geleceğin Akıllı Posta Sistemi\n ESP32-CAM sistemi başlatılıyor...");
    delay(300); // Telegram rate limit için bekle
    bot.sendMessage(CHAT_ID, codeList);

    // Arduino'ya kurtarma şifrelerini gönder
    String arduMsg = "RECOVERY_CODES";
    for (int i = 0; i < 10; i++) {
      arduMsg += ":" + recoveryCodes[i];
    }
    Serial1.println(arduMsg);

    recoveryCodesSent = true;
  }
}

// --------------------------------

void setup() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 3, 1); // RX=GPIO3, TX=GPIO1

  pinMode(FLASH_LED_PIN, OUTPUT);
  pinMode(PIR_SENSOR_PIN, INPUT);

  digitalWrite(FLASH_LED_PIN, flashState);
  configInitCamera();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  clientTCP.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  // Bağlantı olana kadar bekle
  while (WiFi.status() != WL_CONNECTED) delay(500);

  bot.sendMessage(CHAT_ID, "Bağlanıyor...");
  sendRecoveryCodesToTelegramAndArduino();
}

// PIR motion check
void checkMotion() {
  if (!pirEnabled) return;
  int motion = digitalRead(PIR_SENSOR_PIN);
  unsigned long currentTime = millis();
  if (motion == HIGH && currentTime - lastMotionTime > motionCooldown) {
    motionDetected = true;
    lastMotionTime = currentTime;
    Serial.println("Hareket algılandı!");
    bot.sendMessage(CHAT_ID, "Pır Sensörü hareket algılandı! Fotoğraf çekiliyor...", "");
    sendPhoto = true;
  } else if (motion == LOW) {
    motionDetected = false;
  }
}

// foto telg gönderme
String sendPhotoTelegram() {
  const char* myDomain = "api.telegram.org";
  String getAll = "";
  String getBody = "";

  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); // düşük kaliteyi atla
  fb = esp_camera_fb_get();  
  if(!fb) {
    Serial.println("Kamera yakalama hatası");
    delay(1000);
    ESP.restart();
    return "Kamera yakalama hatası";
  }  
  Serial.println(String(myDomain) + " bağlantısı kuruluyor.");

  if (clientTCP.connect(myDomain, 443)) {
    Serial.println("Bağlantı başarılı");
    String head = "--RandomNerdTutorials\r\nContent-Disposition: form-data; name=\"chat_id\"; \r\n\r\n" + CHAT_ID + "\r\n--RandomNerdTutorials\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"esp32-cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--RandomNerdTutorials--\r\n";
    size_t imageLen = fb->len;
    size_t extraLen = head.length() + tail.length();
    size_t totalLen = imageLen + extraLen;
    clientTCP.println("POST /bot"+BOTtoken+"/sendPhoto HTTP/1.1");
    clientTCP.println("Host: " + String(myDomain));
    clientTCP.println("Content-Length: " + String(totalLen));
    clientTCP.println("Content-Type: multipart/form-data; boundary=RandomNerdTutorials");
    clientTCP.println();
    clientTCP.print(head);
    uint8_t *fbBuf = fb->buf;
    size_t fbLen = fb->len;
    for (size_t n=0; n<fbLen; n+=1024) {
      if (n+1024 < fbLen) {
        clientTCP.write(fbBuf, 1024);
        fbBuf += 1024;
      }
      else if (fbLen % 1024 > 0) {
        size_t remainder = fbLen % 1024;
        clientTCP.write(fbBuf, remainder);
      }
    }  
    clientTCP.print(tail);
    esp_camera_fb_return(fb);
    int waitTime = 10000;   
    long startTimer = millis();
    boolean state = false;
    while ((startTimer + waitTime) > millis()){
      delay(100);      
      while (clientTCP.available()) {
        char c = clientTCP.read();
        if (state) getBody += String(c);        
        if (c == '\n') state = true;
        else if (c != '\r') getAll += String(c);
        startTimer = millis();
      }
      if (getBody.length() > 0) break;
    }
    clientTCP.stop();
    Serial.println(getBody);
  } else {
    getBody = "api.telegram.org'a bağlantı başarısız.";
    Serial.println("api.telegram.org'a bağlantı başarısız.");
  }
  return getBody;
}

// ---------- BLE33/Seri KOMUTLAR --- (Kurtarma kodu, Şifre/IPTAL, olaylar)
void handleSerialCommands() {
  if (Serial1.available()) {
    String gelen = Serial1.readStringUntil('\n');
    gelen.trim();

    if (gelen == "DOGRU_SIFRE") {
      int saat = getCurrentHour();
      passwordActive = false;
      String mesaj;
      if (saat >= 9 && saat < 18)
        mesaj = "Normal saatlerde giriş yapıldı. (" + String(saat) + ":00)";
      else
        mesaj = "ANORMAL SAATLERDE GİRİŞ! (" + String(saat) + ":00)";
      bot.sendMessage(CHAT_ID, mesaj, "");
    }
    if (gelen == "BLOKE_FOTO") {
    bot.sendMessage(CHAT_ID, "Sistem BLOKE OLDU! Güvenlik için fotoğraf gönderiliyor...");
    sendPhoto = true;
    }
    if (gelen == "YANLIS_SIFRE") {
      bot.sendMessage(CHAT_ID, "Yanlış şifre ile giriş denemesi yapıldı!", "");
    }
    if (gelen == "KURTARMA_SIFRE") {
      bot.sendMessage(CHAT_ID, "Kurtarma şifresiyle (WIFI offline) başarıyla giriş yapıldı!");
    }
    if (gelen == "YANLIS_KURTARMA") {
      bot.sendMessage(CHAT_ID, "Yanlış veya daha önce kullanılan kurtarma şifresi girişi!", "");
    }
    if (gelen == "RFID_OKUTULDU") {
      bot.sendMessage(CHAT_ID, "Yetkili kart okutuldu (RFID)!");
    }
    if (gelen == "YETKISIZ_RFID_OKUTULDU") {
      bot.sendMessage(CHAT_ID, "Yetkisiz kart okutuldu (RFID)!");
    }
    if (gelen == "KAPAK_ACILDI") {
      bot.sendMessage(CHAT_ID, "Kutu kapağı açıldı.");
    }
    if (gelen == "KAPAK_KAPANDI") {
      bot.sendMessage(CHAT_ID, "Kutu kapağı kapandı (kilitlendi).");
    }
    if (gelen == "HAREKET") {
      unsigned long now = millis();
      for (int i = 4; i > 0; i--) lastBleMotionTimes[i] = lastBleMotionTimes[i-1];
      lastBleMotionTimes[0] = now;
      bleMotionCount = 1;
      for (int i=1; i<5; i++) {
        if (now - lastBleMotionTimes[i] < 2000) bleMotionCount++;
      }
      static unsigned long lastBleTelegramTime = 0;
      if (now - lastBleTelegramTime > 2000) {
        bot.sendMessage(CHAT_ID, "Hareket algılandı! (BLE33/Arduino)", "");
        lastBleTelegramTime = now;
      }
      static unsigned long lastPhotoTime = 0;
      if (bleMotionCount >= 3 && now - lastPhotoTime > 5000) {
        bot.sendMessage(CHAT_ID, "3 kez hareket algılandı! Fotoğraf gönderiliyor...", "");
        sendPhoto = true;
        lastPhotoTime = now;
      }
    }
    // Şifre süresinin iptali
    if (gelen == "SIFRE_IPTAL") {
      mainPassword = "";
      passwordActive = false;
      bot.sendMessage(CHAT_ID, "Şifre süresi doldu, artık geçersiz.");
    }
    // Kurtarma kodları...
    // Gerekirse recoveryUsed dizisi vs. de güncellenebilir
  }
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Yetkisiz kullanıcı", "");
      continue;
    }
    String text = bot.messages[i].text;
    if (text == "/start") {
      String welcome = "ESP32-CAM Telegram Bot - Gelişmiş Sürüm\n\n";
      welcome += "Temel Komutlar:\n";
      welcome += "/photo : Fotoğraf çek\n";
      welcome += "/flash : Flash aç/kapat\n";
      welcome += "/enable : kamerayı etkinleştirir\n";
      welcome += "/disable : kamerayı devre dışı bırakır\n";
      welcome += "/status : Sistem durumunu gösterir\n";
      welcome += "/sifre : 4 haneli ana şifre\n";
      welcome += "/pir_on : PIR sensörü aç\n";
      welcome += "/pir_off : PIR sensörü kapat\n";
      welcome += "/unlock : Sistemi kilitten çıkar\n";
      welcome += "Gelişmiş Komutlar:\n";
      welcome += "/help : Detaylı komut açıklamaları";
      bot.sendMessage(chat_id, welcome);
    }
    if (text == "/enable") {
      cameraEnabled = true;
      bot.sendMessage(CHAT_ID, "Kamera etkinleştirildi.", "");
      Serial.println("Kamera etkinleştirildi.");
    }
    if (text == "/disable") {
      cameraEnabled = false;
      bot.sendMessage(CHAT_ID, "Kamera devre dışı bırakıldı.", "");
      Serial.println("Kamera devre dışı bırakıldı.");
    }
    if (text == "/photo") {
      sendPhoto = true;
    }
    if (text == "/flash") {
      flashState = !flashState;
      digitalWrite(FLASH_LED_PIN, flashState);
      Serial.println("Flash LED durumu değiştirildi");
    }
    if (text == "/status") {
      String statusMessage = "Sistem Durumu:\n";
      statusMessage += "WiFi Sinyali: " + String(WiFi.RSSI()) + " dBm\n";
      statusMessage += "Çalışma Süresi: " + String(millis() / 1000) + " saniye\n";
      statusMessage += "Hareket Sensörü Durumu: " + String(pirEnabled ? "Aktif" : "Pasif") + "\n";
      statusMessage += "Son Hareket: " + String((millis() - lastMotionTime) / 1000) + " saniye önce\n";
      bot.sendMessage(CHAT_ID, statusMessage, "");
    }
    if (text == "/pir_on") {
      pirEnabled = true;
      bot.sendMessage(CHAT_ID, "Hareket sensörü aktif edildi.", "");
    }
    if (text == "/pir_off") {
      pirEnabled = false;
      bot.sendMessage(CHAT_ID, "Hareket sensörü pasif edildi.", "");
    }
    if (text == "/settings") {
      String settings = "Mevcut Ayarlar:\n";
      settings += "Flash: " + String(flashState ? "Açık" : "Kapalı") + "\n";
      settings += "\nAna Şifre: " + (mainPassword.length() > 0 ? mainPassword : "Henüz üretilmedi");
      settings += "Kilitli mi: ";
      settings += (isLocked ? "EVET" : "HAYIR");
      bot.sendMessage(chat_id, settings);
    }
    if (text == "/help") {
      String help = "Detaylı Komut Açıklamaları:\n\n";
      help += "/photo - Anlık fotoğraf çeker\n";
      help += "/video - Video kaydı\n";
      help += "/flash - Flash ışığı aç/kapat\n";
      help += "/sifre - 4 haneli ana şifre üret\n";
      help += "/pir_on - Hareket algılamayı açar\n";
      help += "/pir_off - Hareket algılamayı kapatır\n";
      help += "/resolution - Çözünürlükleri listeler\n";
      help += "/setres X - Çözünürlüğü ayarla (X=0-6)\n";
      help += "/unlock - Sistemi kilitli durumdan çıkarır (admin)\n";
      help += "/settings - Mevcut tüm ayarları listeler";
      bot.sendMessage(chat_id, help);
    }
    // ANA ŞİFRE YARATMA & süresi
    if (text == "/sifre") {
      mainPassword = generatePassword();
      passwordStartTime = millis();
      passwordActive = true;
      bot.sendMessage(chat_id, "Yeni ana şifreniz: " + mainPassword + "\n30 saniye içinde girmelisiniz!");
      sendSerialStatus("PASSWORD_" + mainPassword);
    }
    // KİLİT AÇMA
    if (text == "/unlock") {
      isLocked = false;
      passwordAttempt = 0;
      bot.sendMessage(chat_id, "Güvenlik kilidi kaldırıldı.");
      Serial1.println("UNLOCK");
    }
  }
}

void sendSerialStatus(String status) {
    Serial1.println(status);
}

String generatePassword() {
  int num = random(0, 10000);
  char sifre[5];
  sprintf(sifre, "%04d", num);
  return String(sifre);
}

void loop() {
  // WiFi bağlantısı otomatik tekrar denensin:
  static unsigned long lastWifiReconnectAttempt = 0;
  const unsigned long wifiReconnectInterval = 10000; // 10 sn
  bool wifiNow = (WiFi.status() == WL_CONNECTED);
  if (!wifiNow && wifiIsUp) {
    // WiFi şu anda koptu, Arduino'ya bildir!
    offlineModeNow = true;
    sendSerialStatus("RECOVERY_ON");
  }
  if (wifiNow && !wifiIsUp) {
    // WiFi geri geldi, Arduino'ya bildir!
    offlineModeNow = false;
    sendSerialStatus("RECOVERY_OFF");
  }
  wifiIsUp = wifiNow;

  // Otomatik tekrar bağlanma
  if(WiFi.status() != WL_CONNECTED){
    unsigned long now = millis();
    if(now - lastWifiReconnectAttempt > wifiReconnectInterval){
      Serial.println("WiFi kopuk, tekrar WiFi.begin() deneniyor...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      lastWifiReconnectAttempt = now;
    }
  }

  if (rebootRequested) {
    delay(1000);
    ESP.restart();
  }

  if (sendPhoto) {
    sendPhotoTelegram();
    sendPhoto = false;
  }

  handleSerialCommands();
  checkMotion();

  if (millis() > lastTimeBotRan + botRequestDelay) {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages) {
          handleNewMessages(numNewMessages);
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
      lastTimeBotRan = millis();
  }

  // Ana şifrenin zamanını kontrol et: SÜRE DOLUNCA tüm sistemlerden silinsin.
  if (passwordActive) {
        unsigned long elapsed = millis() - passwordStartTime;
        long kalan = (PASSWORD_TIMEOUT - elapsed) / 1000;
        if (kalan < 0) kalan = 0;
        if (elapsed >= PASSWORD_TIMEOUT) {
            mainPassword = "";
            passwordActive = false;
            bot.sendMessage((bot.messages[0].chat_id != "" ? bot.messages[0].chat_id : "YOUR_TELEGRAM_CHAT_ID"), 
                "Şifre süresi dolduğu için iptal edildi.");
            sendSerialStatus("SIFRE_IPTAL");
        }
    }
}
