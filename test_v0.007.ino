/*
  SD card datalogger

   SD card attached to SPI bus as follows:
 ** MOSI - pin 11
 ** MISO - pin 12
 ** CLK - pin 13
 ** CS - pin 4 (for MKRZero SD: SDCARD_SS_PIN)
*/

#include <SPI.h>
#include <SD.h>

#define SHORT_PULSE  500
#define LONG_PULSE  1500
#define SHORT_MARGIN 300
#define LONG_MARGIN 300

const int chipSelect = 10;
static int wind_dir_degr[]= {0, 23, 45, 68, 90, 113, 135, 158, 180, 203, 225, 248, 270, 293, 315, 338};
int temp_raw;
float temperature;
int val = 0;
unsigned long transition_t = micros();
unsigned long now, duration;
#ifdef DEBUG
unsigned long short_min = 9999, short_max = 0;
unsigned long long_min = 9999, long_max = 0;
unsigned int pulse_buffer[200];
unsigned int pb_idx = 0;
#endif
#define BUFFER_SIZE  16//16
byte byte_buffer[BUFFER_SIZE];
byte buffer_idx = 0;
byte sig_seen = 0;
unsigned short shift_register = 0;
byte bit_count = 0;
/*
* Function taken from Luc Small (http://lucsmall.com), itself
* derived from the OneWire Arduino library. Modifications to
* the polynomial according to Fine Offset's CRC8 calulations.
*/
uint8_t _crc8( uint8_t *addr, uint8_t len)
{
        uint8_t crc = 0;
        // Indicated changes are from reference CRC-8 function in OneWire library
        while (len--) {
                uint8_t inbyte = *addr++;
                uint8_t i;
                for (i = 8; i; i--) {
                        uint8_t mix = (crc ^ inbyte) & 0x80; // changed from & 0x01
                        crc <<= 1; // changed from right shift
                        if (mix) crc ^= 0x31;// changed from 0x8C;
                        inbyte <<= 1; // changed from right shift
                }
        }
        return crc;
}
void setup() {
  // put your setup code here, to run once:
  pinMode(2, INPUT);
  Serial.begin(115200);// Open serial communications and wait for port to open:
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
  Serial.print("Initializing SD card...");
  // see if the card is present and can be initialized:
  if (!SD.begin(chipSelect)) {
    Serial.println("Card failed, or not present");
    // don't do anything more:
    while (1);
  }
  Serial.println("card initialized.");
  // prints title with ending line break
  Serial.println("WH1080RF Monitor"); 
}
void loop() {
  // put your main code here, to run repeatedly:
  int newVal=0;
  // make a string for assembling the data to log:
  String dataString = "";
  for(int i=0; i < 10; i++) {
    newVal += digitalRead(2) ? 1 : 0;
    delayMicroseconds(5);
  }
  newVal = (newVal + 5) / 10;
  /*
   * Handle situations where the clock has rolled over
   * between transitions (happens every ~70 mins).
   */
  now = micros();
  if(transition_t <= now)
    duration = now - transition_t;
  else
    duration = (~transition_t) + now;
  if(newVal != val) {  // then transitioning state
    /*
     *  We update the transition time for the pulse, and
     *  change the current state of the input value.
     */
    transition_t = now;
    val = newVal;
    /*
     *  If the pulse width (hi or low) is outside the
     *  range of the Fine Offset signal, then ignore them.
     */
    if(duration < (SHORT_PULSE - SHORT_MARGIN) 
        || duration > (LONG_PULSE + LONG_MARGIN)) {
      // Meaningless pulse
      return;
    }
    /*
     *  If we reach here, then we have seen a potentially
     *  valid pulse. Shift the bit into the register.
     */
    if(newVal == 1) {
      // rising edge of a pulse (0 -> 1)
    } else {
      // falling edge of a pulse (1 -> 0)
      if( duration >= (SHORT_PULSE - SHORT_MARGIN) && duration <= (SHORT_PULSE + SHORT_MARGIN) ) {
        // short pulse is binary '1'
        shift_register = (shift_register << 1) | 0x01;
        bit_count++;
        #ifdef DEBUG
        if(duration < short_min) short_min = duration;
        if(duration > short_max) short_max = duration;
        if(pb_idx < 200) pulse_buffer[pb_idx++] = duration;
        #endif
      } else if(duration >= (LONG_PULSE - LONG_MARGIN) && duration <= (LONG_PULSE + LONG_MARGIN)) {
        // long pulse is binary '0'
        shift_register = (shift_register << 1);
        bit_count++;
        #ifdef DEBUG
        if(duration < long_min) long_min = duration;
        if(duration > long_max) long_max = duration;
        if(pb_idx < 200) pulse_buffer[pb_idx++] = duration;
        #endif
      }
    }
    // Look for signature of 0xfa (4 bits 0xf0 pre-amble + 0xa)
    if((shift_register & 0xff) == 0xfa && buffer_idx == 0) {
      // Found signature - discard pre-amble and leave 0x0a.
      shift_register = 0x0a;
      bit_count = 4;
      sig_seen = 1;  // Flag that the signature has been seen.
      #ifdef DEBUG
      pb_idx = 0;
      #endif
    } else if(bit_count == 8 && sig_seen) {
      // Got a byte, so store it if we have room.
      if(buffer_idx < BUFFER_SIZE)
        byte_buffer[buffer_idx++] = (byte)(shift_register & 0xff);
      else
        Serial.println("Overflow on byte");
      shift_register = 0;
      bit_count = 0;
    }
  } else {
      /*
       *  Have we reached timeout on duration? If so, process any
       *  bytes present in the buffer and then reset the state
       *  variables.
       */    
      if(duration > 5000) {
        if (buffer_idx > 0) {
          /*
           *  Dump the bytes to the Serial.
           */
          Serial.print("Found ");
          Serial.println(buffer_idx);
          for(int i = 0; i < buffer_idx; i++) {
            for (byte mask = 0x80; mask; mask >>= 1) {
              Serial.print(mask & byte_buffer[i] ? '1' : '0');
            }
            Serial.print(' ');
            Serial.println(byte_buffer[i]);
          }
          /*
           *  If we have enough bytes, then verify the checksum.
           */
          if(buffer_idx >= 10 && _crc8(byte_buffer, 9) == byte_buffer[9]) {
            Serial.println("CRC Passed");
          }
          int device_id = (byte_buffer[0] << 4 & 0xf0) | (byte_buffer[1] >> 4);
          Serial.print("Device ID=");Serial.println(device_id);
          int battery_low = (byte_buffer[8] >> 4) == 1;
          Serial.print("Battery "); battery_low ? Serial.println("LOW") : Serial.println("OK");
          temp_raw    = ((byte_buffer[1] & 0x03) << 8) | byte_buffer[2]; // only 10 bits, discard top bits
          temperature = (temp_raw - 400) * 0.1f;
          Serial.print("Temperature=");Serial.println(temperature);
          int humidity = byte_buffer[3];
          Serial.print("Humidity=");Serial.print(humidity);Serial.println("%");
          int direction_deg = wind_dir_degr[byte_buffer[8] & 0x0f];
          Serial.print("Wind Direction=");Serial.print(direction_deg);Serial.println("Deg");
          float speeds = (byte_buffer[4] * 0.30f) * 3.6f;
          Serial.print("Wind Speed=");Serial.print(speeds);Serial.println("km/h");
          float gust = (byte_buffer[5] * 0.30f) * 3.6f;
          Serial.print("Wind Gust=");Serial.print(gust);Serial.println("km/h");
          int rain_raw = ((byte_buffer[6] & 0x0f) << 8) | byte_buffer[8];
          float rain = rain_raw * 0.3f;
          Serial.print("Rain=");Serial.print(rain);Serial.println("mm");
          buffer_idx = 0;

  // read three sensors and append to the string:
    dataString=dataString+"Device ID="+String(device_id);
    dataString += ",";

  // open the file. note that only one file can be open at a time,
  // so you have to close this one before opening another.
  File dataFile = SD.open("datalog.txt", FILE_WRITE);

  // if the file is available, write to it:
  if (dataFile) {
    dataFile.println(dataString);
    dataFile.close();
    // print to the serial port too:
    Serial.println(dataString);
  }
  // if the file isn't open, pop up an error:
  else {
    Serial.println("error opening datalog.txt");
  }
        }
        #ifdef DEBUG
          for(int i = 0; i < pb_idx; i++) {
            Serial.print("Pulse ");
            Serial.print(i);
            Serial.print(' ');
            Serial.println(pulse_buffer[i]);
          }
          Serial.print("Short ");
          Serial.print(short_min);
          Serial.print(' ');
          Serial.println(short_max);
          Serial.print("Long ");
          Serial.print(long_min);
          Serial.print(' ');
          Serial.println(long_max);
          short_min = long_min = 9999;
          short_max = long_max = 0;
        #endif
        shift_register = 0;
        bit_count = 0;
        sig_seen = 0;
      }
      // No transition on this iteration
      // Serial.flush();
  }
}  // end loop()
