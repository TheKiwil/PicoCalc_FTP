/* PicoCalc FTP server
 *
 *  - Esc returns to the PicoCalc boot menu (UF2 Loader + PicoCalc Shell),
 *    so no power cycle is needed
 *  - settings come from /config_ftp.conf; if it's missing, the WiFi
 *    settings (and time zone) come from /settime.cfg or
 *    /virtualt/virtualt.cfg, and the FTP login defaults to
 *    picocalc / picocalc
 *  - config lines may have spaces around '=' and '#' comments; WiFi
 *    passwords up to 79 characters
 *  - the clock is set from NTP (or kept from an earlier program in the
 *    RP2350's always-on timer), so uploaded files get real dates
 *  - uploads go to "name.~up" and replace the old file only once they
 *    have fully arrived; a failed upload leaves the old file as it was
 *  - every command the client sends is shown on screen, with the result
 *    of each upload
 */
#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <LovyanGFX.hpp>
#include "LGFX_config.hpp"
#include <SdFat.h>
#include <sdios.h>
#include <SimpleFTPServer.h>
#include "hardware/structs/watchdog.h"
#include "pico/aon_timer.h"
#include <lwip_wrap.h>
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"

#define SS_PIN 17
#define SD_DET_PIN 22

/* keyboard (the STM32 "south bridge") on I2C1 */
#define SB_SDA  6
#define SB_SCL  7
#define SB_ADDR 0x1F
#define SB_REG_FIF 0x09
#define KEY_ESC 0xB1

static LGFX lcd;
FtpServer ftpSrv;
SdFat sd;

static char ssid[64], password[80], ftp_user[32] = "picocalc", ftp_password[32] = "picocalc";
static char tz[64] = "PST8PDT,M3.2.0,M11.1.0";
static const char *cfg_used = 0;

/* ------------------------------------------------------------ screen */

/* The log is kept here and redrawn, never scrolled on the display itself:
   LovyanGFX's text scrolling reads the screen back over SPI (at 25 MHz,
   which the PicoCalc's display can't do reliably -- garbled lines), and its
   read loop has no timeout, so a lost byte froze the whole server.  This
   only ever writes to the display. */

class Console : public Print
{
public:
  static const int COLS = 53, ROWS = 40;
  void begin()
  {
    cols = lcd.width() / 6;  if (cols > COLS) cols = COLS;
    rows = lcd.height() / 8; if (rows > ROWS) rows = ROWS;
    memset(ch, ' ', sizeof ch);
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) co[r][c] = TFT_WHITE;
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(TFT_BLACK);
  }
  void color(uint16_t c) { fg = c; }
  size_t write(uint8_t b) override
  {
    if (b == '\r') return 1;
    if (b == '\n') { newline(); return 1; }
    if (x >= cols) newline();
    ch[y][x] = b;
    co[y][x] = fg;
    drawRun(y, x, x + 1);
    x++;
    return 1;
  }
private:
  char ch[ROWS][COLS];
  uint16_t co[ROWS][COLS];
  int cols = COLS, rows = 30, x = 0, y = 0;
  uint16_t fg = TFT_WHITE;
  void newline()
  {
    x = 0;
    if (++y < rows) return;
    y = rows - 1;                           /* shift up one line and redraw */
    memmove(ch, ch[1], sizeof ch[0] * (rows - 1));
    memmove(co, co[1], sizeof co[0] * (rows - 1));
    memset(ch[y], ' ', COLS);
    for (int r = 0; r < rows; r++) drawRun(r, 0, cols);
  }
  void drawRun(int r, int from, int to)
  {
    char buf[COLS + 1];
    while (from < to)
    {
      int e = from;
      uint16_t c = co[r][from];
      while (e < to && (co[r][e] == c || ch[r][e] == ' ')) { buf[e - from] = ch[r][e]; e++; }
      buf[e - from] = 0;
      lcd.setTextColor(c, TFT_BLACK);       /* background colour: fills, no read */
      lcd.drawString(buf, from * 6, r * 8);
      from = e;
    }
  }
};
static Console con;

/* ------------------------------------------------------------ settings */

static char *trim(char *s)
{
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
  return s;
}

static void set(char *dst, size_t n, const char *v) { strncpy(dst, v, n - 1); dst[n - 1] = 0; }

/* read key=value lines; returns true if the file was there */
static bool read_cfg(const char *path, bool ftp_keys)
{
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return false;
  char line[160];
  while (f.available())
  {
    int len = f.readBytesUntil('\n', line, sizeof line - 1);
    line[len] = 0;
    char *l = trim(line);
    if (!*l || *l == '#') continue;
    char *eq = strchr(l, '=');
    if (!eq) continue;
    *eq = 0;
    char *k = trim(l), *v = trim(eq + 1);
    if (!strcasecmp(k, "ssid"))          set(ssid, sizeof ssid, v);
    else if (!strcasecmp(k, "password")) set(password, sizeof password, v);
    else if (!strcasecmp(k, "tz"))       set(tz, sizeof tz, v);
    else if (ftp_keys && !strcasecmp(k, "ftp_user"))     set(ftp_user, sizeof ftp_user, v);
    else if (ftp_keys && !strcasecmp(k, "ftp_password")) set(ftp_password, sizeof ftp_password, v);
  }
  f.close();
  return true;
}

static void load_settings()
{
  if (read_cfg("/config_ftp.conf", true))
  {
    cfg_used = "config_ftp.conf";
    if (ssid[0]) return;
  }
  if (read_cfg("/settime.cfg", false) && ssid[0])
    cfg_used = cfg_used ? "config_ftp.conf + settime.cfg" : "settime.cfg";
  else if (read_cfg("/virtualt/virtualt.cfg", false) && ssid[0])
    cfg_used = cfg_used ? "config_ftp.conf + virtualt.cfg" : "virtualt/virtualt.cfg";
}

/* ------------------------------------------------------------ keyboard */

static void net_report();

static volatile bool want_report = false;
extern volatile const char *ftp_where;

static bool esc_pressed()
{
  Wire1.beginTransmission(SB_ADDR);
  Wire1.write(SB_REG_FIF);
  if (Wire1.endTransmission() != 0) return false;
  if (Wire1.requestFrom(SB_ADDR, 2) != 2) return false;
  uint8_t state = Wire1.read(), key = Wire1.read();
  if (state == 1 && (key == 'n' || key == 'N')) want_report = true;
  return state == 1 && key == KEY_ESC;      /* 1 = pressed */
}

/* back to the PicoCalc Shell: the same request picocalc_shell.h makes
   (UF2 Loader 2.3+, stage3 loads /BOOT2350.UF2) */
static void return_to_menu()
{
  con.println("\nBack to the menu...");
  sd.end();
  delay(200);
  pinMode(SD_DET_PIN, INPUT_PULLUP);
  delayMicroseconds(100);
  if (digitalRead(SD_DET_PIN))              /* no card: the loader couldn't start the shell */
  {
    con.println("No SD card -- switch off instead.");
    for (;;) delay(1000);
  }
  watchdog_hw->scratch[1] = 1;              /* boot from the card */
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[0] = 0xe98cc638u;
  watchdog_reboot(0, 0, 0);
  for (;;) ;
}

static void wait_or_menu(const char *why)
{
  con.println(why);
  con.println("Press Esc for the menu.");
  for (;;) { if (esc_pressed()) return_to_menu(); delay(50); }
}

/* ------------------------------------------------------------ clock */

/* FAT file dates, in local time */
static void fat_datetime(uint16_t *date, uint16_t *time_)
{
  time_t now = time(NULL);
  struct tm t;
  localtime_r(&now, &t);
  *date = FS_DATE(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  *time_ = FS_TIME(t.tm_hour, t.tm_min, t.tm_sec);
}

static bool clock_is_set() { return time(NULL) > 1600000000; }

static void set_clock()
{
  setenv("TZ", tz, 1);
  tzset();

  /* the always-on timer keeps the time from an earlier program
     (settime, VirtualT) until the power goes off */
  struct timespec ts;
  if (aon_timer_is_running() && aon_timer_get_time(&ts) && ts.tv_sec > 1600000000)
  {
    struct timeval tv = { ts.tv_sec, (suseconds_t)(ts.tv_nsec / 1000) };
    settimeofday(&tv, NULL);
  }
  else
  {
    con.print("Clock: asking pool.ntp.org...");
    NTP.begin("pool.ntp.org");
    uint32_t until = millis() + 10000;
    while (!clock_is_set() && millis() < until) delay(100);
    if (clock_is_set())
    {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      struct timespec t2 = { tv.tv_sec, tv.tv_usec * 1000 };
      if (aon_timer_is_running()) aon_timer_set_time(&t2); else aon_timer_start(&t2);
    }
    con.println(clock_is_set() ? " ok" : " no answer");
  }
  if (clock_is_set())
  {
    FsDateTime::setCallback(fat_datetime);
    time_t now = time(NULL);
    char buf[40];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", localtime(&now));
    con.print("Time: ");
    con.println(buf);
  }
  else
    con.println("Files will be undated.");
}

/* ------------------------------------------------------------ log */

static void ftp_log(const char *line)
{
  char t[12] = "";
  if (clock_is_set())
  {
    time_t now = time(NULL);
    strftime(t, sizeof t, "%H:%M:%S ", localtime(&now));
  }
  bool result = line[0] == ' ';
  bool bad = strstr(line, "failed") || strstr(line, "ERROR") || strstr(line, "lost") ||
             strstr(line, " 425") || strstr(line, " 426") || strstr(line, " 451") || strstr(line, " 552") || strstr(line, "waited") || strstr(line, "not answering");
  con.color(bad ? TFT_RED : result ? TFT_GREEN : TFT_LIGHTGREY);
  con.print(t);
  con.println(line);
  con.color(TFT_WHITE);
}

/* the network library's connection table (it has room for MEMP_NUM_TCP_PCB,
   5 in this build): local port and state of each.  Shown on a 425, and
   when N is pressed. */
static void net_report()
{
  static const char *st[] = { "closed", "listen", "syn-sent", "syn-rcvd", "open",
                              "fin-wait1", "fin-wait2", "close-wait", "closing",
                              "last-ack", "time-wait" };
  char l[200];
  int n = 0, len = 0;
  {
    LWIPMutex m;
    for (int list = 0; list < 2; list++)
      for (struct tcp_pcb *p = list ? tcp_tw_pcbs : tcp_active_pcbs; p && len < 180; p = p->next)
      {
        n++;
        len += snprintf(l + len, sizeof l - len, "%s%u %s", n > 1 ? ", " : "", p->local_port,
                        p->state <= TIME_WAIT ? st[p->state] : "?");
      }
  }
  char h[48];
  snprintf(h, sizeof h, "  Connections in use: %d of %d", n, MEMP_NUM_TCP_PCB);
  ftp_log(h);
  if (n)
  {
    char *q = l;                            /* wrap at about 50 characters */
    while (*q)
    {
      char line[56] = "    ";
      int k = strlen(q) > 50 ? 50 : strlen(q);
      if (k == 50) { while (k > 10 && q[k] != ' ') k--; }
      strncat(line, q, k);
      ftp_log(line);
      q += k;
      while (*q == ' ') q++;
    }
  }
}

void ftp_net_report() { net_report(); }

/* ------------------------------------------------------------ main */

void setup()
{
  Wire1.setSDA(SB_SDA);
  Wire1.setSCL(SB_SCL);
  Wire1.begin();
  Wire1.setClock(10000);
  while (esc_pressed()) ;                   /* drain keys left from the menu */

  /* The display comes up switched off (see LGFX_config.hpp): clear it, then
     turn it on, so it never flashes white */
  lcd.init();
  lcd.setRotation(4);
  con.begin();
  lcd.startWrite();
  lcd.writeCommand(0x29);                   /* DISPON */
  lcd.endWrite();
  con.color(TFT_GREEN);
  con.println("PicoCalc FTP server   (Esc: menu)");
  con.color(TFT_WHITE);

  if (!sd.begin(SS_PIN, SD_SCK_MHZ(20)))
    wait_or_menu("SD card error.");

  load_settings();
  if (!ssid[0])
    wait_or_menu("No WiFi settings: put ssid= and password=\n"
                 "in /config_ftp.conf (or settime.cfg).");
  con.print("Settings: ");
  con.println(cfg_used);

  con.print("WiFi: joining ");
  con.println(ssid);
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    for (int i = 0; i < 10; i++) { if (esc_pressed()) return_to_menu(); delay(50); }
    con.print(".");
    if (++tries % 20 == 0)
    {
      con.println(" retrying");
      WiFi.begin(ssid, password);
    }
  }
  con.println();
  WiFi.noLowPowerMode();                    /* power saving delays packets enough to time out transfers */

  set_clock();

  ftpSrv = FtpServer(21, 50009);              /* passive data port: one, fixed */
  ftpSrv.setLogCallback(ftp_log);
  ftpSrv.begin(ftp_user, ftp_password);

  con.println();
  con.color(TFT_YELLOW);
  con.print("ftp://");
  con.println(WiFi.localIP());
  con.color(TFT_WHITE);
  con.print("User: ");
  con.print(ftp_user);
  con.print("   Password: ");
  con.println(ftp_password);
  con.println("One connection at a time (set your");
  con.println("client's connection limit to 1).");
  con.println();
  con.println("Esc: back to the menu (wait for");
  con.println("transfers to finish first).");
  con.println();
  {
    char l[48];
    snprintf(l, sizeof l, "WiFi signal %ld dBm", (long)WiFi.RSSI());
    con.println(l);
  }
  con.println("N: show network connections.");
  con.println("Commands from your client appear below.");
}

/* the WiFi can drop on its own; nothing would notice, and every
   transfer would then fail with 425 until a restart.  Say so on the
   screen and rejoin. */
static void watch_wifi()
{
  static bool up = true;
  static uint32_t lost_at, retry_at;
  bool now_up = WiFi.status() == WL_CONNECTED;
  char l[64];
  if (up && !now_up)
  {
    lost_at = millis();
    retry_at = millis() + 3000;
    snprintf(l, sizeof l, "WiFi lost (status %d) -- rejoining", (int)WiFi.status());
    ftp_log(l);
  }
  else if (!up && now_up)
  {
    snprintf(l, sizeof l, "  WiFi back after %lu s, signal %ld dBm",
             (unsigned long)((millis() - lost_at) / 1000), (long)WiFi.RSSI());
    ftp_log(l);
    WiFi.noLowPowerMode();
  }
  else if (!now_up && (int32_t)(millis() - retry_at) >= 0)
  {
    retry_at = millis() + 15000;
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
  up = now_up;
}


void loop()
{
  ftp_where = "loop";
  static uint32_t next_key;
  if ((int32_t)(millis() - next_key) >= 0)
  {
    next_key = millis() + 50;
    ftp_where = "keyboard";
    if (esc_pressed()) return_to_menu();
    ftp_where = "loop";
  }
  if (want_report) { want_report = false; net_report(); }

  ftpSrv.handleFTP();
  ftp_where = "loop";
  static uint32_t next_wifi;
  if ((int32_t)(millis() - next_wifi) >= 0)
  {
    next_wifi = millis() + 500;
    ftp_where = "WiFi check";
    watch_wifi();
    ftp_where = "loop";
  }
}
