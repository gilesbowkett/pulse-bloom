#include <avr/io.h>
#include <util/delay.h>     /* for _delay_ms() */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <wdt.h>
#if defined (__AVR_ATtiny84__)
#include <tiny/wiring.h>
#include <SoftwareSerial/SoftwareSerial.h>
#else
#include <HardwareSerial.h>
#endif
#include <neopixel/Adafruit_NeoPixel.h>
#include <si1143/si1143.h>

#include "pulse.h"
#include "smooth.h"

#define USE_SERIAL
// #define PRINT_LED_VALS

// ===================
// = Pin Definitions =
// ===================

#if defined (__AVR_ATtiny84__)
const uint8_t stemLedPin = PB2;
const uint8_t serialPin = 7;
#elif defined (__AVR_ATmega328P__)
const uint8_t sensorPin = 0; // SCL=18, SDA=19
const uint8_t stemLedPin = 8;
const uint8_t stem2LedPin = 7;
const uint8_t petalRedPin = 6;
const uint8_t petalGreenPin = 5;
const uint8_t petalBluePin = 3;
const uint8_t petalWhitePin = 9;
#endif

// ===========
// = Globals =
// ===========

long timer = 0;
long loops = 0;
const int SAMPLES_TO_AVERAGE = 5;

// Serial
#if defined (__AVR_ATtiny84__)
SoftwareSerial Serial(0, serialPin); // RX, TX
#endif

// Stem
const int NUMBER_OF_STEM_LEDS = 300;
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMBER_OF_STEM_LEDS, 
                                            stemLedPin, 
                                            NEO_GRB + NEO_KHZ800);
const int STEM_PULSE_WIDTH = 20;
int stripLedCount = 0;
int currentStripLed = 0;

// Petals
uint16_t ledBrightness = 0;
unsigned long beginLedRiseTime = 0;
unsigned long endLedRiseTime = 0;
unsigned long beginLedFallTime = 0;
unsigned long endLedFallTime = 0;

// Pulse sensor
PortI2C myBus(sensorPin);
PulsePlug pulse(myBus); 

// Pulse globals
unsigned int binOut;    // 1 or 0 depending on state of heartbeat
unsigned long red;      // read value from visible red LED
unsigned long IR1;      // read value from infrared LED1
unsigned long IR2;      // read value from infrared LED2
unsigned long IR_total; // all three LED reads added together
unsigned long lastBeat = 0;
unsigned long previousBeat = 0;
unsigned int latestBpm = 0;

// States
typedef enum
{
    STATE_RESTING = 0,
    STATE_STEM_RISING = 1,
    STATE_LED_RISING = 2,
    STATE_LED_FALLING = 3
} state_app_t;
state_app_t appState;
state_app_t petalState;

// ============
// = Routines =
// ============

void setup(){
    analogReference(EXTERNAL);
    pinMode(petalRedPin, OUTPUT);
    pinMode(petalGreenPin, OUTPUT);
    pinMode(petalBluePin, OUTPUT);
    pinMode(petalWhitePin, OUTPUT);
    analogWrite(petalRedPin, LOW);
    analogWrite(petalGreenPin, LOW);
    analogWrite(petalBluePin, LOW);
    analogWrite(petalWhitePin, LOW);
    
    appState = STATE_RESTING;
    petalState = STATE_RESTING;
    delay(50);
#ifdef USE_SERIAL
    Serial.begin(115200);
    Serial.flush();
#endif

    printHeader();
    strip.begin();
    stripLedCount = strip.numPixels();
    clearStemLeds();
    setupPulseSensor();
}

void setupPulseSensor() {
#ifdef USE_SERIAL
    if (pulse.isPresent()) {
        Serial.println("SI1143 Pulse Sensor found OK. Let's roll!");
    } else {
        Serial.println("No SI1143 found!");
    }
#endif

    pulse.setReg(PulsePlug::HW_KEY, 0x17);  
    
    pulse.setReg(PulsePlug::INT_CFG, 0x03);       // turn on interrupts
    pulse.setReg(PulsePlug::IRQ_ENABLE, 0x10);    // turn on interrupt on PS3
    pulse.setReg(PulsePlug::IRQ_MODE2, 0x01);     // interrupt on ps3 measurement
    pulse.setReg(PulsePlug::MEAS_RATE, 0x84);     // wake up every 10ms
    pulse.setReg(PulsePlug::ALS_RATE, 0x08);      // take measurement every wakeup
    pulse.setReg(PulsePlug::PS_RATE, 0x08);       // take measurement every wakeup
    
    pulse.setReg(PulsePlug::PS_LED21, 0x39);      // LED current for 2 (IR1 - high nibble) & LEDs 1 (red - low nibble) 
    pulse.setReg(PulsePlug::PS_LED3, 0x02);       // LED current for LED 3 (IR2)
/*  debug infor for the led currents
    Serial.print( "PS_LED21 = ");                                         
    Serial.println(pulse.getReg(PulsePlug::PS_LED21), BIN);                                          
    Serial.print("CHLIST = ");
    Serial.println(pulse.readParam(0x01), BIN);
*/

    pulse.writeParam(PulsePlug::PARAM_CH_LIST, 0x77);         // all measurements on
    // increasing PARAM_PS_ADC_GAIN will increase the LED on time and ADC window
    // you will see increase in brightness of visible LED's, ADC output, & noise 
    // datasheet warns not to go beyond 4 because chip or LEDs may be damaged
    pulse.writeParam(PulsePlug::PARAM_PS_ADC_GAIN, 0x00);
    // You can select which LEDs are energized for each reading.
    // The settings below (in the comments)
    // turn on only the LED that "normally" would be read
    // ie LED1 is pulsed and read first, then LED2 & LED3.
    pulse.writeParam(PulsePlug::PARAM_PSLED12_SELECT, 0x21);  // 21 select LEDs 2 & 1 (red) only                                                               
    pulse.writeParam(PulsePlug::PARAM_PSLED3_SELECT, 0x04);   // 4 = LED 3 only

    // Sensors for reading the three LEDs
    // 0x03: Large IR Photodiode
    // 0x02: Visible Photodiode - cannot be read with LEDs on - just for ambient measurement
    // 0x00: Small IR Photodiode
    pulse.writeParam(PulsePlug::PARAM_PS1_ADCMUX, 0x03);      // PS1 photodiode select 
    pulse.writeParam(PulsePlug::PARAM_PS2_ADCMUX, 0x03);      // PS2 photodiode select 
    pulse.writeParam(PulsePlug::PARAM_PS3_ADCMUX, 0x03);      // PS3 photodiode select  

    pulse.writeParam(PulsePlug::PARAM_PS_ADC_COUNTER, B01110000);    // B01110000 is default                                   
    pulse.setReg(PulsePlug::COMMAND, PulsePlug::PSALS_AUTO_Cmd);     // starts an autonomous read loop
}

void loop() {
    // printHeader();
    int sensorOn = readPulseSensor();
    
    if (sensorOn > 0) {
        appState = STATE_STEM_RISING;
    } else if (appState == STATE_STEM_RISING) {
        bool stemDone = runStemRising();
        if (stemDone) {
            appState = STATE_LED_RISING;
            beginLedRising();
        }
    } 
    
    if (appState == STATE_LED_RISING || petalState == STATE_LED_RISING) {
        bool ledDone = runLedRising();
        if (ledDone) {
            if (appState == STATE_LED_RISING) appState = STATE_LED_FALLING;
            petalState = STATE_LED_FALLING;
            beginLedFalling();
        }
    } else if (appState == STATE_LED_FALLING || petalState == STATE_LED_FALLING) {
        bool ledDone = runLedFalling();
        if (ledDone) {
            if (appState == STATE_LED_FALLING) appState = STATE_RESTING;
            petalState = STATE_RESTING;
        }
    }
}

void newHeartbeat() {
    clearStemLeds();
    currentStripLed = 0;    
}

// ==========
// = States =
// ==========

void beginStateOn() {
    
}

void clearStemLeds() {
    // Reset stem LEDs
    for (int i=0; i < stripLedCount; i++) {
        strip.setPixelColor(i, 0, 0, 0);
    }
    strip.show();
}

bool runStemRising() {
    unsigned long now = millis();
    int bpm = max(min(latestBpm, 100), 45);
    unsigned long nextBeat = lastBeat + 60000.0/(bpm/0.50);
    unsigned long millisToNextBeat = (now > nextBeat) ? 0 : (nextBeat - now);
    unsigned long millisFromLastBeat = now - lastBeat;

    double progress = (double)millisFromLastBeat / (double)(millisFromLastBeat + millisToNextBeat);
    
    int newLed = (int)floor(progress * stripLedCount);
    if (currentStripLed != newLed) {
        // Serial.print(" ---> Stem Led: ");
        // Serial.print(bpm, DEC);
        // Serial.print("/");
        // Serial.println(newLed, DEC);

        // Reset old pixels that won't be refreshed
        for (int i = -1*STEM_PULSE_WIDTH; i < STEM_PULSE_WIDTH; i++) {
            strip.setPixelColor(min(stripLedCount-1, currentStripLed + i), strip.Color(0, 0, 0));
        }

        currentStripLed = newLed;

        // Fade new pixels
        for (int i = -1*STEM_PULSE_WIDTH; i < STEM_PULSE_WIDTH; i++) {
            if (currentStripLed + i < 0 || currentStripLed + i > stripLedCount) continue;
            // Serial.print(" ---> in stem pulse width: ");
            // Serial.print(min(stripLedCount-1, currentStripLed + i), DEC);
            // Serial.print(" / ");
            // Serial.println((int)floor(255.0/(abs(i)+1)), DEC);
            uint32_t color = strip.Color((int)floor(255.0/(abs(i)+1)), 0, 0);
            strip.setPixelColor(min(stripLedCount-1, currentStripLed + i), color);
        }
        strip.show();
    }
    
    // At end of stem
    if (currentStripLed == stripLedCount || progress >= 1.0) {
        currentStripLed = 0;
        for (int i=0; i < stripLedCount; i++) {
            strip.setPixelColor(i, 0, 0, 0);
        }
        strip.show();
        return true;
    }

    return false;
}

void beginLedRising() {
    int bpm = max(min(latestBpm, 100), 45);
    unsigned long nextBeat = lastBeat + 60000.0/bpm;
    unsigned long now = millis();

    Serial.print(" ---> Led Rising: ");
    Serial.println(nextBeat);    
    if (now > endLedRiseTime) {
        // If still rising from previous rise, ignore this signal
        beginLedRiseTime = now;
    }
    if (now < endLedFallTime) {
        // Compensate for still falling led
        unsigned int remainingRiseTime = endLedFallTime - now;
        beginLedRiseTime = beginLedRiseTime - (600 - endLedFallTime);
    }
    // endLedRiseTime = beginLedRiseTime + 0.4*(nextBeat - beginLedRiseTime);
    endLedRiseTime = beginLedRiseTime + 300;
}

bool runLedRising() {
    unsigned long now = millis();
    unsigned long millisToNextBeat = (now > endLedRiseTime) ? 0 : (endLedRiseTime - now);
    unsigned long millisFromLastBeat = now - beginLedRiseTime;

    double progress = (double)millisFromLastBeat / (double)(millisFromLastBeat + millisToNextBeat);

    // Serial.print(" ---> STATE: Led Rising - ");
    // Serial.print(progress, DEC);
    // Serial.print(" Brightness: ");
    // Serial.println((int)floor(255 * progress), DEC);

    // Set Lotus LED brightness
    ledBrightness = max(8, min((int)floor(255 * progress), 255));
    analogWrite(petalRedPin, ledBrightness);
    if (progress >= 1.0) {
        return true;
    }

    return false;
}

void beginLedFalling() {
    int bpm = max(min(latestBpm, 100), 45);
    unsigned long nextBeat = lastBeat + 60000.0/bpm;
    Serial.print(" ---> Led Falling: ");
    Serial.println(nextBeat);    

    beginLedFallTime = millis();
    // endLedFallTime = beginLedFallTime + 2*(nextBeat - beginLedFallTime);
    endLedFallTime = beginLedFallTime + 600;
}

bool runLedFalling() {
    unsigned long now = millis();
    unsigned long millisToNextBeat = (now > endLedFallTime) ? 0 : (endLedFallTime - now);
    unsigned long millisFromLastBeat = now - beginLedFallTime;

    double progress = (double)millisFromLastBeat / (double)(millisFromLastBeat + millisToNextBeat);

    // Serial.print(" ---> STATE: Led Falling - ");
    // Serial.print(progress, DEC);
    // Serial.print(" Brightness: ");
    // Serial.println((int)floor(255 * progress), DEC);
    ledBrightness = max(255 - (int)floor(255 * progress), 8);
    analogWrite(petalRedPin, ledBrightness);
    
    if (progress >= 1.0) {
        return true;
    }
    
    return false;
}

// ===========
// = Sensors =
// ===========


int readPulseSensor() {
    static int foundNewFinger, red_signalSize, red_smoothValley;
    static long red_valley, red_Peak, red_smoothRedPeak, red_smoothRedValley, 
               red_HFoutput, red_smoothPeak; // for PSO2 calc
    static  int IR_valley=0, IR_peak=0, IR_smoothPeak, IR_smoothValley, binOut, lastBinOut;
    static unsigned long lastTotal, lastMillis, IRtotal, valleyTime, lastValleyTime, peakTime, lastPeakTime;
    static float IR_baseline, red_baseline, IR_HFoutput, IR_HFoutput2, shiftedOutput, LFoutput, hysterisis;
    
    if (!valleyTime) valleyTime = millis();
    if (!lastValleyTime) lastValleyTime = millis();
    if (!peakTime) peakTime = millis();
    if (!lastPeakTime) lastPeakTime = millis();
    
    unsigned long total=0, start;
    int i=0;
    int IR_signalSize;
    red = 0;
    IR1 = 0;
    IR2 = 0;
    total = 0;
    start = millis();
    
    while (i < SAMPLES_TO_AVERAGE){      
        pulse.fetchLedData();
        red += pulse.ps1;
        IR1 += pulse.ps2;
        IR2 += pulse.ps3;
        i++;
    }
    
    red = red / i;  // get averages
    IR1 = IR1 / i;
    IR2 = IR2 / i;
    total =  IR1 + IR2 + red;  // red excluded
    IRtotal = IR1 + IR2;
    
    if (red == 0 && IR1 == 0 && IR2 == 0) {
        delay(500);
        Serial.println(" ---> Resetting to fix Pulse Sensor");
        resetArduino();
    }
#ifdef PRINT_LED_VALS

    Serial.print(red);
    Serial.print("\t");
    Serial.print(IR1);
    Serial.print("\t");
    Serial.print(IR2);
    Serial.print("\t");
    Serial.println((long)total);   

#endif

    if (lastTotal < 20000L && total > 20000L) foundNewFinger = 1;  // found new finger!

    lastTotal = total;
     
    // if found a new finger prime filters first 20 times through the loop
    if (++foundNewFinger > 25) foundNewFinger = 25;   // prevent rollover 
    
    if (foundNewFinger < 20) {
        IR_baseline = total - 200;   // take a guess at the baseline to prime smooth filter
        Serial.println("found new finger");     
    } else if (total > 20000L) {    // main running function
        // baseline is the moving average of the signal - the middle of the waveform
        // the idea here is to keep track of a high frequency signal, HFoutput and a 
        // low frequency signal, LFoutput
        // The LF signal is shifted downward slightly downward (heartbeats are negative peaks)
        // The high freq signal has some hysterisis added. 
        // When the HF signal crosses the shifted LF signal (on a downward slope), 
        // we have found a heartbeat.
        IR_baseline = smooth(IRtotal, 0.99, IR_baseline);
        IR_HFoutput = smooth((IRtotal - IR_baseline), 0.2, IR_HFoutput);    // recycling output - filter to slow down response
        
        red_baseline = smooth(red, 0.99, red_baseline); 
        red_HFoutput = smooth((red - red_HFoutput), 0.2, red_HFoutput);
        
        // beat detection is performed only on the IR channel so 
        // fewer red variables are needed
        IR_HFoutput2 = IR_HFoutput + hysterisis;     
        LFoutput = smooth((IRtotal - IR_baseline), 0.95, LFoutput);
        // heartbeat signal is inverted - we are looking for negative peaks
        shiftedOutput = LFoutput - (IR_signalSize * .05);

        if (IR_HFoutput > IR_peak) IR_peak = IR_HFoutput; 
        if (red_HFoutput > red_Peak) red_Peak = red_HFoutput;
        
        // default reset - only if reset fails to occur for 1800 ms
        if (millis() - lastPeakTime > 1800) {  // reset peak detector slower than lowest human HB
            IR_smoothPeak = smooth((float)IR_peak, 0.6, (float)IR_smoothPeak);  // smooth peaks
            IR_peak = 0;
            
            red_smoothPeak = smooth((float)red_Peak, 0.6, (float)red_smoothPeak);  // smooth peaks
            red_Peak = 0;
            
            lastPeakTime = millis();
        }

        if (IR_HFoutput  < IR_valley)   IR_valley = IR_HFoutput;
        if (red_HFoutput  < red_valley)   red_valley = red_HFoutput;
        
        if (millis() - lastValleyTime > 1800) {  // insure reset slower than lowest human HB
            IR_smoothValley =  smooth((float)IR_valley, 0.6, (float)IR_smoothValley);  // smooth valleys
            IR_valley = 0;
            lastValleyTime = millis();           
        }

        hysterisis = constrain((IR_signalSize / 15), 35, 120) ;  // you might want to divide by smaller number
                                                                // if you start getting "double bumps"
            
        if  (IR_HFoutput2 < shiftedOutput) {
            // found a beat - pulses are valleys
            lastBinOut = binOut;
            binOut = 1;
            hysterisis = -hysterisis;
            IR_smoothValley =  smooth((float)IR_valley, 0.99, (float)IR_smoothValley);  // smooth valleys
            IR_signalSize = IR_smoothPeak - IR_smoothValley;
            IR_valley = 0x7FFF;
            
            red_smoothValley =  smooth((float)red_valley, 0.99, (float)red_smoothValley);  // smooth valleys
            red_signalSize = red_smoothPeak - red_smoothValley;
            red_valley = 0x7FFF;
            
            lastValleyTime = millis();
        } else {
            lastBinOut = binOut;
            binOut = 0;
            IR_smoothPeak =  smooth((float)IR_peak, 0.99, (float)IR_smoothPeak);  // smooth peaks
            IR_peak = 0;
            
            red_smoothPeak =  smooth((float)red_Peak, 0.99, (float)red_smoothPeak);  // smooth peaks
            red_Peak = 0;
            lastPeakTime = millis();
        } 

        if (lastBinOut == 1 && binOut == 0) {
            Serial.println(binOut);
            return -1;
        }

        if (lastBinOut == 0 && binOut == 1) {
            previousBeat = lastBeat;
            lastBeat = millis();
            latestBpm = 60000 / (lastBeat - previousBeat);
            newHeartbeat();
            Serial.print(binOut);
            Serial.print("\t BPM ");
            Serial.print(latestBpm);  
            Serial.print("\t IR ");
            Serial.print(IR_signalSize);
            Serial.print("\t PSO2 ");         
            Serial.println(((float)red_baseline / (float)(IR_baseline/2)), 3);                     

            return 1;
        }

    }
    
    return 0;
}

// ====================
// = Serial debugging =
// ====================

void printHeader() {
    if (millis() - timer > 1000) {
#ifdef USE_SERIAL
        Serial.print("------------------ ");
        Serial.print(loops);
        Serial.println(" ------------------");
#endif
        // blink(3, 25, true);
        timer = millis();
        loops += 1;
    }
}

void blink(int loops, int loopTime, bool half) {
    while (loops--) {
        digitalWrite(petalRedPin, HIGH);
        delay(loopTime);
        digitalWrite(petalRedPin, LOW);
        delay(loopTime / (half ? 2 : 1));
    }
}

void resetArduino() {
  asm volatile ("  jmp 0");  
}

