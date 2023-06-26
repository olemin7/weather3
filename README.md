# platformio
source ~/.platformio/penv/bin/activate
pio run
pio project init --ide eclipse
# clock
clock with ntp synk
# Board
http://arduino.esp8266.com/stable/package_esp8266com_index.json
# lib 
https://github.com/olemin7/esp_libs

# referenses
schematic: https://circuits.io/circuits/3798057-net-clock-led
gtest
https://www.codeproject.com/Articles/811934/Cplusplus-unit-test-start-guide-how-to-set-up-Goog
https://github.com/google/googletest.git
arduino:
https://github.com/arduino/Arduino/blob/master/build/shared/manpage.adoc

# esp8266 reference
http://www.kloppenborg.net/blog/microcontrollers/2016/08/02/getting-started-with-the-esp8266

##wemos
Pin | Function    | ESP-8266 Pin | used
TX  | TXD         | TXD          |
RX  | RXD         | RXD          |
A0  | ADC, 3.3V   | A0           | battery
D0  | IO          | GPIO16       |
D1  | IO, SCL     | GPIO5        | 
D2  | IO, SDA     | GPIO4        | 
D3  | IO, 10k P-up| GPIO0        | 
D4  | IO, 10k P-up,LED|   GPIO2  | DHT.data
D5  | IO, SCK     | GPIO14       | 
D6  | IO, MISO    | GPIO12       | 
D7  | IO, MOSI    | GPIO13       | 
D8  | IO, 10k P-down, SS|  GPIO15|
G   | Ground      | GND          |
5V  | 5V          | -            |
3V3 | 3.3V        | 3.3V         |
RST | Reset       | RST          |


#ADC wemos
 A0--
    |
   220K
    |--- ADC
   100K
    |
   GND
 
#battery
 vbat 130k --A0
 adc val= 848,volt=3.72656 (3.95

#DHT
1 vcc (3.3V)
2 data (D4)
3 nc
4 GND (G)

#deepsleep
D0->RST Connect GPIO16 to RST pin only after uploading the code

https://github.com/adam-p/markdown-here/wiki/Markdown-Cheatsheet#lists
https://diyi0t.com/how-to-reduce-the-esp8266-power-consumption/
https://www.electronicshub.org/esp8266-deep-sleep-mode/
