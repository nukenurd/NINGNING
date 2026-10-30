# NING NING - Meshtastic client for the 2.8" CYD
By Claude. Inspired by Mark.

## Get the .bin (no software to install)
1. Make a free GitHub account, create a new repo, upload everything in this folder
   (keep the folder structure, including `.github/workflows/build.yml`).
2. Open the **Actions** tab -> **Build NING NING firmware** -> wait ~5 min (or **Run workflow**).
3. Open the finished run -> download artifact **NINGNING-firmware** (zip).
   You get two single-file images:
   - `NINGNING_cyd_ili9341.bin`  <- try this first (most CYDs)
   - `NINGNING_cyd_st7789.bin`   <- use if the first gives a white / garbled screen

## Flash it (one file, address 0x0)
Easiest: Chrome/Edge -> https://espressif.github.io/esptool-js/
Connect, set **Flash Address = 0x0**, choose the .bin, Program.
Or: `esptool.py --chip esp32 --baud 460800 write_flash 0x0 NINGNING_cyd_ili9341.bin`

## Or build yourself
`pip install platformio` then `pio run -e cyd_ili9341` (or `-e cyd_st7789`), `pio run -t upload`.

## Using it
- First boot: splash, then touch calibration - tap the 4 red crosses.
  To recalibrate later: touch the screen during the splash, or SCAN screen -> CALIB.
- Scan screen lists nearby Meshtastic nodes - tap yours. If the node shows a PIN, type it on the
  pad. The node is saved and auto-reconnects on next boot.
- Top bar: PRIMARY (channel 0) | SECONDARY (channel 1) | BT status (tap = back to scan screen).
  A `*` on a tab means unread messages. Messages on channels 2-7 appear in SECONDARY tagged `[cN]`.
- Type with the keyboard (Shift, 123 symbols, DEL), SEND goes to the selected tab's channel.
- FORGET clears the saved node and BLE bonds - do this if you factory-reset the node and pairing fails.

## Notes
- Phone apps usually hold the BLE link exclusively: disconnect your phone's Meshtastic app first.
- Node's Bluetooth must be enabled. Set channel 1 on the node if you want a working SECONDARY tab.
