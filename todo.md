Webserver (manuelle Änderung der Rest API endpunkte)

- [x] lumitec testen mit Refactored code

- Menuconfig anpassen falls sinnvoll:
  - [x] DMX config 
  - [x] LED config
  - [x] Button Config
  - [x] Wifi

- [x] Webserver zur Konfiguration
- [x] Pins/RS485 Chip config über menuconfig

- Ethernet
- RS485 - 1 für DMX


- github seite auf Webpage
- status auf webpage? Letzten 5 Befehle oder so fürs debugging?



# Platine
- Benötigt die Platine eine Power LED die Dauerhaft an ist? 
  - Eventuell nur einschalten wenn Knopf gedrückt? 

- Status LED ist leicht an beim flashen. Eventuell Diode ergänzen. 
- Reset Button notwendig? 
- UART Pins auf dem Board zum flashen ohne USB? Ist USB notwendig? 

# PinOut ESP
ESP32 Wroom 32UE - 4 Mb flash
- Status LED
  - 5
- BTN0
  - 16
- RS485 - ADM 3485JRZ
  - 1
    - DI (TX): 33
    - RO (RX): 32
    - RE/DE: 13
  - 2
    - DI (TX): 12
    - RO (RX): 14
    - RE/DE: 15

