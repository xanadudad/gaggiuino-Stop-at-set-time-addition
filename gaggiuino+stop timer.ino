// #define SINGLE_HX711_CLOCK
#define DEBUG_ENABLED
// #define MAX31855_ENABLED
#define TIMERINTERRUPT_ENABLED
#if defined(DEBUG_ENABLED) && defined(ARDUINO_ARCH_STM32)
  #include "dbg.h"
#endif
#if defined(ARDUINO_ARCH_AVR)
  #include <EEPROM.h>
#elif defined(ARDUINO_ARCH_STM32)
  #include "ADS1X15.h"
  #include <FlashStorage_STM32.h>
#endif
#include <EasyNextionLibrary.h>
#if defined(MAX31855_ENABLED)
  #include <Adafruit_MAX31855.h>
#else
  #include <max6675.h>
#endif
#if defined(SINGLE_HX711_CLOCK)
  #include <HX711_2.h>
#else
  #include <HX711.h>
#endif
#include <PSM.h>
#include "PressureProfile.h"

#if defined(ARDUINO_ARCH_AVR)
  // ATMega32P pins definitions
  #define zcPin 2
  #define thermoDO 4
  #define thermoCS 5
  #define thermoCLK 6
  #define steamPin 7
  #define relayPin 8  // PB0
  #define dimmerPin 9
  #define valvePin 3
  #define brewPin A0 // PD7
  #define pressurePin A1
  #define HX711_dout_1 12 //mcu > HX711 no 1 dout pin
  #define HX711_dout_2 13 //mcu > HX711 no 2 dout pin
  #define HX711_sck_1 10 //mcu > HX711 no 1 sck pin
  #define HX711_sck_2 11 //mcu > HX711 no 2 sck pin
  #define USART_CH Serial

  #if defined(TIMERINTERRUPT_ENABLED)
    // configuration for TimerInterruptGeneric
    #define TIMER_INTERRUPT_DEBUG 0
    #define _TIMERINTERRUPT_LOGLEVEL_ 0

    #define USING_16MHZ true
    #define USING_8MHZ false
    #define USING_250KHZ false

    #define USE_TIMER_0 false
    #define USE_TIMER_1 true
    #define USE_TIMER_2 false
    #define USE_TIMER_3 false

    #include <TimerInterrupt_Generic.h>

    void initPressure(int);
  #endif

#elif defined(ARDUINO_ARCH_STM32)// if arch is stm32
  // STM32F4 pins definitions
  #define zcPin PA15
  #define thermoDO PA5 //PB4
  #define thermoCS PA6 //PB5
  #define thermoCLK PA7 //PB6
  #define brewPin PA11 // PD7
  #define relayPin PB9  // PB0
  #define dimmerPin PB3
  #define valvePin PC15
  #define pressurePin ADS115_A0 //set here just for reference
  #define steamPin PA12
  #define HX711_sck_1 PB0 //mcu > HX711 no 1 sck pin
  #define HX711_sck_2 PB1 //mcu > HX711 no 2 sck pin
  #define HX711_dout_1 PA1 //mcu > HX711 no 1 dout pin
  #define HX711_dout_2 PA2 //mcu > HX711 no 2 dout pin
  #define USART_CH Serial1
  //#define // USART_CH1 Serial
#endif


// Define some const values
#define GET_KTYPE_READ_EVERY    250 // thermocouple data read interval not recommended to be changed to lower than 250 (ms)
#define GET_PRESSURE_READ_EVERY 6
#define GET_SCALES_READ_EVERY   100
#define REFRESH_SCREEN_EVERY    150 // Screen refresh interval (ms)
#define DESCALE_PHASE1_EVERY    60000 // short pump pulses during descale
#define DESCALE_PHASE2_EVERY    120000 // long pause for scale softening
#define DESCALE_PHASE3_EVERY    4000 // short pause for pulse effficience activation
#define MAX_SETPOINT_VALUE      110 //Defines the max value of the setpoint
#define EEPROM_RESET            1 //change this value if want to reset to defaults
#define PUMP_RANGE              127 //how often dimmer interrupts AC cycle **maybe**
#define DELTA_RANGE             0.25f // % to apply as delta
#if defined(ARDUINO_ARCH_AVR)
  #define ZC_MODE FALLING
#elif defined(ARDUINO_ARCH_STM32)
  #define ZC_MODE RISING
#endif

#if defined(ARDUINO_ARCH_STM32)// if arch is stm32
//If additional USART ports want ti eb used thy should be enable first
//HardwareSerial USART_CH(PA10, PA9);
ADS1115 ADS(0x48);
#endif
//Init the thermocouples with the appropriate pins defined above with the prefix "thermo"
#if defined(ADAFRUIT_MAX31855_H)
  Adafruit_MAX31855 thermocouple(thermoCLK, thermoCS, thermoDO);
#else
  MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);
#endif
// EasyNextion object init
EasyNex myNex(USART_CH);
//Banoz PSM - for more cool shit visit https://github.com/banoz  and don't forget to star
PSM pump(zcPin, dimmerPin, PUMP_RANGE, ZC_MODE, 2);
//#######################__HX711_stuff__##################################
#if defined(SINGLE_HX711_CLOCK)
HX711_2 LoadCells;
#else
HX711 LoadCell_1; //HX711 1
HX711 LoadCell_2; //HX711 2
#endif


// Some vars are better global
//Timers
unsigned long stopTimeSec;
unsigned long pressureTimer = millis();
unsigned long thermoTimer = millis();
unsigned long scalesTimer = millis();
unsigned long flowTimer = millis();
unsigned long pageRefreshTimer = millis();
unsigned long brewingTimer = millis();
unsigned long activeBrewingStart = 4294967295; // max value so timer only updates after start defined
//volatile vars
volatile float kProbeReadValue; //temp val
volatile float livePressure;
volatile float liveWeight;

//scales vars
/* If building for STM32 define the scales factors here */
float scalesF1 = -4183.14f; // 3,911.142856
float scalesF2 = 3911.14f; // -4,183.142856
float currentWeight;
float previousWeight;
float flowVal;

//int tarcalculateWeight;
bool weighingStartRequested;
bool scalesPresent;
bool tareDone;

// brew detection vars
bool brewActive;
bool brewTimerActive; // active if brewing or descaling
bool previousBrewState;

//PP&PI variables
//default phases. Updated in updatePressureProfilePhases.
Phase phaseArray[] = {
  Phase{1, 2, 6000},
  Phase{2, 2, 6000},
  Phase{0, 0, 2000},
  Phase{9, 9, 500},
  Phase{9, 6, 40000}
};
Phases phases {5,  phaseArray};
int preInfusionFinishedPhaseIdx = 3;

bool preinfusionFinished;

bool POWER_ON;
bool  descaleCheckBox;
bool  preinfusionState;
bool  pressureProfileState;
bool  warmupEnabled;
bool  flushEnabled;
bool  descaleEnabled;
bool brewDeltaActive;
bool homeScreenScalesEnabled;
volatile int  HPWR;
volatile int  HPWR_OUT;
int  setPoint;
int  offsetTemp;
int  MainCycleDivider;
int  BrewCycleDivider;
int  preinfuseTime;
int preinfuseBar;
int preinfuseSoak;
int ppStartBar;
int ppFinishBar;
int ppHold;
int ppLength;
int selectedOperationalMode;
int regionHz;

// Other util vars
float pressureTargetComparator;

// EEPROM  stuff
#define  EEP_SIG                 0
#define  EEP_SETPOINT            10
#define  EEP_OFFSET              20
#define  EEP_HPWR                40
#define  EEP_M_DIVIDER           60
#define  EEP_B_DIVIDER           80
#define  EEP_P_START             100
#define  EEP_P_FINISH            120
#define  EEP_P_HOLD              110
#define  EEP_P_LENGTH            130
#define  EEP_PREINFUSION         140
#define  EEP_P_PROFILE           160
#define  EEP_PREINFUSION_SEC     180
#define  EEP_PREINFUSION_BAR     190
#define  EEP_PREINFUSION_SOAK    170
#define  EEP_REGPWR_HZ           195
#define  EEP_WARMUP              200
#define  EEP_HOME_ON_SHOT_FINISH 205
#define  EEP_GRAPH_BREW          210
#define  EEP_BREW_DELTA          225
#define  EEP_SCALES_F1           215
#define  EEP_SCALES_F2           220
#define  EEP_STOPTIME_SEC        230

void setup() {
  // USART_CH1.begin(115200); //debug channel
  USART_CH.begin(115200); // LCD comms channel

  // Various pins operation mode handling
  pinInit();

  // init the exteranl ADC
  ads1115Init();

  // Debug init if enabled
  dbgInit();

  // Turn off boiler in case init is unsecessful
  setBoiler(LOW);  // relayPin LOW

  //Pump
  pump.set(0);

  digitalWrite(valvePin, LOW);

  // USART_CH1.println("Init step 4");
  // Will wait hereuntil full serial is established, this is done so the LCD fully initializes before passing the EEPROM values
  while (myNex.readNumber("safetyTempCheck") != 100 )
  {
    delay(100);
  }

  // USART_CH1.println("Init step 5");
  // Initialising the vsaved values or writing defaults if first start
  eepromInit();

  #if defined(ARDUINO_ARCH_AVR) && defined(TIMERINTERRUPT_GENERIC_H)
    initPressure(myNex.readNumber("regHz"));
  #endif

  #if defined(ADAFRUIT_MAX31855_H)
    thermocouple.begin();
  #endif

  // Scales handling
  scalesInit();
  myNex.lastCurrentPageId = myNex.currentPageId;
  POWER_ON = true;

  // USART_CH1.println("Init step 6");
}

//##############################################################################################################################
//############################################________________MAIN______________################################################
//##############################################################################################################################


//Main loop where all the logic is continuously run
void loop() {
  pageValuesRefresh();
  myNex.NextionListen();
  sensorsRead();
  brewDetect();
  modeSelect();
  lcdRefresh();
}

//##############################################################################################################################
//#############################################___________SENSORS_READ________##################################################
//##############################################################################################################################


void sensorsRead() { // Reading the thermocouple temperature
  // static long thermoTimer;
  // Reading the temperature every 350ms between the loops
  if (millis() > thermoTimer) {
    kProbeReadValue = thermocouple.readCelsius();  // Making sure we're getting a value
    /*
    This *while* is here to prevent situations where the system failed to get a temp reading and temp reads as 0 or -7(cause of the offset)
    If we would use a non blocking function then the system would keep the SSR in HIGH mode which would most definitely cause boiler overheating
    */
    while (kProbeReadValue <= 0.0f || kProbeReadValue == NAN || kProbeReadValue > 165.0f) {
      /* In the event of the temp failing to read while the SSR is HIGH
      we force set it to LOW while trying to get a temp reading - IMPORTANT safety feature */
      setBoiler(LOW);
      if (millis() > thermoTimer) {
        kProbeReadValue = thermocouple.readCelsius();  // Making sure we're getting a value
        thermoTimer = millis() + GET_KTYPE_READ_EVERY;
      }
    }
    thermoTimer = millis() + GET_KTYPE_READ_EVERY;
  }

  // Read pressure and store in a global var for further controls
  #if defined(TIMERINTERRUPT_GENERIC_H)
    livePressure = getPressure();
  #else
    if (millis() > pressureTimer) {
      livePressure = getPressure();
      pressureTimer = millis() + GET_PRESSURE_READ_EVERY;
    }
  #endif
}

void calculateWeight() {
  // static long scalesTimer;

  scalesTare(); //Tare at the start of any weighing cycle

  // Weight output
  if (millis() > scalesTimer) {
    if (scalesPresent && weighingStartRequested) {
      // Stop pump to prevent HX711 critical section from breaking timing
      //pump.set(0);
      #if defined(SINGLE_HX711_CLOCK)
        if (LoadCells.is_ready()) {
          float values[2];
          LoadCells.get_units(values);
          currentWeight = values[0] + values[1];
        }
      #else
        currentWeight = LoadCell_1.get_units() + LoadCell_2.get_units();
      #endif
      // Resume pumping
      //pump.set(pumpValue);
    }
    scalesTimer = millis() + GET_SCALES_READ_EVERY;
  }
  calculateFlow();
}

void calculateFlow() {
  // static long refreshTimer;

  if (millis() >= flowTimer) {
    flowVal = (currentWeight - previousWeight)*10;
    previousWeight = currentWeight;
    flowTimer = millis() + 1000;
  }
}

//##############################################################################################################################
//############################################______PRESSURE_____TRANSDUCER_____################################################
//##############################################################################################################################
#if defined(ARDUINO_ARCH_AVR) && defined(TIMERINTERRUPT_GENERIC_H)
  volatile int presData[2];
  volatile char presIndex = 0;

  void presISR() {
    presData[presIndex] = ADCW;
    presIndex ^= 1;
  }

  void initPressure(int hz) {
    int pin = pressurePin - 14;
    ADMUX = (DEFAULT << 6) | (pin & 0x07);
    ADCSRB = (1 << ACME);
    ADCSRA = (1 << ADEN) | (1 << ADSC) | (1 << ADATE) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

    ITimer1.init();
    ITimer1.attachInterrupt(hz * 2, presISR);
  }
#endif

float getPressure() {  //returns sensor pressure data
    // 5V/1024 = 1/204.8 (10 bit) or 6553.6 (15 bit)
    // voltageZero = 0.5V --> 102.4(10 bit) or 3276.8 (15 bit)
    // voltageMax = 4.5V --> 921.6 (10 bit) or 29491.2 (15 bit)
    // range 921.6 - 102.4 = 819.2 or 26214.4
    // pressure gauge range 0-1.2MPa - 0-12 bar
    // 1 bar = 68.27 or 2184.5

    #if defined(ARDUINO_ARCH_AVR)
      #if defined(TIMERINTERRUPT_GENERIC_H)
        return (presData[0] + presData[1]) / 39.79f - 6.17f;
      #else
        return analogRead(pressurePin) / 19.9F - 6.2F;
      #endif
    #elif defined(ARDUINO_ARCH_STM32)
      return ADS.getValue() / 1706.6f - 1.49f;
    #endif
}

void setPressure(float targetValue) {
  int pumpValue;
  float diff = targetValue - livePressure;

  if (targetValue == 0 || livePressure > targetValue) {
    pumpValue = 0;
  } else {
    float diff = targetValue - livePressure;
    pumpValue = PUMP_RANGE / (1.f + exp(1.7f - diff/0.9f));
  }

  pump.set(pumpValue);
}

//##############################################################################################################################
//############################################______PAGE_CHANGE_VALUES_REFRESH_____#############################################
//##############################################################################################################################

void pageValuesRefresh() {  // Refreshing our values on page changes

  if ( myNex.currentPageId != myNex.lastCurrentPageId || POWER_ON == true ) {
    preinfusionState        = myNex.readNumber("piState"); // reding the preinfusion state value which should be 0 or 1
    pressureProfileState    = myNex.readNumber("ppState"); // reding the pressure profile state value which should be 0 or 1
    preinfuseTime           = myNex.readNumber("piSec");
    preinfuseBar            = myNex.readNumber("piBar");
    preinfuseSoak           = myNex.readNumber("piSoak"); // pre-infusion soak value
    ppStartBar              = myNex.readNumber("ppStart");
    ppFinishBar             = myNex.readNumber("ppFin");
    ppHold                  = myNex.readNumber("ppHold"); // pp start pressure hold
    ppLength                = myNex.readNumber("ppLength"); // pp shot length
    brewDeltaActive         = myNex.readNumber("deltaState");
    flushEnabled            = myNex.readNumber("flushState");
    descaleEnabled          = myNex.readNumber("descaleState");
    setPoint                = myNex.readNumber("setPoint");  // reading the setPoint value from the lcd
    offsetTemp              = myNex.readNumber("offSet");  // reading the offset value from the lcd
    HPWR                    = myNex.readNumber("hpwr");  // reading the brew time delay used to apply heating in waves
    MainCycleDivider        = myNex.readNumber("mDiv");  // reading the delay divider
    BrewCycleDivider        = myNex.readNumber("bDiv");  // reading the delay divider
    regionHz                = myNex.readNumber("regHz");
    warmupEnabled           = myNex.readNumber("warmupState");
    homeScreenScalesEnabled = myNex.readNumber("scalesEnabled");
    stopTimeSec             = myNex.readNumber("stopTime");

    // MODE_SELECT should always be last
    selectedOperationalMode = myNex.readNumber("modeSelect");

    updatePressureProfilePhases();

    myNex.lastCurrentPageId = myNex.currentPageId;
    POWER_ON = false;
  }
}

//#############################################################################################
//############################____OPERATIONAL_MODE_CONTROL____#################################
//#############################################################################################
void modeSelect() {
  // USART_CH1.println("MODE SELECT ENTER");
  switch (selectedOperationalMode) {
    case 0:
    case 1:
    case 2:
    case 4:
      if (!steamState()) newPressureProfile();
      else steamCtrl();
      break;
    case 3:
      // USART_CH1.println("MODE SELECT 3");
      manualPressureProfile();
      break;
    case 5:
      // USART_CH1.println("MODE SELECT 5");
      if (!steamState() ) justDoCoffee();
      else steamCtrl();
      break;
    case 6:
      // USART_CH1.println("MODE SELECT 6");
      deScale();
      break;
    case 7:
      // USART_CH1.println("MODE SELECT 7");
      break;
    case 8:
      // USART_CH1.println("MODE SELECT 8");
      break;
    case 9:
      // USART_CH1.println("MODE SELECT 9");
      if (!steamState() ) justDoCoffee();
      else steamCtrl();
      break;
    default:
      POWER_ON = true;
      pageValuesRefresh();
      break;
  }
  // USART_CH1.println("MODE SELECT EXIT");
}

//#############################################################################################
//#########################____NO_OPTIONS_ENABLED_POWER_CONTROL____############################
//#############################################################################################

//delta stuff
inline static float TEMP_DELTA(float d) { return (d*DELTA_RANGE); }

void justDoCoffee() {
  // USART_CH1.println("DO_COFFEE ENTER");
  int HPWR_LOW = HPWR/MainCycleDivider;
  static double heaterWave;
  static bool heaterState;
  float BREW_TEMP_DELTA;
  // Calculating the boiler heating power range based on the below input values
  int HPWR_OUT = mapRange(kProbeReadValue, setPoint - 10, setPoint, HPWR, HPWR_LOW, 0);
  HPWR_OUT = constrain(HPWR_OUT, HPWR_LOW, HPWR);  // limits range of sensor values to HPWR_LOW and HPWR
  BREW_TEMP_DELTA = mapRange(kProbeReadValue, setPoint, setPoint + TEMP_DELTA(setPoint), TEMP_DELTA(setPoint), 0, 0);
  BREW_TEMP_DELTA = constrain(BREW_TEMP_DELTA, 0, TEMP_DELTA(setPoint));

  // USART_CH1.println("DO_COFFEE TEMP CTRL BEGIN");
  if (brewActive) {
  // Applying the HPWR_OUT variable as part of the relay switching logic
    if (kProbeReadValue > setPoint-1.5f && kProbeReadValue < setPoint+0.25f && !preinfusionFinished ) {
      if (millis() - heaterWave > HPWR_OUT*BrewCycleDivider && !heaterState ) {
        setBoiler(LOW);
        heaterState=true;
        heaterWave=millis();
      } else if (millis() - heaterWave > HPWR_LOW*MainCycleDivider && heaterState ) {
        setBoiler(HIGH);
        heaterState=false;
        heaterWave=millis();
      }
    } else if (kProbeReadValue > setPoint-1.5f && kProbeReadValue < setPoint+(brewDeltaActive ? BREW_TEMP_DELTA : 0.f) && preinfusionFinished ) {
      if (millis() - heaterWave > HPWR*BrewCycleDivider && !heaterState ) {
        setBoiler(HIGH);
        heaterState=true;
        heaterWave=millis();
      } else if (millis() - heaterWave > HPWR && heaterState ) {
        setBoiler(LOW);
        heaterState=false;
        heaterWave=millis();
      }
    } else if (brewDeltaActive && kProbeReadValue >= (setPoint+BREW_TEMP_DELTA) && kProbeReadValue <= (setPoint+BREW_TEMP_DELTA+2.5f)  && preinfusionFinished ) {
      if (millis() - heaterWave > HPWR*MainCycleDivider && !heaterState ) {
        setBoiler(HIGH);
        heaterState=true;
        heaterWave=millis();
      } else if (millis() - heaterWave > HPWR && heaterState ) {
        setBoiler(LOW);
        heaterState=false;
        heaterWave=millis();
      }
    } else if(kProbeReadValue <= setPoint-1.5f) {
    setBoiler(HIGH);
    } else {
      setBoiler(LOW);
    }
  } else { //if brewState == 0
    if (kProbeReadValue < ((float)setPoint - 10.00f)) {
      setBoiler(HIGH);
    } else if (kProbeReadValue >= ((float)setPoint - 10.f) && kProbeReadValue < ((float)setPoint - 5.f)) {
      if (millis() - heaterWave > HPWR_OUT && !heaterState) {
        setBoiler(HIGH);
        heaterState=true;
        heaterWave=millis();
      } else if (millis() - heaterWave > HPWR_OUT / BrewCycleDivider && heaterState ) {
        setBoiler(LOW);
        heaterState=false;
        heaterWave=millis();
      }
    } else if ((kProbeReadValue >= ((float)setPoint - 5.f)) && (kProbeReadValue <= ((float)setPoint - 0.25f))) {
      if (millis() - heaterWave > HPWR_OUT * BrewCycleDivider && !heaterState) {
        setBoiler(HIGH);
        heaterState=!heaterState;
        heaterWave=millis();
      } else if (millis() - heaterWave > HPWR_OUT / BrewCycleDivider && heaterState ) {
        setBoiler(LOW);
        heaterState=!heaterState;
        heaterWave=millis();
      }
    } else {
      setBoiler(LOW);
    }
  }
}

//#############################################################################################
//################################____STEAM_POWER_CONTROL____##################################
//#############################################################################################

void steamCtrl() {

  if (!brewActive) {
    if (livePressure <= 9.f) { // steam temp control, needs to be aggressive to keep steam pressure acceptable
      if ((kProbeReadValue > setPoint-10.f) && (kProbeReadValue <= 155.f)) setBoiler(HIGH);
      else setBoiler(LOW);
    } else if(livePressure >= 9.1f) setBoiler(LOW);
  } else if (brewActive) { //added to cater for hot water from steam wand functionality
    if ((kProbeReadValue > setPoint-10.f) && (kProbeReadValue <= 105.f)) {
      setBoiler(HIGH);
      setPressure(9);
    } else {
      setBoiler(LOW);
      setPressure(9);
    }
  } else setBoiler(LOW);
}

//#############################################################################################
//################################____LCD_REFRESH_CONTROL___###################################
//#############################################################################################

void lcdRefresh() {
  // static long pageRefreshTimer;
  static float shotWeight;

  if (millis() > pageRefreshTimer) {
    /*LCD brew timer output*/
    if (brewTimerActive) myNex.writeNum("activeBrewTime", (millis() > activeBrewingStart) ? (int)((millis() - activeBrewingStart) / 1000) : 0);
    /*LCD pressure output, as a measure to beautify the graphs locking the live pressure read for the LCD alone*/
    // if (brewActive) myNex.writeNum("pressure.val", (livePressure > 0.f) ? livePressure*10.f : 0.f);
    if (brewActive) myNex.writeNum("pressure.val", (livePressure > 0.f) ? (livePressure <= pressureTargetComparator + 0.5f) ? livePressure*10.f : pressureTargetComparator*10.f : 0.f);
    else myNex.writeNum("pressure.val", (livePressure > 0.f) ? livePressure*10.f : 0.f);
    /*LCD temp output*/
    myNex.writeNum("currentTemp",int(kProbeReadValue-offsetTemp));
    /*LCD weight output*/
    if (weighingStartRequested && brewActive) {
      (currentWeight > 0.f) ? myNex.writeStr("weight.txt",String(currentWeight,1)) : myNex.writeStr("weight.txt", "0.0");
      shotWeight = currentWeight;
    } else if (weighingStartRequested && !brewActive) {
      if (myNex.currentPageId != 0 && !homeScreenScalesEnabled) myNex.writeStr("weight.txt",String(shotWeight,1));
      else if(myNex.currentPageId == 0 && homeScreenScalesEnabled) (currentWeight > 0.f) ? myNex.writeStr("weight.txt",String(currentWeight,1)) : myNex.writeStr("weight.txt", "0.0");
    }
    /*LCD flow output*/
    if (weighingStartRequested) (flowVal>0.f) ? myNex.writeNum("flow.val", int(flowVal)) : myNex.writeNum("flow.val", 0.f);

    dbgOutput();

    pageRefreshTimer = millis() + REFRESH_SCREEN_EVERY;
  }
}
//#############################################################################################
//###################################____SAVE_BUTTON____#######################################
//#############################################################################################
// Save the desired temp values to EEPROM
void trigger1() {
  #if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_STM32)
    int valueToSave;
    int allValuesUpdated;

    switch (myNex.currentPageId){            
      case 1:        
        break;
      case 2:
        break;
      case 3:        
        // Saving stopTime
        valueToSave = myNex.readNumber("stopTime");
        if (valueToSave >= 0 ) {
          EEPROM.put(EEP_STOPTIME_SEC, valueToSave);
          allValuesUpdated++;
        } 
        // Saving ppStart,ppFin,ppHold and ppLength
        valueToSave = myNex.readNumber("ppStart");
        if (valueToSave >= 0 ) {
          EEPROM.put(EEP_P_START, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("ppFin");
        if (valueToSave >= 0 ) {
          EEPROM.put(EEP_P_FINISH, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("ppHold");
        if (valueToSave >= 0) {
          EEPROM.put(EEP_P_HOLD, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("ppLength");
        if (valueToSave >= 0) {
          EEPROM.put(EEP_P_LENGTH, valueToSave);
          allValuesUpdated++;
        }
        // Saving PI and PP
        valueToSave = myNex.readNumber("piState");
        if (valueToSave == 0 || valueToSave == 1 ) {
          EEPROM.put(EEP_PREINFUSION, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("ppState");
        if (valueToSave == 0 || valueToSave == 1 ) {
          EEPROM.put(EEP_P_PROFILE, valueToSave);
          allValuesUpdated++;
        }
        //Saved piSec
        valueToSave = myNex.readNumber("piSec");
        if ( valueToSave >= 0 ) {
          EEPROM.put(EEP_PREINFUSION_SEC, valueToSave);
          allValuesUpdated++;
        }
        //Saved piBar
        valueToSave = myNex.readNumber("piBar");
        if ( valueToSave >= 0 ) {
          EEPROM.put(EEP_PREINFUSION_BAR, valueToSave);
          allValuesUpdated++;
        }
        //Saved piSoak
        valueToSave = myNex.readNumber("piSoak");
        if ( valueToSave >= 0 ) {
          EEPROM.put(EEP_PREINFUSION_SOAK, valueToSave);
          allValuesUpdated++;
        }
        if (allValuesUpdated == 10) {
          allValuesUpdated=0;
          myNex.writeStr("popupMSG.t0.txt","UPDATE SUCCESSFUL!");
        } else myNex.writeStr("popupMSG.t0.txt","ERROR!");
        myNex.writeStr("page popupMSG");
        break;
      case 4:
        //Saving brewSettings
        valueToSave = myNex.readNumber("homeOnBrewFinish");
        if ( valueToSave >= 0 ) {
          EEPROM.put(EEP_HOME_ON_SHOT_FINISH, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("graphEnabled");
        if ( valueToSave >= 0 ) {
          EEPROM.put(EEP_GRAPH_BREW, valueToSave);
          allValuesUpdated++;
        }
        valueToSave = myNex.readNumber("deltaState");
        if ( valueToSave == 0 || valueToSave == 1 ) {
          EEPROM.put(EEP_BREW_DELTA, valueToSave);
          allValuesUpdated++;
        }
        if (allValuesUpdated == 3) {
          allValuesUpdated=0;
          myNex.writeStr("popupMSG.t0.txt","UPDATE SUCCESSFUL!");
        } else myNex.writeStr("popupMSG.t0.txt","ERROR!");
        myNex.writeStr("page popupMSG");
        break;
      case 5:
        break;
      case 6:
        // Reading the LCD side set values
        valueToSave = myNex.readNumber("setPoint");
        if ( valueToSave > 0) {
        EEPROM.put(EEP_SETPOINT, valueToSave);
          allValuesUpdated++;
        }
        // Saving offset
        valueToSave = myNex.readNumber("offSet");
        if ( valueToSave >= 0 ) {
        EEPROM.put(EEP_OFFSET, valueToSave);
          allValuesUpdated++;
        }
        // Saving HPWR
        valueToSave = myNex.readNumber("hpwr");
        if ( valueToSave >= 0 ) {
        EEPROM.put(EEP_HPWR, valueToSave);
          allValuesUpdated++;
        }
        // Saving mDiv
        valueToSave = myNex.readNumber("mDiv");
        if ( valueToSave >= 1) {
        EEPROM.put(EEP_M_DIVIDER, valueToSave);
          allValuesUpdated++;
        }
        //Saving bDiv
        valueToSave = myNex.readNumber("bDiv");
        if ( valueToSave >= 1) {
        EEPROM.put(EEP_B_DIVIDER, valueToSave);
          allValuesUpdated++;
        }
        if (allValuesUpdated == 5) {
          allValuesUpdated=0;
          myNex.writeStr("popupMSG.t0.txt","UPDATE SUCCESSFUL!");
        } else myNex.writeStr("popupMSG.t0.txt","ERROR!");
        myNex.writeStr("page popupMSG");
        break;
      case 7:
        valueToSave = myNex.readNumber("regHz");
        if ( valueToSave == 50 || valueToSave == 60 ) {
        EEPROM.put(EEP_REGPWR_HZ, valueToSave);
          allValuesUpdated++;
        }
        // Saving warmup state
        valueToSave = myNex.readNumber("warmupState");
        if (valueToSave == 0 || valueToSave == 1 ) {
        EEPROM.put(EEP_WARMUP, valueToSave);
          allValuesUpdated++;
        }
        if (allValuesUpdated == 2) {
          allValuesUpdated=0;
          myNex.writeStr("popupMSG.t0.txt","UPDATE SUCCESSFUL!");
        } else myNex.writeStr("popupMSG.t0.txt","ERROR!");
        myNex.writeStr("page popupMSG");
        break;
      default:
        break;
    }
  #endif
}

//#############################################################################################
//###################################_____SCALES_TARE____######################################
//#############################################################################################

void trigger2() {
  tareDone = false;
  previousBrewState = false;
  scalesTare();
}

void trigger3() {
  homeScreenScalesEnabled = myNex.readNumber("scalesEnabled");
}

//#############################################################################################
//###############################_____HELPER_FUCTIONS____######################################
//#############################################################################################

//Function to get the state of the brew switch button
bool brewState() {
  return digitalRead(brewPin) == LOW; // pin will be low when switch is ON.
}

//Function to get the state of the steam switch button
bool steamState() {
  return digitalRead(steamPin) == LOW; // pin will be low when switch is ON.
}

void brewTimer(bool start) { // small function for easier brew timer start/stop
  if (!brewTimerActive && start) {
    myNex.writeNum("activeBrewTime", 0);
    myNex.writeNum("timerState", 1);
    activeBrewingStart = millis();
    brewTimerActive = true;
  } else if (!start) {
    brewTimerActive = false;
    myNex.writeNum("timerState", 0);
    activeBrewingStart = 4294967295;
  } 
}

// Actuating the heater element
void setBoiler(int val) {
  // USART_CH1.println("SET_BOILER BEGIN");
  #if defined(ARDUINO_ARCH_AVR)
  // USART_CH1.println("SET_BOILER AVR BLOCK BEGIN");
    if (val == HIGH) {
      PORTB |= _BV(PB0);  // boilerPin -> HIGH
    } else {
      PORTB &= ~_BV(PB0);  // boilerPin -> LOW
    }
  // USART_CH1.println("SET_BOILER AVR BLOCK END");
  #elif defined(ARDUINO_ARCH_STM32)// if arch is stm32
  // USART_CH1.println("SET_BOILER STM32 BLOCK BEGIN");
    if (val == HIGH) {
      digitalWrite(relayPin, HIGH);  // boilerPin -> HIGH
    } else {
      digitalWrite(relayPin, LOW);   // boilerPin -> LOW
    }
  // USART_CH1.println("SET_BOILER STM32 BLOCK END");
  #endif
  // USART_CH1.println("SET_BOILER END");
}

//#############################################################################################
//###############################____DESCALE__CONTROL____######################################
//#############################################################################################

void deScale() {
  static bool blink = true;
  static long timer = 0;
  static int currentCycleRead = 0;
  static int lastCycleRead = 0;
  static bool descaleFinished = false;
  if (brewActive && !descaleFinished) {
    if (currentCycleRead < lastCycleRead) { // descale in cycles for 5 times then wait according to the below condition
      if (blink == true) { // Logic that switches between modes depending on the $blink value
        pump.set(10);
        if (millis() - timer > DESCALE_PHASE1_EVERY) { // 60 sec
          blink = false;
          currentCycleRead += 2;
          timer = millis();
        }
      } else {
        pump.set(0);
        if (millis() - timer > DESCALE_PHASE2_EVERY) { // nothing for 120 sec
          blink = true;
          currentCycleRead += 4;
          timer = millis();
        }
      }
    } else {
      pump.set(30);
      if ((millis() - timer) > DESCALE_PHASE3_EVERY) { //set dimmer power to max descale value for 4 sec
        solenoidBeat();
        blink = true;
        currentCycleRead += 4; // need an overflow on 3rd cycle (34 -> 68 -> 102)
        lastCycleRead += 33;
        timer = millis();
      }
    }

    if (currentCycleRead >= 100) {
      descaleFinished = true;
    }

    if (millis() > pageRefreshTimer) {
      if (currentCycleRead < 100) {
        myNex.writeNum("j0.val", currentCycleRead);
      } else {
        myNex.writeNum("j0.val", 100);
      }
    }
  } else if (brewActive && descaleFinished) {
    pump.set(0);
    if ((millis() - timer) > 1000) {
      brewTimer(0);
      myNex.writeStr("t14.txt", "FINISHED!");
      timer = millis();
    }
  } else {
    pump.set(0);
    blink = true;
    currentCycleRead = 0;
    lastCycleRead = 30;
    descaleFinished = false;
    timer = millis();
  }
  //keeping it at temp
  justDoCoffee();
}

void solenoidBeat() {
  pump.set(PUMP_RANGE);
  digitalWrite(valvePin, LOW);
  delay(200);
  digitalWrite(valvePin, HIGH);
  delay(200);
  digitalWrite(valvePin, LOW);
  delay(200);
  digitalWrite(valvePin, HIGH);
  delay(200);
  digitalWrite(valvePin, LOW);
  delay(200);
  digitalWrite(valvePin, HIGH);
  pump.set(0);
}

//#############################################################################################
//###############################____PRESSURE_CONTROL____######################################
//#############################################################################################
void updatePressureProfilePhases() {
  switch (selectedOperationalMode)
  {
  case 0: // no PI and no PP -> Pressure fixed at 9bar
    phases.count = 1;
    setPhase(0, 5, 5, 1000);
    preInfusionFinishedPhaseIdx = 0;
    break;
  case 1: // Just PI no PP -> after PI pressure fixed at 9bar
    phases.count = 4;
    setPreInfusionPhases(0, preinfuseBar, preinfuseTime, preinfuseSoak);
    setPhase(3, 9, 9, 1000);
    preInfusionFinishedPhaseIdx = 3;
    break;
  case 2: // No PI, yes PP
    phases.count = 2;
    setPresureProfilePhases(0, ppStartBar, ppFinishBar, ppHold, ppLength);
    preInfusionFinishedPhaseIdx = 0;
    break;
  case 4: // Both PI + PP
    phases.count = 5;
    setPreInfusionPhases(0, preinfuseBar, preinfuseTime, preinfuseSoak);
    setPresureProfilePhases(3, ppStartBar, ppFinishBar, ppHold, ppLength);
    preInfusionFinishedPhaseIdx = 3;
    break;
  default:
    break;
  }
}

void setPreInfusionPhases(int startingIdx, int piBar, int piTime, int piSoakTime) {
    setPhase(startingIdx + 0, piBar/2, piBar, piTime * 1000 / 2);
    setPhase(startingIdx + 1, piBar, piBar, piTime * 1000 / 2);
    setPhase(startingIdx + 2, 0, 0, piSoakTime * 1000);
}

void setPresureProfilePhases(int startingIdx,int fromBar, int toBar, int holdTime, int dropTime) {
    setPhase(startingIdx + 0, fromBar, fromBar, holdTime * 1000);
    setPhase(startingIdx + 1, fromBar, toBar, dropTime * 1000);
}

void setPhase(int phaseIdx, int fromBar, int toBar, int timeMs) {
    phases.phases[phaseIdx].startPressure = fromBar;
    phases.phases[phaseIdx].endPressure = toBar;
    phases.phases[phaseIdx].durationMs = timeMs;
}

void newPressureProfile() {
  float newBarValue;

  if (brewActive) { //runs this only when brew button activated and pressure profile selected
    long timeInPP = millis() - brewingTimer;
    CurrentPhase currentPhase = phases.getCurrentPhase(timeInPP);
    newBarValue = phases.phases[currentPhase.phaseIndex].getPressure(currentPhase.timeInPhase);
    preinfusionFinished = currentPhase.phaseIndex >= preInfusionFinishedPhaseIdx;
  }
  else {
    newBarValue = 0.0f;
  }
  setPressure(newBarValue);
  // saving the target pressure
  pressureTargetComparator = preinfusionFinished ? (int) newBarValue : livePressure;
  // Keep that water at temp
  justDoCoffee();
}

void manualPressureProfile() {
  if( selectedOperationalMode == 3 ) {
    int power_reading = myNex.readNumber("h0.val");
    setPressure(power_reading);
  }
  justDoCoffee();
}

//#############################################################################################
//###############################____INIT_AND_ADMIN_CTRL____###################################
//#############################################################################################
static bool stopped_on_time = false;

void brewDetect() {
  if ( brewState() ) // switch is on
  {
    if( brewActive && millis() - activeBrewingStart > stopTimeSec*1000UL)
    {
        // brewing but  time is up
        stopped_on_time = true;
    }
  }
  else // switch is off
  {
    stopped_on_time = false;
  }

   if ( brewState() && !stopped_on_time){
    digitalWrite(valvePin, HIGH);
    /* Applying the below block only when brew detected */
    if (selectedOperationalMode == 0 || selectedOperationalMode == 1 || selectedOperationalMode == 2 || selectedOperationalMode == 3 || selectedOperationalMode == 4) {
      weighingStartRequested = true; // Flagging weighing start
      if (!brewActive) {
        myNex.writeNum("warmupState", 0); // Flaggig warmup notification on Nextion needs to stop (if enabled)
      }
      if (myNex.currentPageId == 1 || myNex.currentPageId == 2 || myNex.currentPageId == 8 || homeScreenScalesEnabled ) calculateWeight();
    } else if (selectedOperationalMode == 5 || selectedOperationalMode == 9) {
       pump.set(PUMP_RANGE); // setting the pump output target to 9 bars for non PP or PI profiles
    }
    if (!brewActive) {
      brewTimer(true); // nextion timer start
    }
    brewActive = true;
  } else {
    digitalWrite(valvePin, LOW);
    brewTimer(false);
    pump.set(0);
    brewActive = false;
    /* UPDATE VARIOUS INTRASHOT TIMERS and VARS */
    brewingTimer = millis();
    /* Only resetting the brew activity value if it's been previously set */
    preinfusionFinished = false;
    if (myNex.currentPageId == 1 || myNex.currentPageId == 2 || myNex.currentPageId == 8 || homeScreenScalesEnabled ) {
      /* Only setting the weight activity value if it's been previously unset */
      weighingStartRequested=true;
      calculateWeight();
    } else {/* Only resetting the scales values if on any other screens than brew or scales */
      weighingStartRequested = false; // Flagging weighing stop
      tareDone = false;
      previousBrewState = false;
      currentWeight = 0.f;
      previousWeight = 0.f;
    }
  }
}

void scalesInit() {

  #if defined(SINGLE_HX711_CLOCK)
    LoadCells.begin(HX711_dout_1, HX711_dout_2, HX711_sck_1);
    LoadCells.set_scale(scalesF1, scalesF2);
    LoadCells.power_up();

    delay(500);

    if (LoadCells.is_ready()) {
      LoadCells.tare(5);
      scalesPresent = true;
    }
  #else
    LoadCell_1.begin(HX711_dout_1, HX711_sck_1);
    LoadCell_2.begin(HX711_dout_2, HX711_sck_2);
    LoadCell_1.set_scale(scalesF1); // calibrated val1
    LoadCell_2.set_scale(scalesF2); // calibrated val2

    delay(500);

    if (LoadCell_1.is_ready() && LoadCell_2.is_ready()) {
      scalesPresent = true;
      LoadCell_1.tare();
      LoadCell_2.tare();
    }
  #endif
}

void scalesTare() {
  if( scalesPresent && (!tareDone || !previousBrewState) ) {
    #if defined(SINGLE_HX711_CLOCK)
      if (LoadCells.is_ready()) LoadCells.tare(5);
    #else
      if (LoadCell_1.wait_ready_timeout(300) && LoadCell_2.wait_ready_timeout(300)) {
        LoadCell_1.tare(2);
        LoadCell_2.tare(2);
      }
    #endif
    tareDone=1;
    previousBrewState=1;
  }
}


void eepromInit() {
  int sig;

  #if defined(ARDUINO_ARCH_AVR)
  sig = EEPROM.read(EEP_SIG);
  setPoint = EEPROM.read(EEP_SETPOINT);
  preinfuseSoak = EEPROM.read(EEP_PREINFUSION_SOAK);
  #elif defined(ARDUINO_ARCH_STM32)
  EEPROM.get(EEP_SIG, sig);
  EEPROM.get(EEP_SETPOINT, setPoint);
  EEPROM.get(EEP_PREINFUSION_SOAK, preinfuseSoak);
  #endif

  // #if defined(ARDUINO_ARCH_AVR)
    //If it's the first boot we'll need to set some defaults
    if (sig != EEPROM_RESET || setPoint <= 0 || setPoint > 160 || preinfuseSoak > 500) {
    // USART_CH.print("SECU_CHECK FAILED! Applying defaults!");
    EEPROM.put(EEP_SIG, EEPROM_RESET);
    //The values can be modified to accomodate whatever system it tagets
    //So on first boot it writes and reads the desired system values
    EEPROM.put(EEP_SETPOINT, 100);
    EEPROM.put(EEP_OFFSET, 7);
    EEPROM.put(EEP_HPWR, 550);
    EEPROM.put(EEP_M_DIVIDER, 5);
    EEPROM.put(EEP_B_DIVIDER, 2);
    EEPROM.put(EEP_PREINFUSION, 1);
    EEPROM.put(EEP_P_START, 9);
    EEPROM.put(EEP_P_FINISH, 6);
    EEPROM.put(EEP_P_PROFILE, 1);
    EEPROM.put(EEP_PREINFUSION_SEC, 12);
    EEPROM.put(EEP_PREINFUSION_BAR, 5);
    EEPROM.put(EEP_REGPWR_HZ, 50);
    EEPROM.put(EEP_WARMUP, 0);
    EEPROM.put(EEP_GRAPH_BREW, 1);
    EEPROM.put(EEP_HOME_ON_SHOT_FINISH, 1);
    EEPROM.put(EEP_PREINFUSION_SOAK, 3);
    EEPROM.put(EEP_P_HOLD, 8);
    EEPROM.put(EEP_P_LENGTH, 18);
    EEPROM.put(EEP_BREW_DELTA, 1);
    EEPROM.put(EEP_SCALES_F1, 4000);
    EEPROM.put(EEP_SCALES_F2, 4000);
    EEPROM.put(EEP_STOPTIME_SEC, 45);
    }

  // Applying our saved EEPROM saved values
  valuesLoadFromEEPROM();
}

void valuesLoadFromEEPROM() {
  int init_val;

  // Loading the saved values fro EEPROM and sending them to the LCD
  EEPROM.get(EEP_STOPTIME_SEC, init_val);// reading stopTime value from eeprom
  if ( init_val >= 0 ) {
    myNex.writeNum("stopTime", init_val);
    myNex.writeNum("brewAuto.n7.val", init_val);
  }
  EEPROM.get(EEP_SETPOINT, init_val);// reading setpoint value from eeprom
  if ( init_val > 0 ) {
    myNex.writeNum("setPoint", init_val);
    myNex.writeNum("moreTemp.n1.val", init_val);
  }
  EEPROM.get(EEP_OFFSET, init_val); // reading offset value from eeprom
  if ( init_val >= 0 ) {
    myNex.writeNum("offSet", init_val);
    myNex.writeNum("moreTemp.n2.val", init_val);
  }
  EEPROM.get(EEP_HPWR, init_val);//reading HPWR value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("hpwr", init_val);
    myNex.writeNum("moreTemp.n3.val", init_val);
  }
  EEPROM.get(EEP_M_DIVIDER, init_val);//reading main cycle div from eeprom
  if ( init_val >= 1 ) {
    myNex.writeNum("mDiv", init_val);
    myNex.writeNum("moreTemp.n4.val", init_val);
  }
  EEPROM.get(EEP_B_DIVIDER, init_val);//reading brew cycle div from eeprom
  if (  init_val >= 1 ) {
    myNex.writeNum("bDiv", init_val);
    myNex.writeNum("moreTemp.n5.val", init_val);
  }
  EEPROM.get(EEP_P_START, init_val);//reading pressure profile start value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("ppStart", init_val);
    myNex.writeNum("brewAuto.n2.val", init_val);
  }

  EEPROM.get(EEP_P_FINISH, init_val);// reading pressure profile finish value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("ppFin", init_val);
    myNex.writeNum("brewAuto.n3.val", init_val);
  }
  EEPROM.get(EEP_P_HOLD, init_val);// reading pressure profile hold value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("ppHold", init_val);
    myNex.writeNum("brewAuto.n5.val", init_val);
  }
  EEPROM.get(EEP_P_LENGTH, init_val);// reading pressure profile length value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("ppLength", init_val);
    myNex.writeNum("brewAuto.n6.val", init_val);
  }

  EEPROM.get(EEP_PREINFUSION, init_val);//reading preinfusion checkbox value from eeprom
  if ( init_val >= 0 ) {
    myNex.writeNum("piState", init_val);
    myNex.writeNum("brewAuto.bt0.val", init_val);
  }

  EEPROM.get(EEP_P_PROFILE, init_val);//reading pressure profile checkbox value from eeprom
  if ( init_val >= 0 ) {
    myNex.writeNum("ppState", init_val);
    myNex.writeNum("brewAuto.bt1.val", init_val);
  }

  EEPROM.get(EEP_PREINFUSION_SEC, init_val);//reading preinfusion time value from eeprom
  if (init_val >= 0) {
    myNex.writeNum("piSec", init_val);
    myNex.writeNum("brewAuto.n0.val", init_val);
  }

  EEPROM.get(EEP_PREINFUSION_BAR, init_val);//reading preinfusion pressure value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("piBar", init_val);
    myNex.writeNum("brewAuto.n1.val", init_val);
  }
  EEPROM.get(EEP_PREINFUSION_SOAK, init_val);//reading preinfusion soak times value from eeprom
  if (  init_val >= 0 ) {
    myNex.writeNum("piSoak", init_val);
    myNex.writeNum("brewAuto.n4.val", init_val);
  }
  // Region POWER value
  EEPROM.get(EEP_REGPWR_HZ, init_val);//reading region frequency value from eeprom
  if (  init_val == 50 || init_val == 60 ) myNex.writeNum("regHz", init_val);


  // Brew page settings
  EEPROM.get(EEP_HOME_ON_SHOT_FINISH, init_val);//reading brew time value from eeprom
  if (  init_val == 0 || init_val == 1 ) {
    myNex.writeNum("homeOnBrewFinish", init_val);
    myNex.writeNum("brewSettings.btGoHome.val", init_val);
  }

  EEPROM.get(EEP_GRAPH_BREW, init_val);//reading preinfusion pressure value from eeprom
  if (  init_val == 0 || init_val == 1) {
    myNex.writeNum("graphEnabled", init_val);
    myNex.writeNum("brewSettings.btGraph.val", init_val);
  }

  EEPROM.get(EEP_BREW_DELTA, init_val);//reading preinfusion pressure value from eeprom
  if (  init_val == 0 || init_val == 1) {
    myNex.writeNum("deltaState", init_val);
    myNex.writeNum("brewSettings.btTempDelta.val", init_val);
  }

  // Warmup checkbox value
  EEPROM.get(EEP_WARMUP, init_val);//reading preinfusion pressure value from eeprom
  if (  init_val == 0 || init_val == 1 ) {
    myNex.writeNum("warmupState", init_val);
    myNex.writeNum("morePower.bt0.val", init_val);
  }
  // Scales values
  EEPROM.get(EEP_SCALES_F1, scalesF1);//reading scale factors value from eeprom
  EEPROM.get(EEP_SCALES_F2, scalesF2);//reading scale factors value from eeprom
}

void ads1115Init() {
#if defined(ARDUINO_ARCH_STM32)
  ADS.begin();
  ADS.setGain(0);      // 6.144 volt
  ADS.setDataRate(7);  // fast
  ADS.setMode(0);      // continuous mode
  ADS.readADC(0);      // first read to trigger
#endif
}

void pinInit() {
  pinMode(relayPin, OUTPUT);
  pinMode(valvePin, OUTPUT);
  pinMode(brewPin, INPUT_PULLUP);
  pinMode(steamPin, INPUT_PULLUP);
  pinMode(HX711_dout_1, INPUT_PULLUP);
  pinMode(HX711_dout_2, INPUT_PULLUP);
}

void dbgInit() {
  #if defined(STM32F4xx) && defined(DEBUG_ENABLED)
  analogReadResolution(12);
  #endif
}
void dbgOutput() {
  #if defined(STM32F4xx) && defined(DEBUG_ENABLED)
  int VRef = readVref();
  myNex.writeNum("debug1",readTempSensor(VRef));
  myNex.writeNum("debug2",ADS.getError());
  #endif
}
