# Flashing the pedal from a Mac

This installs a firmware `.bin` onto the Daisy Seed inside the pedal. It takes about five minutes the first time and under a minute after that.

You need: the pedal, a USB micro-B cable that carries data (some charging cables do not), and a Mac with Chrome or Edge for the browser method, or Homebrew for the command-line method.

## Which file to flash

Every push to `main` builds firmware for all pedal variants on GitHub.

1. Open the repository's **Actions** tab and click the newest **Build All** run with a green check.
2. Scroll to **Artifacts** and download the one for your pedal. For the 125B build (the one Ben and Steve have) that is **125B-Firmware**.
3. Unzip it. You get `125B.bin`.

If you built the firmware yourself, the file is `Software/GuitarPedal/build/guitarpedal.bin`.

## Method 1: browser (recommended)

1. Plug the pedal into the Mac by USB. Power the pedal from its 9 V supply as usual.
2. Open https://flash.daisy.audio in Chrome or Edge. Safari and Firefox do not support WebUSB.
3. Put the pedal in bootloader mode. Press **RESET** on the Daisy Seed, then within 5 seconds single-press **BOOT**. The Seed's onboard LED keeps blinking; that means the bootloader is locked open and will wait for you.
4. In the web flasher, choose **Connect**, pick the device named **DFU in FS Mode** or **Daisy Bootloader**, and allow it.
5. Under the firmware section choose **File Upload**, pick your `.bin`, and press **Flash**. The progress bar runs for a few seconds.
6. When it reports done, press **RESET** on the Seed. The pedal starts on the new firmware.

## Method 2: command line with dfu-util

1. Install dfu-util once:

   ```bash
   brew install dfu-util
   ```

2. Put the pedal in bootloader mode: press **RESET**, then within 5 seconds single-press **BOOT**. The onboard LED keeps blinking.
3. Confirm the Mac sees it:

   ```bash
   dfu-util -l
   ```

   You should see one line containing `0483:df11` whose `name=` starts with `@Flash /0x90000000`. That is the Daisy bootloader. If the name starts with `@Internal Flash /0x08000000` instead, see troubleshooting below.

4. Flash, replacing the path with your file:

   ```bash
   dfu-util -a 0 -s 0x90040000:leave -D ~/Downloads/125B.bin -d ,0483:df11
   ```

   Expected output ends with `File downloaded successfully` and `Transitioning to dfuMANIFEST state`. The pedal reboots by itself.

   The warning `Invalid DFU suffix signature` is harmless.

## First-time only: installing the Daisy bootloader

Skip this if the pedal has ever been flashed before; the bootloader is already there. A brand-new Daisy Seed needs it once.

1. Hold **BOOT**, press and release **RESET**, then release **BOOT**. This enters the factory bootloader.
2. In the web flasher, open the **Bootloader** tab and press **Flash Bootloader**. Or from the command line, from `Software/GuitarPedal` after building:

   ```bash
   make program-boot
   ```

3. Press **RESET**. The onboard LED blinks for about 3 seconds. Now follow Method 1 or 2 above.

## Troubleshooting

**`dfu-util -l` shows `@Internal Flash /0x08000000` and flashing fails with `Last page at ... is not writeable`.**
You entered the factory bootloader by holding BOOT while pressing RESET. Press RESET on its own, then single-press BOOT within 5 seconds. Run `dfu-util -l` again; the name should now start with `@Flash /0x90000000`.

**`No DFU capable USB device available`, or the device disappears after a couple of seconds.**
RESET was pressed but BOOT was not, so the bootloader timed out and started the old firmware. Press RESET, then BOOT within 5 seconds, and retry.

**Nothing shows up at all.**
Try another USB cable; many are charge-only. Try a USB-A port or a different hub. Make sure the pedal is powered.

**The pedal starts but the LEDs are off.**
Firmware older than September 2026 booted bypassed. Tap the right footswitch. If the LED still does not light, flash the newest build from the Actions tab.
