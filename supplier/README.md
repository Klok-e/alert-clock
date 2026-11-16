# Supplier firmware

`HG3.2.2-SD.bin` is the clock's stock HG V3.2.2 firmware, the `SD.bin` member of the
supplier's `HG3.2.2 APP.rar` (source link and SHA-256 in [`manifest.json`](manifest.json)).
It is a full flash image starting at offset 0. To put the clock back on stock firmware:

    pio pkg exec -p platformio/tool-esptoolpy -- esptool.py --port /dev/ttyUSB0 write_flash 0 supplier/HG3.2.2-SD.bin

A `read_flash` backup of your own clock (README "Setup", step 4) also keeps your settings;
prefer it when you have one.
