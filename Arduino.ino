#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <Arduino_BMI270_BMM150.h>

// Servo ve Selenoid
Servo myServo;
const int servoPin = 6;       // D6
const int selenoidPin = 8;    // D8

// I2C LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// RFID
#define SS_PIN    10
#define RST_PIN   9
MFRC522 rfid(SS_PIN, RST_PIN);
byte authorizedUID[4] = {0x14, 0x07, 0xD7, 0xA7}; // Yetkili kart

// Tuş Takımı
const byte ROWS = 4, COLS = 3;
char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', ' '}
};

byte rowPins[ROWS] = {D7, A0, D2, D4}; 
byte colPins[COLS] = {D5, A1, D3};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Buzzer
const int buzzerPin = A6;

// Şifre değişkenleri
String PASSWORD = "";      // Geçerli şifre (ESP32'den gelir)
String enteredPassword = "";   // Girilen şifre
bool passwordMode = false;     // Şifre giriş aktiflik durumu

// Kurtarma şifresi ve offline mod sistemi
String recoveryCodes[10];
bool recoveryUsed[10] = {false};
bool recoveryReady = false;
bool offlineMode = false;

// Sistem kilidi ve sayaçlar
int wrongPasswordCount = 0, wrongRFIDCount = 0;
const int maxWrongTries = 3;
const int maxRFIDTries = 3;
bool systemLocked = false;

// Hareket sensörü değişkenleri (IMU)
unsigned long lastMotionTime = 0;
int motionCount = 0;
const unsigned long motionInterval = 2000;

// Geri sayım
bool countdownActive = false;
unsigned long countdownStartMillis = 0;
const int countdownSeconds = 30;

// === SETUP ===
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);     // ESP32-CAM ile UART

  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sifre Girin:");
  lcd.setCursor(0, 1); lcd.print(">");

  // IMU başlat
  if (!IMU.begin()) {
    Serial.println("IMU baslatilamadi!");
    while (1);
  }
  Serial.println("IMU hazir!");

  pinMode(D7, INPUT_PULLUP); pinMode(A0, INPUT_PULLUP); pinMode(D2, INPUT_PULLUP); pinMode(D4, INPUT_PULLUP);
  pinMode(D5, INPUT_PULLUP); pinMode(A1, INPUT_PULLUP); pinMode(D3, INPUT_PULLUP);

  pinMode(selenoidPin, OUTPUT); digitalWrite(selenoidPin, LOW);
  myServo.attach(servoPin); myServo.write(0);
  pinMode(buzzerPin, OUTPUT);

  SPI.begin(); rfid.PCD_Init();
  Serial.println("RFID Modulü Hazir!");
  passwordMode = true;
}

// === DESTEK FONKSIYONLAR ===
void threatTone() {
  for (int freq = 1500; freq >= 800; freq -= 50) {
    tone(buzzerPin, freq, 30);
    delay(40);
  }
  delay(100);
  for (int i = 0; i < 3; i++) {
    tone(buzzerPin, 1200, 100);
    delay(150);
    noTone(buzzerPin);
    delay(100);
  }
}

void shortBeep() {
  tone(buzzerPin, 1000, 100);
  delay(120);
}

void resetPassword() {
  enteredPassword = "";
  if (!systemLocked) {
    lcd.setCursor(0, 0); lcd.print("Sifre Girin:    ");
    lcd.setCursor(0, 1); lcd.print(">              ");
    passwordMode = true;
  } else {
    lcd.setCursor(0, 0); lcd.print("Sistem Kilitlendi");
    lcd.setCursor(0, 1); lcd.print("                ");
    passwordMode = false;
  }
}

// === ASIL KONTROL FONKSIYONU ===
void checkPassword() {
  // Sistem kilitliyse hiçbir işleme izin verme
  if (systemLocked) {
    lcd.setCursor(0, 0); lcd.print("Sistem Kilitlendi");
    lcd.setCursor(0, 1); lcd.print("                ");
    tone(buzzerPin, 200, 400); delay(1200);
    resetPassword(); return;
  }
  // Sadece wifi varken ana şifre!
  if (!offlineMode && passwordMode) {
    if (PASSWORD == "" || enteredPassword == "") {
      lcd.setCursor(0,0); lcd.print("Gecerli Sifre Yok");
      lcd.setCursor(0,1); lcd.print("                ");
      delay(1000); resetPassword(); return;
    }
    if (enteredPassword == PASSWORD) {
      // Doğru şifre
      lcd.setCursor(0, 1); lcd.print("Sifre Dogru!     ");
      wrongPasswordCount = 0; countdownActive = false;
      lcd.setCursor(0, 1); lcd.print("                ");
      digitalWrite(selenoidPin, HIGH);
      delay(150);
      tone(buzzerPin, 1500, 200); delay(200);
      tone(buzzerPin, 2000, 200);
      myServo.write(120); Serial1.println("KAPAK_ACILDI");
      delay(4000);
      myServo.write(0); delay(200);
      digitalWrite(selenoidPin, LOW); Serial1.println("KAPAK_KAPANDI");
      delay(1000);
      Serial1.println("DOGRU_SIFRE");
      resetPassword();
    } else {
      wrongPasswordCount++;
      lcd.setCursor(0, 1); lcd.print("Yanlis Sifre!    ");
      tone(buzzerPin, 300, 500); delay(1500);
      if(wrongPasswordCount >= maxWrongTries) {
        systemLocked = true;
        lcd.setCursor(0, 0); lcd.print("Sistem Kilitlendi");
        lcd.setCursor(0, 1); lcd.print("                ");
        Serial1.println("KILITLENME");
        Serial1.println("BLOKE_FOTO");
        passwordMode = false;
      }
      resetPassword();
      Serial1.println("YANLIS_SIFRE");
    }
    return;
  }
  // Sadece wifi yoksa ve kurtarma kodları yüklüyse (offlineMode==true)
  if (offlineMode && recoveryReady) {
    int foundIdx = -1;
    for(int i=0; i<10; i++) {
      if(!recoveryUsed[i] && enteredPassword == recoveryCodes[i]) {
        foundIdx = i; break;
      }
    }
    if (foundIdx >= 0) {
      recoveryUsed[foundIdx] = true;
      lcd.setCursor(0, 1); lcd.print("Kurtarma Dogru!  ");
      digitalWrite(selenoidPin, HIGH); delay(150);
      tone(buzzerPin, 1500, 200); delay(200);
      tone(buzzerPin, 2000, 200);
      myServo.write(120); Serial1.println("KAPAK_ACILDI");
      delay(4000);
      myServo.write(0); delay(200);
      digitalWrite(selenoidPin, LOW); Serial1.println("KAPAK_KAPANDI");
      delay(1000);
      Serial1.println("KURTARMA_SIFRE");
      resetPassword();
    } else {
      lcd.setCursor(0, 1); lcd.print("Yanlis Kurtarma!");
      tone(buzzerPin, 300, 500); delay(1500);
      resetPassword();
      Serial1.println("YANLIS_KURTARMA");
    }
    return;
  }
  // Diğer tüm durumlar: giriş PASİF!
  lcd.setCursor(0,0); lcd.print("Sifre Giris Yok  ");
  lcd.setCursor(0,1); lcd.print("                ");
  delay(1000); resetPassword(); return;
}

// === LOOP ===
void loop() {
  // --- ESP32-CAM'dan gelen seri mesajları işle ---
  if (Serial1.available()) {
    String gelen = Serial1.readStringUntil('\n');
    gelen.trim();
    Serial.print("ESP32-CAM'dan gelen: "); Serial.println(gelen);

    if (gelen == "UNLOCK") {
      systemLocked = false;
      wrongPasswordCount = 0; wrongRFIDCount = 0;
      passwordMode = true; resetPassword();
      lcd.setCursor(0, 0); lcd.print("Sifre Girin:    ");
      lcd.setCursor(0, 1); lcd.print(">              ");
    }
    if (gelen == "RECOVERY_ON")           offlineMode = true;
    if (gelen == "RECOVERY_OFF")          offlineMode = false;

    // Kurtarma kodları alınıyor
    if (gelen.startsWith("RECOVERY_CODES")) {
      int idx = 0, lastPos = gelen.indexOf(':')+1, nextPos;
      while (idx < 10 && lastPos > 0) {
        nextPos = gelen.indexOf(':', lastPos);
        if (nextPos == -1) nextPos = gelen.length();
        recoveryCodes[idx] = gelen.substring(lastPos, nextPos);
        recoveryUsed[idx] = false;
        lastPos = nextPos + 1; idx++;
      }
      recoveryReady = true;
    }

    // Ana şifre - yalnız wifi modunda geçerlidir!
    if (gelen.startsWith("PASSWORD_")) {
      PASSWORD = gelen.substring(9);
      Serial.print("Yeni Sifre: "); Serial.println(PASSWORD);
      passwordMode = true;
      resetPassword();
    }

    if (gelen == "SIFRE_IPTAL") {  // Şifre zamanı geçti!
      PASSWORD = "";
      enteredPassword = "";
      passwordMode = false;
      lcd.setCursor(0, 0); lcd.print("Sifre Zamani Gecti");
      lcd.setCursor(0, 1); lcd.print("                ");
      delay(2000);
      passwordMode = true;
    }

    // Şifreyle geri sayım başlatılsın
    if (gelen == "GERI_SAYIM_BASLADI") {
      countdownActive = true;
      countdownStartMillis = millis();
    }
  }

  // Geri sayım LCD
  if (countdownActive) {
    unsigned long elapsed = (millis() - countdownStartMillis) / 1000;
    int remaining = countdownSeconds - elapsed;
    lcd.setCursor(0, 1);
    if (remaining > 0) {
      lcd.print("Kalan: ");
      lcd.print(remaining);
      lcd.print(" sn   ");
    } else {
      lcd.print("SURE DOLDU!    ");
      countdownActive = false;
      Serial1.println("SIFRE_IPTAL");
    }
  }

  // Tuş takımı kontrolü
  char key = keypad.getKey();
  if (key == ' ' || (!passwordMode && !offlineMode)) return; // Pasifken tuş girişi yok!
  if (systemLocked) {
    lcd.setCursor(0, 0); lcd.print("Sistem Kilitlendi");
    lcd.setCursor(0, 1); lcd.print("                ");
    return;
  }
  if (key) {
    Serial.print("Basilan Tus: "); Serial.println(key);
    if (key == '*') {
      checkPassword();
    } else { // şifre toplama
      enteredPassword += key;
      lcd.setCursor(0, 1); lcd.print(">");
      for (int i = 0; i < enteredPassword.length(); i++)
        lcd.print("*");
      lcd.print("         "); // ekranı temizle
      tone(buzzerPin, 1000, 100);
    }
  }

  // IMU hareket algılama
  static float ax0 = 0, ay0 = 0, az0 = 0; float ax, ay, az;
  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(ax, ay, az);
    float delta = abs(ax - ax0) + abs(ay - ay0) + abs(az - az0);
    if (delta > 2.0) {
      unsigned long now = millis();
      if (now - lastMotionTime > motionInterval) motionCount = 0;
      motionCount++;
      lastMotionTime = now;
      Serial1.println("HAREKET");
      Serial.println("Hareket algilandi! ESP32-CAM'a gonderildi.");
      if (motionCount <= 3) shortBeep();
      else threatTone();
      delay(500);
    }
    ax0 = ax; ay0 = ay; az0 = az;
  }

  // RFID okuma
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    bool authorized = true;
    for (byte i = 0; i < 4; i++) {
      if (rfid.uid.uidByte[i] != authorizedUID[i]) { authorized = false; break; }
    }
    if (authorized) {
      Serial.println("Yetkili Kart Okundu!"); Serial1.println("RFID_OKUTULDU");
      wrongRFIDCount = 0; // sayacı sıfırla
      lcd.setCursor(0, 1); lcd.print("Yetkili Kart!  ");
      digitalWrite(selenoidPin, HIGH); delay(150);
      tone(buzzerPin, 1500, 200); delay(200);
      tone(buzzerPin, 2000, 200);
      myServo.write(120);
      Serial1.println("KAPAK_ACILDI");
      delay(4000);
      myServo.write(0);
      Serial1.println("KAPAK_KAPANDI");
      delay(200);
      digitalWrite(selenoidPin, LOW); delay(1000);
    } else {
      Serial.println("Yetkisiz Kart!"); Serial1.println("YETKISIZ_RFID_OKUTULDU");
      wrongRFIDCount++;
      lcd.setCursor(0, 1); lcd.print("Kart Yetkisiz! ");
      tone(buzzerPin, 300, 500); delay(1500);
      if (wrongRFIDCount >= maxRFIDTries) {
        systemLocked = true;
        lcd.setCursor(0, 0); lcd.print("Sistem Kilitlendi");
        lcd.setCursor(0, 1); lcd.print("                ");
        Serial1.println("RFID_KILITLENME");
        Serial1.println("BLOKE_FOTO");
        passwordMode = false;
      }
    }
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    resetPassword();
  }

  // Bilgisayardan gelen veri ESP32-CAM'a yönlendirilir (debug/manuel kontrol amaçlı)
  if (Serial.available()) {
    String komut = Serial.readStringUntil('\n');
    komut.trim();
    Serial1.println(komut);
    Serial.print("ESP32-CAM'a gönderildi: "); Serial.println(komut);
  }
}