//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2018-08-09 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------

// define this to read the device id, serial and device type from bootloader section
// #define USE_OTA_BOOTLOADER

//#define STORAGEDRIVER at24cX<0x50,128,32>
#define TICKS_PER_SECOND 1000UL

#include <SPI.h>    // when we include SPI.h - we can use LibSPI class
#include <Wire.h>
#include <EEPROM.h> // the EEPROM library contains Flash Access Methods
#include <OneWireSTM.h>
#include <AskSinPP.h>

#include <Dimmer.h>
#include <Sensors.h>
#include <sensors/Ds18b20.h>


// we use a Pro Mini
// Arduino pin for the LED
// D4 == PIN 4 on Pro Mini
#define LED_PIN LED_BUILTIN
// Arduino pin for the config button
// B0 == PIN 8 on Pro Mini
#define CONFIG_BUTTON_PIN PB12

#define DIMMER1_PIN PB1
#define DIMMER2_PIN PA3

#define ENCODER1_SWITCH PB15
#define ENCODER1_CLOCK  PB13
#define ENCODER1_DATA   PB14

#define ENCODER2_SWITCH PB10
#define ENCODER2_CLOCK  PB9
#define ENCODER2_DATA   PB8

// number of available peers per channel
#define PEERS_PER_CHANNEL 2

// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0x66,0x23,0xab},       // Device ID
    "papa6623ab",           // Device Serial
    {0x01,0x09},            // Device Model
    0x2C,                   // Firmware Version
    as::DeviceType::Dimmer, // Device Type
    {0x01,0x00}             // Info Bytes
};

/**
 * Configure the used hardware
 */
typedef LibSPI<PA4> RadioSPI;
typedef AskSin<StatusLed<LED_BUILTIN>,NoBattery,Radio<RadioSPI,PB0> > HalType;
typedef DimmerChannel<HalType,PEERS_PER_CHANNEL> ChannelType;
typedef DimmerDevice<HalType,ChannelType,6,3,PWM16<> > DimmerType;

HalType hal;
DimmerType sdev(devinfo,0x20);
ConfigButton<DimmerType> cfgBtn(sdev);

// https://www.mikrocontroller.net/articles/Drehgeber
const int8_t encoder_table[16] PROGMEM = {0,0,-1,0,0,0,0,1,1,0,0,0,0,-1,0,0};
template<class DeviceType>
class Encoder : public InternalButton<DeviceType> {

  class EncAlarm : public Alarm {
    Encoder& enc;
  public:
    EncAlarm (Encoder& e) : Alarm(0), enc(e) {
      async(true);
    }
    virtual void trigger (AlarmClock& clock) {
      enc.checkPins();
      start(clock);
    }
    void start (AlarmClock& clock) {
      set(millis2ticks(5));
      clock.add(*this);
    }
    void stop (AlarmClock& clock) {
      clock.cancel(*this);
    }
  };

  int8_t last;
  volatile int8_t delta;
  uint8_t clkpin, dtpin;
  EncAlarm alarm;

public:
  Encoder (DeviceType& dev,uint8_t num) : InternalButton<DeviceType>(dev,num), last(0), delta(0), clkpin(0), dtpin(0), alarm(*this) {};
  virtual ~Encoder () {};

  bool checkPins () {
    int8_t ll=0;
    if (digitalRead(clkpin)) ll |=2;
    if (digitalRead(dtpin))  ll |=1;
    last = ((last << 2)  & 0x0F) | ll;
    delta += pgm_read_byte(&encoder_table[last]);
    return delta != 0;
  }

  void init (uint8_t sw) {
    InternalButton<DeviceType>::init(sw);
  }

  void init (uint8_t clk,uint8_t dt) {
    clkpin = clk;
    dtpin = dt;
    pinMode(clkpin, INPUT);
    pinMode(dtpin, INPUT);
    alarm.start(sysclock);
  }

  int8_t read () {
    int8_t val=0;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE )
    {
      val = delta;
      delta = val & 1;
    }
    return val >> 1;
  }

  template<class ChannelType,class List3Type>
  void process (ChannelType& channel) {
    int8_t dx = read();
    if( dx != 0 && channel.status() != 0 ) {
      List3Type l3 = channel.getList3(this->peer());
      if( l3.valid() ) {
        if( dx > 0 ) channel.dimUp(l3.sh());
        else channel.dimDown(l3.sh());
      }
    }
  }
};

class TempSens : public Alarm {
  Ds18b20  temp;
  OneWire  ow;
  bool     measure;
public:
  TempSens () : Alarm(0), ow(PB5), measure(false) {}
  virtual ~TempSens () {}

  void init () {
    Ds18b20::init(ow, &temp, 1);
    set(seconds2ticks(15));
    sysclock.add(*this);
  }

  virtual void trigger (AlarmClock& clock) {
    if( measure == false ) {
      temp.convert();
      set(millis2ticks(800));
    }
    else {
      temp.read();
      DPRINT("Temp: ");DDECLN(temp.temperature());
      set(seconds2ticks(60));
    }
    measure = !measure;
    clock.add(*this);
  }
};

Encoder<DimmerType> enc1(sdev,1);
Encoder<DimmerType> enc2(sdev,2);
TempSens tempsensor;

void setup () {
  delay(5000);
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  bool first = sdev.init(hal,DIMMER1_PIN,DIMMER2_PIN);
  buttonISR(cfgBtn,CONFIG_BUTTON_PIN);
  buttonISR(enc1,ENCODER1_SWITCH);
  enc1.init(ENCODER1_CLOCK,ENCODER1_DATA);
  buttonISR(enc2,ENCODER2_SWITCH);
  enc2.init(ENCODER2_CLOCK,ENCODER2_DATA);

  if( 1 || first == true ) {
    sdev.channel(1).peer(enc1.peer());
    DimmerList3 l3 = sdev.channel(1).getList3(enc1.peer());
    l3.lg().actionType(AS_CM_ACTIONTYPE_INACTIVE);

    sdev.channel(2).peer(enc2.peer());
    l3 = sdev.channel(2).getList3(enc2.peer());
    l3.lg().actionType(AS_CM_ACTIONTYPE_INACTIVE);
  }

  tempsensor.init();

  sdev.initDone();
  sdev.led().invert(true);
}

void loop () {
  HMID devid;
  sdev.getDeviceID(devid);
  enc1.process<ChannelType,DimmerList3>(sdev.channel(1));
  enc2.process<ChannelType,DimmerList3>(sdev.channel(2));

  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if( worked == false && poll == false ) {
//    hal.activity.savePower<Idle<true> >(hal);
  }
}

