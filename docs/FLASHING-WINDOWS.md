# Flashing the pedal from Windows

This installs a firmware `.bin` onto the Daisy Seed inside the pedal. Plan on ten minutes the first time, mostly for the USB driver, and under a minute after that.

You need: the pedal, a USB micro-B cable that carries data (some charging cables do not), and a PC with Chrome or Edge.

## Which file to flash

Every push to `main` builds firmware for all pedal variants on GitHub.

1. Open the repository's **Actions** tab and click the newest **Build All** run with a green check.
2. Scroll to **Artifacts** and download the one for your pedal. For the 125B build (the one Ben and Steve have) that is **125B-Firmware**.
3. Unzip it. You get `125B.bin`.

## One-time driver setup with Zadig

Windows does not ship a driver that lets the browser or dfu-util talk to the Daisy. Zadig installs one. This is the step that trips most people, so do it first.

1. Download Zadig from https://zadig.akeo.ie and run it. No install needed.
2. Plug the pedal in and put it in bootloader mode: press **RESET** on the Daisy Seed, then within 5 seconds single-press **BOOT**. The Seed's onboard LED keeps blinking.
3. In Zadig, choose **Options > List All Devices**.
4. In the dropdown pick the entry named **DFU in FS Mode** or **Daisy Bootloader**. Check that the USB ID shows `0483 DF11`.
5. In the driver box to the right of the arrow, choose **WinUSB**. Press **Replace Driver** (or **Install Driver**). Wait for it to finish.
6. Unplug and replug the pedal. Repeat step 2 to get back into bootloader mode.

If you also want the factory bootloader to work (needed only for a brand-new Seed), repeat steps 3 to 5 once more with the pedal in factory bootloader mode: hold **BOOT**, press and release **RESET**, release **BOOT**. It appears as **STM32 BOOTLOADER**.

## Method 1: browser (recommended)

1. With the driver installed, plug the pedal in and power it from its 9 V supply.
2. Open https://flash.daisy.audio in Chrome or Edge. Firefox does not support WebUSB.
3. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**. The onboard LED keeps blinking.
4. Choose **Connect**, pick **DFU in FS Mode** or **Daisy Bootloader**, and allow it.
5. Under the firmware section choose **File Upload**, pick your `.bin`, and press **Flash**.
6. When it reports done, press **RESET**. The pedal starts on the new firmware.

## Method 2: command line with dfu-util

1. Download the Windows binaries from http://dfu-util.sourceforge.net/releases/ (the newest `dfu-util-x.xx-binaries.tar.xz`). Extract `win64\dfu-util.exe` somewhere convenient, such as `C:\dfu\`.
2. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**.
3. Open PowerShell and check the device is visible:

   ```powershell
   C:\dfu\dfu-util.exe -l
   ```

   Expect one line containing `0483:df11` whose `name=` starts with `@Flash /0x90000000`. If it starts with `@Internal Flash /0x08000000`, see troubleshooting.

4. Flash, replacing the path with your file:

   ```powershell
   C:\dfu\dfu-util.exe -a 0 -s 0x90040000:leave -D "$env:USERPROFILE\Downloads\125B.bin" -d ,0483:df11
   ```

   Expected output ends with `File downloaded successfully`. The pedal reboots by itself. The warning `Invalid DFU suffix signature` is harmless.

## First-time only: installing the Daisy bootloader

Skip this if the pedal has ever been flashed before. A brand-new Daisy Seed needs it once.

1. Hold **BOOT**, press and release **RESET**, then release **BOOT**. This enters the factory bootloader (make sure Zadig has installed WinUSB for **STM32 BOOTLOADER** as described above).
2. In the web flasher open the **Bootloader** tab and press **Flash Bootloader**.
3. Press **RESET**. The onboard LED blinks for about 3 seconds. Now follow Method 1 or 2.

## Troubleshooting

**Zadig does not list the device.**
Choose **Options > List All Devices** and make sure the pedal is in bootloader mode at that moment (RESET, then BOOT within 5 seconds). Try another cable and a USB-A port directly on the PC.

**The browser's Connect dialog is empty.**
The WinUSB driver is not installed for this mode of the device. Run Zadig again with the pedal in bootloader mode.

**`dfu-util -l` shows `@Internal Flash /0x08000000` and flashing fails with `Last page at ... is not writeable`.**
You entered the factory bootloader by holding BOOT while pressing RESET. Press RESET on its own, then single-press BOOT within 5 seconds, and run the flash again.

**`No DFU capable USB device available`, or the device disappears after a couple of seconds.**
RESET was pressed but BOOT was not, so the bootloader timed out and started the old firmware. Press RESET, then BOOT within 5 seconds, and retry.

**The pedal starts but the LEDs are off.**
Firmware older than September 2026 booted bypassed. Tap the right footswitch. If the LED still does not light, flash the newest build from the Actions tab.
