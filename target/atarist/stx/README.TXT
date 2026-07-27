MD/Net
----------------------------------------------------------------------
WiFi for your Atari ST by Neil Rackett
neilrackett.com/atarist


INTRODUCTION

MD/Net turns your SidecarTridge Multi-device into a wireless network
adapter for your Atari ST. The firmware connects to your WiFi and the
MDNET.STX driver lets STinG use it.

In the STinG control panel, MD/Net will appear as a port named "WiFi".


WHAT YOU NEED

* SidecarTridge Multi-device with Raspberry Pi Pico W
* MD/Net firmware installed and connected to your WiFi
* STinG installed from https://hardware.atari.org/files/sfl.zip


INSTALLING

1. Copy MDNET.STX into your STING folder, usually C:\STING.
2. Deactivate any other Ethernet drivers, either by removing the X
   in the control panel, or renaming ENEC.STX and similar files.
3. Reboot your ST.

CONFIGURING

1. Open the STinG control panel (STNGPORT.CPX).
2. Select the port named "WiFi".
3. Set the IP address you'd like your ST to use, for example
   192.168.1.242, and set the subnet mask to 255.255.255.0.
4. Make sure the port is set to Active, then save.
5. Edit ROUTE.TAB in your STING folder so it refers to the WiFi port.
   The entries are separated by TAB characters (replace 192.168.1.1 
   with your actual router address):

   192.168.1.0    255.255.255.0    WiFi    0.0.0.0
   0.0.0.0        0.0.0.0          WiFi    192.168.1.1

6. For name lookups, set NAMESERVER in DEFAULT.CFG to your
   router's address.
7. Reboot your ST.

Similar to NetUSBee, but with "WiFi" selected instead of "EtherNet": 
https://hardware.atari.org/manuals/netsting.htm


TESTING

Run STING/TOOLS/PING.PRG and ping your router.


FAQS

Q. Why don't I see "MD/Net connected" message at boot?
A. Check that the MD/Net firmware is installed and your SidecarTridge
   Multi-device is connected to WiFi.

Q. Why does MDNET.STX say "MD/Net cartridge not found"?
A. The driver could not see the MD/Net firmware. Make sure the
   MD/Net firmware is installed, you see the boot message and
   do a full power-cycle rather than reset after installing.

Q. Why doesn't The WiFi port appear in the STinG control panel?
A. You need to copy MDNET.STX to your STING folder and restart your ST.

Q. Why do pings time out?
A. Check the IP address and subnet mask in the STinG control panel,
   and that ROUTE.TAB refers to "WiFi" and uses tab separators not spaces.

Copyright (C) 2026 Neil Rackett
MD/Net is free software under the GNU General Public License v3
or later.
