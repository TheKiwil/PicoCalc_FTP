#include <WiFi.h>
#include <SPI.h>
#include <LovyanGFX.hpp>
#include "LGFX_config.hpp"
#include <SdFat.h>
#include <sdios.h>
#include <SimpleFTPServer.h>

#define SCK_PIN 18
#define MOSI_PIN 19
#define MISO_PIN 16
#define SS_PIN 17
#define SD_DET_PIN 22

#define FILE_NAME "config_ftp.conf"
#define KEY_MAX_LENGTH 32
#define VALUE_MAX_LENGTH 32
#define CONFIG_MAX_LINES 20

static LGFX lcd;
FtpServer ftpSrv;
SdFat sd;

// ===== Buffers et Config =====
char global_SD_buffer[KEY_MAX_LENGTH + VALUE_MAX_LENGTH + 2]; // +2 pour '=' et '\0'
char ssid[VALUE_MAX_LENGTH] = {0};
char password[VALUE_MAX_LENGTH] = {0};
char ftp_user[VALUE_MAX_LENGTH] = {0};
char ftp_password[VALUE_MAX_LENGTH] = {0};

struct ConfigEntry {
  char key[KEY_MAX_LENGTH];
  char value[VALUE_MAX_LENGTH];
};

ConfigEntry configEntries[CONFIG_MAX_LINES];
int configEntryCount = 0;

// ===== Fonctions utilitaires =====
void drawGradation(void) {
  lcd.startWrite();
  lcd.setAddrWindow(0, 0, lcd.width(), lcd.height());
  for (int y = 0; y < lcd.height(); ++y) {
    for (int x = 0; x < lcd.width(); ++x) {
      lcd.writeColor(lcd.color888(x >> 1, (x + y) >> 2, y >> 1), 1);
    }
  }
  lcd.endWrite();
}

void loadConfigFromSD() {
  FsFile configFile = sd.open(FILE_NAME, FILE_READ);
  if (!configFile) {
    Serial.println("Erreur ouverture config");
    return;
  }

  configEntryCount = 0;

  while (configFile.available() && configEntryCount < CONFIG_MAX_LINES) {
    int len = configFile.readBytesUntil('\n', global_SD_buffer, sizeof(global_SD_buffer) - 1);
    if (len > 0) {
      global_SD_buffer[len] = '\0';

      if (global_SD_buffer[len - 1] == '\r') {
        global_SD_buffer[len - 1] = '\0';
      }

      char* equalSign = strchr(global_SD_buffer, '=');
      if (equalSign) {
        int keyLen = equalSign - global_SD_buffer;
        int valLen = strlen(equalSign + 1);

        if (keyLen < KEY_MAX_LENGTH && valLen < VALUE_MAX_LENGTH) {
          strncpy(configEntries[configEntryCount].key, global_SD_buffer, keyLen);
          configEntries[configEntryCount].key[keyLen] = '\0';

          strncpy(configEntries[configEntryCount].value, equalSign + 1, valLen);
          configEntries[configEntryCount].value[valLen] = '\0';

          configEntryCount++;
        }
      }
    }
  }

  configFile.close();
}

const char* getConfigValue(const char* key) {
  for (int i = 0; i < configEntryCount; i++) {
    if (strcmp(configEntries[i].key, key) == 0) {
      return configEntries[i].value;
    }
  }
  return nullptr;
}

// ===== Setup principal =====
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(4);
  drawGradation();
  lcd.setTextScroll(true);
  lcd.setScrollRect(0, 0, lcd.width(), lcd.height());
  lcd.setTextColor(TFT_WHITE);
  lcd.println("FTP simple server");

  if (!sd.begin(SS_PIN, SD_SCK_MHZ(20))) {
    sd.initErrorHalt(&Serial);
    lcd.println("SD error");
    delay(1000);
  }

  // Chargement de la config
  loadConfigFromSD();

  const char* ssid_val = getConfigValue("ssid");
  const char* pass_val = getConfigValue("password");
  const char* ftp_user_val = getConfigValue("ftp_user");
  const char* ftp_pass_val = getConfigValue("ftp_password");

  if (!ssid_val || !pass_val || !ftp_user_val || !ftp_pass_val) {
    lcd.println("Missing config");
    Serial.println("Missing config");
    while (true);
  }

  strcpy(ssid, ssid_val);
  strcpy(password, pass_val);
  strcpy(ftp_user, ftp_user_val);
  strcpy(ftp_password, ftp_pass_val);

  // Connexion WiFi
  lcd.print("Start Wifi with ");
  lcd.println(ssid);
  WiFi.begin(ssid, password);

  int retry_wifi = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    lcd.print(".");
    Serial.print(".");
    if (retry_wifi++ >= 20) {
      lcd.println("Reconnect...");
      WiFi.begin(ssid, password);
      retry_wifi = 0;
    }
  }

  lcd.println("Connected");
  lcd.print("IP: ");
  lcd.println(WiFi.localIP());

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  lcd.println("Start FTP");
  lcd.print("User: ");
  lcd.print(ftp_user);
  lcd.print(", Pass: ");
  lcd.println(ftp_password);

  // Use custom ports for better compatibility
  // Command port: 21, Passive data port: 21000 (a port that's likely to be available)
  ftpSrv = FtpServer(21, 21000);
  ftpSrv.begin(ftp_user, ftp_password);
  
  // Print debug info
  Serial.println("FTP server started");
  Serial.print("Command port: 21, Passive port: 21000");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
  
  // Force active mode for better compatibility
  Serial.println("NOTE: If passive mode doesn't work, please use active mode in your FTP client");
}

void loop() {
  ftpSrv.handleFTP();
}
