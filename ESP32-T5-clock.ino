/*
   ESP32 e-paper clock, showing local time and UTC time.
   Specifically designed for use by radio amateurs, who live in UTC time
   but still want to know when it is time for dinner or tea :-)

   Some code inspired from https://github.com/G6EJD/ESP32-e-Paper-Weather-Display
   Original code looks to be a combination of the weather app at:
     https://github.com/ThingPulse/esp8266-weather-station-color/blob/master/esp8266-weather-station-color.ino
     (MIT licensed)
   and using the e-paper library examples from:
     https://github.com/ZinggJM/GxEPD2
     GPLV3 Licensed

   Original code in this project is Licensed under the GPLV3
*/

#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <sunset.h>
#include <SPI.h>
#define  ENABLE_GxEPD2_display 0  //FIXME Graham - do we need this??
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <EEPROM.h>
#include <esp_deep_sleep.h>
#include <driver/adc.h>

//To enable debug, use the ESP32 options in the build menu or
// make command line to enable the log_xxx level.

#define EE_FINGERPRINT 0xEEC0F19

struct eeconfig {
  unsigned int fingerprint;
  unsigned int updateSeconds;       //How long do we nap between screen updates
  unsigned int fullRefreshMinutes;  //On what minute cycle do we do a full screen refresh
  float longitude;
  float lattitude;
  char TZ1Timezone[64];    //https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
  char TZ1Moniker[8];
  char ssid[32];
  char wifipassword[8];
};

struct eeconfig ee_default = {
  EE_FINGERPRINT,
  60,     //s between sleeps
  5,      //minute mask to do full screen refresh
  -0.12780, //longitude	//EXAMPLE - change to your location
  54.50740, //lattitude	//EXAMPLE - change to your location
  "GMT0BST,M3.5.0/01,M10.5.0/02",	//EXAMPLE - change to your timezones
  "UK",	//Example, change to your country
  "WIFISSID",	//EDIT this for your wifi SSID
  {'W', 'I', 'F', 'I', 'K', 'E', 'Y', 'X'}	//EDIT this for your wifi key
};

struct eeconfig ee;

//We have two timezones. Our base time is fixed to UTC
const char* BaseTimezone    = "UTC0";  // Choose your time zone from: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char* ntpServer   = "pool.ntp.org";                  // Or, choose a time server close to you, but in most cases it's best to use pool.ntp.org to find an NTP server

//#define DRAW_GRID 1   //Help debug layout changes
#define SCREEN_WIDTH   250
#define SCREEN_HEIGHT  122

enum alignmentType {LEFT, RIGHT, CENTER};

//Day of the week
const char* weekday_D[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
//Month
const char* month_M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Connections for Lilygo TTGO T5 V2.3_2.13 from
// https://github.com/lewisxhe/TTGO-EPaper-Series#board-pins
static const uint8_t EPD_BUSY = 4;
static const uint8_t EPD_CS   = 5;
static const uint8_t EPD_RST  = 16;
static const uint8_t EPD_DC   = 17; //Data/Command
static const uint8_t EPD_SCK  = 18; //CLK on pinout?
static const uint8_t EPD_MISO = -1; // Master-In Slave-Out not used, as no data from display
static const uint8_t EPD_MOSI = 23;

//Set up for latest 2.13 epaper - note, there have been other previous revisions - check your hardware!
GxEPD2_BW<GxEPD2_213_B73, GxEPD2_213_B73::HEIGHT> display(GxEPD2_213_B73(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;  // Select u8g2 font from here: https://github.com/olikraus/u8g2/wiki/fntlistall

String utc_time_str, utc_date_str; // strings to hold time and date
String time1_time_str;  //String for 2nd timezone
int UTC_CurrentHour = 0, UTC_CurrentMin = 0, UTC_CurrentSec = 0;
int UTC_CurrentYear = 1970, UTC_CurrentDay = 0, UTC_CurrentMonth = 0;
int TZ1_CurrentHour = 0, TZ1_CurrentMin = 0, TZ1_CurrentSec = 0;
long StartTime = 0;

SunSet sun;
double sunset = 0, sunrise = 0;

#define BUTTON_PIN  39  //IO39 is input button on T5

void setup() {
  bool need_to_ntp = false;
  bool need_full_redraw = false;
  bool button_pressed = false;
  esp_reset_reason_t why;

  StartTime = millis();
  log_i("Starting");

  if (!EEPROM.begin(sizeof(struct eeconfig)) ) {
    log_w("**EEPROM failed to initialise**");
    log_d(" EEPROM requested size: %d", sizeof(eeconfig));
  }
  load_ee();
  why = esp_reset_reason();

  pinMode(BUTTON_PIN, INPUT);
  if (digitalRead(BUTTON_PIN) == LOW) {
    log_i("Button is down");
    button_pressed = true;
  } else {
    log_d("Button is up");
  }

  if (button_pressed) {
    serial_setup();
    BeginSleep();
    //we drop back here if the user quits the menu
  }

  //Why did we wake up - if this is a first power up, then we will
  // need to do an rtc clock update from ntp
  switch (why) {
    case ESP_RST_POWERON:
      log_i("First power on - do NTP sync");
      need_to_ntp = true;
      need_full_redraw = true;
      break;

    //Waking from sleep - no need to ntp by default
    case ESP_RST_DEEPSLEEP:
      log_i("Woken from deep sleep");
      need_to_ntp = false;
      need_full_redraw = false;
      break;

    default:
      //Don't know why we woke up or booted, so be safe, and do a time sync
      // and a full screen draw
      log_w("Unknown wakeup reason %d", why);
      need_to_ntp = true;
      need_full_redraw = true;
      break;
  }

  //If the user has been in the menu, force a full refresh
  //as things may have been edited.
  if (button_pressed) {
    need_to_ntp = true;
    need_full_redraw = true;
  }

  //Only check if we've not already decided we need an ntp update
  if (!need_to_ntp ) {
    //Update our in-ram times from the RTC, and then check
    // if it is time to do an ntp update anyway
    UpdateTimes(); //Extract current time from RTC
    // Update the time at midday UTC
    //FIXME - maybe we should have the ntp sync hour in the eeprom?
    if ( UTC_CurrentHour == 12 && UTC_CurrentMin == 0 ) {
      log_i("Midday - sync to ntp");
      need_to_ntp = true;
    }
    //Have we managed to do a successful ntp yet?
    //If time is unset then we will be living in 1970, the 'epoch'.
    if ( UTC_CurrentYear == 1970 ) {
      log_i("In 1970 - still need NTP!");
      need_to_ntp = true;
    }
  }

  if ( need_to_ntp ) {
    log_i("Doing NTP sync update");
    //Bring up the wifi and sync the RTC
    if (StartWiFi() == WL_CONNECTED )
    {
      log_i("Wifi connected, do ntp");
      SetupTime();  //which also updates the in-ram copies
      StopWiFi(); // Drop as soon as possible
    } else {
      log_w("Wifi connect failed?");
    }

    //If we updated the clocks then we may as well do
    // a full screen redraw, just to make sure things look
    // nice in case of major screen changes.
    log_i("Force full redraw due to ntp update");
    need_full_redraw = true;
  }

  //Now we can drop the CPU frequency once we are sure we don't need
  //any wifi
  log_i("Dropping clock frequency");
  setCpuFrequencyMhz(20); //We can go as low as xtal/2
#if CORE_DEBUG_LEVEL >= 0
  log_i("Re-init serial port");
  Serial.begin(115200);
#endif

  if (!need_full_redraw) {
    //If we have potentially done a number of partial redraws, then
    // occasionally force a full redraw to erase any potential ghosting
    // One document recommended a full redraw every 5 partials for instance
    if (UTC_CurrentMin % ee.fullRefreshMinutes == 0) {
      log_i("Force timed full redraw");
      need_full_redraw = true;
    }
  }
  //And now we can go update the display
  log_d("Initialising Display");
  InitialiseDisplay(need_full_redraw);
  calculateSunset();

  if ( need_full_redraw ) {
    log_d("Do full refresh");
    Draw();
    log_i("Do full screen update");
    display.display(false); // Full screen update mode
    // And then we need to do a full partial update to allow us to
    // do partial partial updates later - apparently??
    display.displayWindow(0, 0, display.width(), display.height());
    log_i("draw again");
    Draw();
    log_i("do second partial screen update");
    display.display(true);
  } else {
    log_d("Do partial refresh");
    display.setPartialWindow(0, 0, display.width(), display.height());
    Draw();
    log_i("partial redraw screen");
    display.display(true); // Full screen update mode
  }

  BeginSleep();
}

void loop() {
  //We never get out of setup - never have a loop to enter.
}

//Main draw routine - used for both full screen and partial updates
void Draw(void) {
  display.fillScreen(GxEPD_WHITE);  //Need full white background for our fonts to lay over
  DrawBattery(170, 14);
  draw_clocks();
}

void BeginSleep() {
  //Try as hard as we can to go to low power
  //This effectively does a poweroff and then a hibernate
  display.hibernate();

  //Graham, FIXME - if we are in the 'big nap' time, then sleep until
  // it is wakeup hour... save battery overnight!
  //if ((CurrentHour >= WakeupTime && CurrentHour <= SleepTime)) {

  log_i("Entering %ds of sleep time", ee.updateSeconds);
  log_i("Awake for %f seconds", (millis() - StartTime) / 1000.0, 3);
  log_i("Starting deep-sleep period...");
  esp_sleep_enable_timer_wakeup(ee.updateSeconds * 1000000LL);

  //We only need/want the RTC timer to wake us up - we don't need any
  //fast or slow ram or perips or ULP processor to help us with that... ensure
  // they are turned off.
  esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
  esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);

  //power_down_flash();
  esp_deep_sleep_start();  // Sleep for e.g. 60 seconds...
}

void draw_clocks() {             // 2.13" e-paper display is 250x122 useable resolution
  char buf[6]; //'hh:mm'

#if DRAW_GRID
  Draw_Grid();
#endif
  UpdateTimes();
  u8g2Fonts.setFont(u8g2_font_helvR14_tf);  //We can squeeze an OK height date in, just above first timezone
  drawString(0, 7, utc_date_str, LEFT);
  u8g2Fonts.setFont(u8g2_font_osr21_tr);
  drawString(0, 45, String("UTC"), LEFT);
  drawString(0, 85, ee.TZ1Moniker, LEFT);
  u8g2Fonts.setFont(u8g2_font_osr35_tn);
  drawString(90, 45, utc_time_str, LEFT);   //Nice fixed point 'LCD' clock font for time
  drawString(90, 85, time1_time_str, LEFT);   //Nice fixed point 'LCD' clock font for time

  u8g2Fonts.setFont(u8g2_font_osr18_tr);
  drawString(0, 110, String("Rise"), LEFT);
  drawString(130, 110, String("Set"), LEFT);

  u8g2Fonts.setFont(u8g2_font_osr18_tn);
  sprintf(buf, "%d:%02d", ((int)sunrise) / 60, ((int)sunrise) % 60);
  drawString(85, 110, String(buf), RIGHT);
  sprintf(buf, "%d:%02d", ((int)sunset) / 60, ((int)sunset) % 60);
  drawString(210, 110, String(buf), RIGHT);
  log_i("Draw done");
}

// Help debug screen layout by drawing a grid of little crosses
void Draw_Grid() {
  int x, y;
  const int grid_step = 10;

  //Draw the screen border so we know how far we can push things out
  display.drawLine(0, 0, SCREEN_WIDTH - 1, 0, GxEPD_BLACK); //across top
  display.drawLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, GxEPD_BLACK); //across bottom
  display.drawLine(0, 0, 0, SCREEN_HEIGHT - 1, GxEPD_BLACK); //lhs
  display.drawLine(SCREEN_WIDTH - 1, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, GxEPD_BLACK); //rhs

  for ( x = grid_step; x < SCREEN_WIDTH; x += grid_step ) {
    for ( y = grid_step; y < SCREEN_HEIGHT; y += grid_step ) {
      display.drawLine(x - 1, y, x + 1, y, GxEPD_BLACK); //Horizontal line
      display.drawLine(x, y - 1, x, y + 1, GxEPD_BLACK); //Vertical line
    }
  }
}

uint8_t StartWiFi() {
  char ssid[sizeof(ee.ssid)+1];
  char password[sizeof(ee.wifipassword)+1];

  strncpy(ssid, ee.ssid, sizeof(ee.ssid));
  ssid[sizeof(ee.ssid)] = '\0';
  strncpy(password, ee.wifipassword, sizeof(ee.wifipassword));
  password[sizeof(ee.wifipassword)] = '\0';

  log_i("Connecting to: [%s]", ssid);
  IPAddress dns(8, 8, 8, 8); // Google DNS
  WiFi.disconnect();
  WiFi.mode(WIFI_STA); // switch off AP
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  delay(1000);  //A little wait before assessing may help stability
  unsigned long start = millis();
  uint8_t connectionStatus;
  bool AttemptConnection = true;
  while (AttemptConnection) {
    connectionStatus = WiFi.status();
    if (millis() > start + 15000) { // Wait 15-secs maximum
      AttemptConnection = false;
    }
    if (connectionStatus == WL_CONNECTED || connectionStatus == WL_CONNECT_FAILED) {
      AttemptConnection = false;
    }
    delay(50);
  }
  if (connectionStatus == WL_CONNECTED) {
    log_i("WiFi connected at: [%s]", (WiFi.localIP().toString()).c_str());
  } else {
    log_w("WiFi connection *** FAILED ***");
  }
  
  return connectionStatus;
}

void StopWiFi() {
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
}

//Set the RTC from NTP source
boolean SetupTime() {
  //Always set the 'physical' clock to be UTC
  configTime(0, 0, ntpServer, "time.nist.gov");
  setenv("TZ", BaseTimezone, 1);  //setenv()adds the "TZ" variable to the environment with a value TimeZone, only used if set to 1, 0 means no change
  tzset(); // Set the TZ environment variable
  delay(100); ///why??? - but, it seems to make ntp update more stable???
  bool TimeStatus = UpdateTimes();
  return TimeStatus;
}

//Update the in-memory local and 2nd time values from the RTC
boolean UpdateTimes() {
  time_t now;
  struct tm timeinfo;
  char   time_output[30], day_output[30], update_time[30];
  time(&now);

  //*** first UTC
  setenv("TZ", BaseTimezone, 1);  //setenv()adds the "TZ" variable to the environment with a value TimeZone, only used if set to 1, 0 means no change
  tzset(); // Set the TZ environment variable

  localtime_r(&now, &timeinfo);
  UTC_CurrentHour = timeinfo.tm_hour;
  UTC_CurrentMin  = timeinfo.tm_min;
  UTC_CurrentSec  = timeinfo.tm_sec;
  UTC_CurrentYear = timeinfo.tm_year + 1900;
  UTC_CurrentMonth = timeinfo.tm_mon;
  UTC_CurrentDay = timeinfo.tm_mday;

  //See http://www.cplusplus.com/reference/ctime/strftime/
  sprintf(day_output, "%s  %02u-%s-%04u", weekday_D[timeinfo.tm_wday], timeinfo.tm_mday, month_M[timeinfo.tm_mon], (timeinfo.tm_year) + 1900);
  strftime(update_time, sizeof(update_time), "%H:%M", &timeinfo);  // Creates: '@ 14:05', 24h, no am or pm or seconds.   and change from 30 to 8 <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
  sprintf(time_output, "%s", update_time);
  log_i("Date/Time [%s][%s]", day_output, time_output);
  utc_date_str = day_output;
  utc_time_str = time_output;

  //And now the 2nd timezone
  setenv("TZ", ee.TZ1Timezone, 1);  //setenv()adds the "TZ" variable to the environment with a value TimeZone, only used if set to 1, 0 means no change
  tzset(); // Set the TZ environment variable

  localtime_r(&now, &timeinfo);
  strftime(update_time, sizeof(update_time), "%H:%M", &timeinfo);  // Creates: '@ 14:05', 24h, no am or pm or seconds.   and change from 30 to 8 <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
  time1_time_str = update_time;

  //And finally, for kicks, put the TZ back to UTC
  setenv("TZ", BaseTimezone, 1);  //setenv()adds the "TZ" variable to the environment with a value TimeZone, only used if set to 1, 0 means no change
  tzset(); // Set the TZ environment variable

  return true;
}

void calculateSunset(void) {
  //sunrise and sunset in 'local' timezone
  setenv("TZ", ee.TZ1Timezone, 1);  //setenv()adds the "TZ" variable to the environment with a value TimeZone, only used if set to 1, 0 means no change
  tzset(); // Set the TZ environment variable
  sun.setTZOffset(0);   //May need to check if this is correct...
  sun.setPosition(ee.lattitude, ee.longitude, 0);  //Did we need a timezone offset??
  //+1 for month - translate from zero indexing
  sun.setCurrentDate(UTC_CurrentYear, UTC_CurrentMonth + 1, UTC_CurrentDay);

  log_i("Calculating rise/set for %d-%d-%d", UTC_CurrentYear, UTC_CurrentMonth, UTC_CurrentDay);

  sunset = sun.calcSunset();
  sunrise = sun.calcSunrise();
}

void DrawBattery(int x, int y) {
  uint8_t percentage = 100;
  float voltage;

  adc_power_on();
  voltage = analogRead(35) / 4096.0 * 7.46;
  //Turn off adc as soon as we are done reading it - we are done with both
  //the analog read and the wifi at this point.
  adc_power_off();
  
  if (voltage > 1 ) { // Only display if there is a valid reading
    log_i("Voltage = %f", voltage);
    percentage = 2836.9625 * pow(voltage, 4) - 43987.4889 * pow(voltage, 3) + 255233.8134 * pow(voltage, 2) - 656689.7123 * voltage + 632041.7303;
    if (voltage >= 4.20) percentage = 100;
    if (voltage <= 3.50) percentage = 0;
    display.drawRect(x + 15, y - 12, 19, 10, GxEPD_BLACK);
    display.fillRect(x + 34, y - 10, 2, 5, GxEPD_BLACK);
    display.fillRect(x + 17, y - 10, 15 * percentage / 100.0, 6, GxEPD_BLACK);
    u8g2Fonts.setFont(u8g2_font_helvB10_tf);   // Explore u8g2 fonts from here: https://github.com/olikraus/u8g2/wiki/fntlistall
    drawString(x + 60, y - 11, String(percentage) + "%", RIGHT);
  }
}

void drawString(int x, int y, String text, alignmentType alignment) {
  int16_t  x1, y1; //the bounds of x,y and w and h of the variable 'text' in pixels.
  uint16_t w, h;
  display.setTextWrap(false);
  display.getTextBounds(text, x, y, &x1, &y1, &w, &h);
  if (alignment == RIGHT)  x = x - w;
  if (alignment == CENTER) x = x - w / 2;
  u8g2Fonts.setCursor(x, y + h);
  u8g2Fonts.print(text);
}

void InitialiseDisplay(bool full_update) {
  log_d("Begin InitialiseDisplay...");
  if ( full_update )  display.init(0);  //Full initialisation
  else display.init(0, false);  //Initialise with partial update ability enabled
  SPI.end();
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
  display.setRotation(3);                    // Use 1 or 3 for landscape modes
  u8g2Fonts.begin(display);                  // connect u8g2 procedures to Adafruit GFX
  u8g2Fonts.setFontMode(1);                  // use u8g2 transparent mode (this is default)
  u8g2Fonts.setFontDirection(0);             // left to right (this is default)
  u8g2Fonts.setForegroundColor(GxEPD_BLACK); // apply Adafruit GFX color
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE); // apply Adafruit GFX color
  display.setFullWindow();  //Graham FIXME - do we need this?
  log_d("... End InitialiseDisplay");
}

void load_ee(void) {
  log_i("Loading eeprom data");

  for (int i = 0; i < sizeof(ee); i++) {
    ((char *)&ee)[i] = EEPROM.read(i);
    log_d("Read ee %d:%02x", i, ((char *)&ee)[i]);
  }

  if ( ee.fingerprint != EE_FINGERPRINT ) {
    log_i("ee data corrupt/unset. Loading defaults");
    log_i(" fingerprint was: 0x%08x", ee.fingerprint);
    ee = ee_default;
  } else {
    log_i("ee loaded");    
  }
}

/**** From here on, most things are only used from the serial menu, so have Serial
 *  enabled, and thus do not need to use log_xxx or have #if DEBUG guards.
 */
void factoryReset(void) {
  Serial.println("Factory reset ee");

  for (int i = 0; i < sizeof(ee_default); i++) {
    log_d("Write ee %d:%02x", i, ((char *)&ee_default)[i]);
    EEPROM.write(i, ((char *)&ee_default)[i]);
  }

  if (!EEPROM.commit()) {
    Serial.println("**eeprom commit failed**");  
  } else {
    Serial.println("eeprom data commited");
  }
  load_ee();
}

void save_ee(void) {
  Serial.println("Saving eeprom data");

  for (int i = 0; i < sizeof(ee); i++) {
    log_d("Write ee %d:%02x", i, ((char *)&ee)[i]);
    EEPROM.write(i, ((char *)&ee)[i]);
  }
  if (!EEPROM.commit()) {
    Serial.println("**eeprom commit failed**");  
  } else {
    Serial.println("eeprom data commited");
  }
}

//User has requested serial setup mode
// Here we let them view, change and save the settings
void serial_setup(void) {
  bool quit = false;

//Try to ensure we get serial set up
//If global log is enabled, presume it has been configured already.
#if CORE_DEBUG_LEVEL <= 0
  Serial.begin(115200);
#endif

  Serial.setTimeout(30 * 1000); //Don't timeout at default 1s for user input!!!
  Serial.println("Entering Serial Setup mode");

  //first, show on the display we are in serial setup mode!
  InitialiseDisplay(true);
  u8g2Fonts.setFont(u8g2_font_helvR14_tf);
  display.fillScreen(GxEPD_WHITE);
  drawString(0, 10, String("In Serial Config mode."), LEFT);
  drawString(0, 30, String("Connect to USB UART"), LEFT);
  drawString(0, 50, String("to configure unit."), LEFT);
  drawString(0, 70, String("Type '?' for help."), LEFT);
  display.display(false);

  Serial.println("'?' for help");
  Serial.print("cmd> ");

  while (!quit) {
    if (Serial.available() > 0 ) {
      char c = Serial.read();

      //Dump any spare chars, like the CRLF pair - otherwise we pick them
      // up in the subroutines.
      for( int i=0; i<10; i++ ) {
        if (Serial.available() > 0 ) {
          char throwaway = Serial.read();
        } else {
          break;
        }
      }

      switch ( c ) {
        case 'f':
          factoryReset();
          break;

        case 'h':
        case '?':
          printHelp();
          break;

        case 'l':
          readLongitude();
          break;

        case 'L':
          readLattitude();
          break;

        case 'm':
          readTimezoneMoniker();
          break;

        case 'p':
          printConfig();
          break;

        case 'P':
          readWifiPassword();
          break;
          
        case 'Q':
        case 'q':
          quit = true;
          Serial.println("Quitting");
          break;

        case 'r':
          Serial.println("Undoing changes, reloading ee");
          load_ee();
          break;
          
        case 's':
          save_ee();
          break;
          
        case 'S':
          readWifiSSID();
          break;
                    
        case 't':
          readTimezone();
          break;
                    
        case 'u':
          readSleepTime();
          break;

        case 'U':
          readFullRefreshMask();
          break;

        case '\r':
        case '\n':
          break;

        default:
          Serial.println(String("Unknown char: ") + String(c));
          break;
      }
    }
    delay(100); //Don't hard spin
  }
}

void readFullRefreshMask(void) {
  String s;

  Serial.print("Input number of minutes between full screen refreshes: ");
  s = Serial.readStringUntil('\r');
  ee.fullRefreshMinutes = atoi(s.c_str());
  Serial.println(String("New value: ") + String(ee.fullRefreshMinutes));
}

void readLongitude(void) {
  String s;

  Serial.print("Input Longitude: ");
  s = Serial.readStringUntil('\r');
  ee.longitude = atof(s.c_str());
  Serial.println(String("New value: ") + String(ee.longitude));
}

void readLattitude(void) {
  String s;

  Serial.print("Input Lattitude: ");
  s = Serial.readStringUntil('\r');
  ee.lattitude = atof(s.c_str());
  Serial.println(String("New value: ") + String(ee.lattitude));
}

void readSleepTime(void) {
  String s;

  Serial.print("Input time between screen refreshes in seconds: ");
  s = Serial.readStringUntil('\r');
  ee.updateSeconds = atoi(s.c_str());
  Serial.println(String("New value: ") + String(ee.updateSeconds));
}

void readWifiPassword(void) {
  String s;
  char buf[9];

  Serial.print("Input new WiFi password: ");
  s = Serial.readStringUntil('\r');
  strncpy(ee.wifipassword, s.c_str(), sizeof(ee.wifipassword));
  strncpy(buf, ee.wifipassword, sizeof(ee.wifipassword));
  buf[sizeof(ee.wifipassword)] = '\0';
  Serial.println(String("New value: ") + String(ee.wifipassword));
}

void readWifiSSID(void) {
  String s;
  char buf[33];

  Serial.print("Input new WiFi SSID: ");
  s = Serial.readStringUntil('\r');
  strncpy(ee.ssid, s.c_str(), sizeof(ee.ssid));
  strncpy(buf, ee.ssid, sizeof(ee.ssid));
  buf[sizeof(ee.ssid)] = '\0';
  Serial.println(String("New value: ") + String(ee.ssid));
}

void readTimezone(void) {
  String s;
  char buf[64];

  Serial.print("Input new timezone string: ");
  s = Serial.readStringUntil('\r');
  strncpy(ee.TZ1Timezone, s.c_str(), sizeof(ee.TZ1Timezone));
  strncpy(buf, ee.TZ1Timezone, sizeof(ee.TZ1Timezone));
  buf[sizeof(ee.TZ1Timezone)] = '\0';
  Serial.println(String("New value: ") + String(ee.TZ1Timezone));
}

void readTimezoneMoniker(void) {
  String s;
  char buf[9];

  Serial.print("Input new timezone moniker: ");
  s = Serial.readStringUntil('\r');
  strncpy(ee.TZ1Moniker, s.c_str(), sizeof(ee.TZ1Moniker));
  strncpy(buf, ee.TZ1Moniker, sizeof(ee.TZ1Moniker));
  buf[sizeof(ee.TZ1Moniker)] = '\0';
  Serial.println(String("New value: ") + String(ee.TZ1Moniker));
}

void printHelp(void) {
  Serial.println("Help:");
  Serial.println("  f : Factory reset");
  Serial.println("  h,? : Print this help");
  Serial.println("  l : Longitude");
  Serial.println("  L : Lattitude");
  Serial.println("  m : Timezone moniker");
  Serial.println("  p : Print current config");
  Serial.println("  P : WiFi password");
  Serial.println("  Q,q : Quit setup");
  Serial.println("  r : Reset settings");
  Serial.println("  s : Save settings");
  Serial.println("  S : Wifi SSID");
  Serial.println("  t : Timezone string");
  Serial.println("  u : Refresh sleep time");
  Serial.println("  U : Full Refresh minute mask");
}

void printConfig(void) {
  char buf[64];

  Serial.println("EE config:");
  Serial.println(String("  Full refresh minute mask: ") + String(ee.fullRefreshMinutes));
  Serial.println(String("  Inter-refresh sleep seconds: ") + String(ee.updateSeconds));
  Serial.println(String("  TZ1 Timezone: ") + String(ee.TZ1Timezone));
  Serial.println(String("  TZ1 Moniker: ") + String(ee.TZ1Moniker));
  Serial.println(String("  longitude: ") + String(ee.longitude));
  Serial.println(String("  lattitude: ") + String(ee.lattitude));
  strncpy(buf, ee.ssid, sizeof(ee.ssid));
  buf[sizeof(ee.ssid)] = '\0';
  Serial.println(String("  WiFi SSID: ") + String(buf));
  strncpy(buf, ee.wifipassword, sizeof(ee.wifipassword));
  buf[sizeof(ee.wifipassword)] = '\0';
  Serial.println(String("  WiFi Password: ") + String(buf));
}
