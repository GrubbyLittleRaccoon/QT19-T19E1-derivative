//From https://github.com/airzone-sama/Narfduino-Brushless-Compleat/blob/main/NBC.h
#include "NBC.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Not sure what EEPROM was for, but these are the other recommendations
/*
#include <EEPROM.h>
#include <SSD1306Ascii.h>
#include <SSD1306AsciiAvrI2c.h>
#include <SSD1306AsciiSoftSpi.h>
#include <SSD1306AsciiSpi.h>
#include <SSD1306AsciiWire.h>
#include <SSD1306init.h>
#include <Bounce2.h>*/

// AirzoneSama's stuff (Electromechanical values)
#define MIN_BATTERY_VOLTAGE (3 * 3.0)  // Originally 4*3.0, TODO: switch when we go 4S
//20545 rpm = 180fps.
//25510/26000 rpm = "recommended" 9.0, hot for 10
//30000 rpm = new default.
#define FLYWHEEL_SPEED 25510
#define MAX_WAIT_TIME 1000  // Set limit waiting for motors to rev

#define SOLENOID_PULSE_TIME 45    // Strong springs need more push time. 45 original
#define SOLENOID_RETRACT_TIME 55  // Weak springs need more release time. 55 original

// My settings
#define PIN_MAG 2      //D2 = 18
#define PIN_TRIGGER 3  //D3 = 19
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define MAGSIZE 15
#define firedCrosshair 5     //Shrink lines after firing
#define unfiredCrosshair 20  //Slowly expand on cooldown
int ammoLeft = MAGSIZE;
int crosshairLine = firedCrosshair;
int lastShrunkCrosshair = millis();
// Fire control stuff
byte numberOfDartsToShoot = 0;
bool triggerState = LOW;
int lastFired = millis();
int lastTrigger = millis(); // Trigger ISR handling

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void setup() {
  //Start setting up LCD
  Serial.begin(9600);
  // initialize OLED display with address 0x3C for 128x64
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (true) {}
  }

  //Note board has pullup only, no pulldown
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  pinMode(PIN_MAG, INPUT_PULLUP);
  //Burst fire requires ISR operation due to long relative firing time vs semi
  attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER),triggerRisingHandler,RISING); //RISING, CHANGE, HIGH...

  //Initialise motor control
  delay(500);
  NBCInit();
  delay(1000);
  FlyshotSetNewMotorSpeed(FLYWHEEL_SPEED);
  delay(500);
  FlyshotSetMotorDirectionForward(FLYSHOT_ESC_A);  //Note: you can just reverse these if you wired the motors wrong
  delay(500);
  FlyshotSetMotorDirectionForward(FLYSHOT_ESC_B);
  delay(500);
  FlyshotBeep1(FLYSHOT_ESC_BOTH);
  delay(1000);  // Just wait
  CalibrateFlywheels();

  //Finish setting up LCD
  oled.display();
  oled.clearDisplay();
  //oled.ssd1306_command(SSD1306_SEGREMAP);  //Flip display horizontally
  //oled.ssd1306_command(SSD1306_COMSCANINC); //Flip display vertically
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(0, 0);
  oled.display();
}

// Handle trigger pull, new async/interrupt method
// Note: Triggered on rising and falling due to chatter
void triggerRisingHandler(){
  if (( millis() - lastTrigger) > 20 ){ //20ms debounce
    if (triggerState == HIGH) {
      numberOfDartsToShoot = 2;
      triggerState = LOW;
      lastTrigger = millis();
    }
  }
}

// Using https://oscarliang.com/lipo-battery-guide/ for reference - 3.6V to 4.2V per cell.
// Charge range = 3S 10.8V-12.6V, 4S 14.4V-16.8V
// Therefore % = (NBCGetVoltage()-(10.8))/(12.6-10.8) for 3S


void loop() {
  // OLED display state
  oled.clearDisplay();
  
  float powerVoltage = NBCGetVoltage();
  float propThreeS = (powerVoltage-(10.8))/(12.6-10.8); 
  float propFourS = (powerVoltage-(14.4))/(16.8-14.4);
  oled.setCursor(0, 47);
  oled.println(String(powerVoltage, 2) + "V");
  oled.setCursor(40, 47);
  oled.println("3S:" + String(propThreeS*100, 0) + "%");
  oled.setCursor(40, 56);
  oled.println("4S:" + String(propFourS*100, 0) + "%");

  
  //oled.setCursor(5, 45);  // Draw the ammo at 5, 45
  oled.setCursor(0, 0);
  oled.println(ammoLeft);
  drawAmmo(ammoLeft);
  
  oled.display(); // Draw

  if( NBCGetVoltage() < MIN_BATTERY_VOLTAGE ) return; // Cap functionality at extreme lower bound 3S (9V)

  // Handle mag out
  if (!digitalRead(PIN_MAG)) ammoLeft = 15;

  // Switch chattering/bouncing introduces unreliability in interrupt-driven state monitoring (trigger pull vs release), see triggerRisingHandler()
  // Have to repeatedly recheck state inbetween shots.
  triggerState = digitalRead(PIN_TRIGGER);  

  // Handle trigger pull - old method, synchronous operation
  // Detect falling edge trigger pull, set the burst count to 2 if it's the case
  /*
  bool currentTriggerState = digitalRead(PIN_TRIGGER);
  if (currentTriggerState == LOW && previousTriggerState == HIGH) {
    // Note: Debouncing probably not necessary since I'm just setting "darts to fire" to 2 rounds (burst) if rising edge found
    numberOfDartsToShoot = 2;
  }
  previousTriggerState = currentTriggerState;
  */

  // Handle firing
  if (numberOfDartsToShoot > 0) {
    if (!FlyshotStartMotorsAndWait(MAX_WAIT_TIME))  // Stall / Jam detection  (True indicates started motor)
    {
      FlyshotStopMotorsAndWait(MAX_WAIT_TIME);
      FlyshotBeep2(FLYSHOT_ESC_BOTH);
      NBCWait(5000);
      return;
    }

    NBCSolenoidOn();
    NBCWait(SOLENOID_PULSE_TIME);
    NBCSolenoidOff();
    NBCWait(SOLENOID_RETRACT_TIME);
    numberOfDartsToShoot--;
    ammoLeft--;
    crosshairLine = firedCrosshair;
    lastShrunkCrosshair = millis();
    lastFired = millis();

    NBCProcessFlywheelSpeed();
  } else {
    if((millis() - lastFired) > 50){ // Give the darts a few ms to pass through the flywheels
      FlyshotStopMotors(); // Before stopping
    }

    // Dynamic crosshair shrinks after no firing
    if (millis() - lastShrunkCrosshair > 80) { //Shrink rate - number refers to wait period between shrinks, smaller = faster shrink
      if (crosshairLine < unfiredCrosshair) {
        crosshairLine++;
      }
      lastShrunkCrosshair = millis();
    }

    NBCProcessFlywheelSpeed();  //I'll be honest I don't really know if this is needed but I'm not removing that shit
  }
}

void drawAmmo(int ammoCount) {
  int ammoWidth = 18;
  int ammoHeight = 14;
  int ammoSpace = 1;
  int cornerRad = 1;

  int cols = 5;
  for (int ammoInd = 0; ammoInd < ammoCount; ammoInd++) {
    int circleX = ((ammoInd%cols)) * (ammoWidth + ammoSpace);
    int circleY = ((ammoInd/cols)) * (ammoHeight + ammoSpace);
    //x, y, width, height, radius, colour
    oled.fillRoundRect(circleX + 15, circleY, ammoWidth, ammoHeight, cornerRad, WHITE);
  }
}