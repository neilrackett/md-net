======================================================================
 MD/Net - Wireless networking for the Atari ST
 MDNET.STX - STinG port driver
======================================================================

WHAT THIS IS

MD/Net turns a SidecarTridge Multi-device into a wireless network
adapter for your Atari ST. The MD/Net firmware runs on the cartridge
and connects to your WiFi; MDNET.STX is the driver that lets STinG
send and receive through it.

The driver appears in STinG as a port named "WiFi".


WHAT YOU NEED

 * A SidecarTridge Multi-device with a Raspberry Pi Pico W
 * MD/Net firmware installed and connected to your WiFi
   (you will see "MD/Net connected: <address>" when the ST boots)
 * STinG installed on your ST


INSTALLING

 1. Copy MDNET.STX into your STING folder (usually C:\STING).

 2. Remove or rename any other Ethernet driver in that folder,
    such as ENEC.STX. Only one Ethernet driver should be active.

 3. Reboot the ST.


CONFIGURING

 1. Run STNGPORT.CPX from the control panel.

 2. Select the port named "WiFi".

 3. Set the IP address you want the ST to use. Pick an address on
    your home network that nothing else is using, for example
    192.168.1.242, and set the subnet mask to 255.255.255.0.

    TIP: reserve that address in your router's DHCP settings so
    nothing else is given it later.

 4. Make sure the port is set to Active, then save.

 5. Edit ROUTE.TAB in your STING folder so it refers to the WiFi
    port. The entries are separated by TAB characters:

       192.168.1.0    255.255.255.0    WiFi    0.0.0.0
       0.0.0.0        0.0.0.0          WiFi    192.168.1.1

    Replace 192.168.1.1 with your router's address if different.

 6. For name lookups, set NAMESERVER in DEFAULT.CFG to your
    router's address.

 7. Reboot the ST.


TESTING

Run PING.PRG and ping your router, for example:

    PING 192.168.1.1

Do not ping the ST's own address - STinG answers that internally,
so it tells you nothing.


IF IT DOES NOT WORK

 * No "MD/Net connected" message at boot
   The cartridge is not connected to WiFi. Check your WiFi settings
   in the Booster configuration.

 * MDNET.STX says "MD/Net cartridge not found"
   The driver could not see the MD/Net firmware. Make sure the
   MD/Net firmware is running (you should see the boot message),
   and power-cycle rather than warm-reset.

 * The WiFi port does not appear in STNGPORT.CPX
   MDNET.STX is not in the STING folder, or STinG did not load it.

 * Ping times out
   Check the IP address and subnet mask in STNGPORT.CPX, and that
   ROUTE.TAB refers to "WiFi" and uses TAB separators.

======================================================================
