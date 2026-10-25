# Pocket Retro 👾

> Built with AI assistance, tested on real hardware.

A Galaga-style vertical shooter for the **LilyGO T-Display C5** board
(the little board with the 1.9-inch screen and two buttons).

Your ship sits at the bottom of the screen. Enemies come down from the
top in formation. Shoot them all to clear the level — then the next
level comes faster.

## How to play

- **Left button** = move the ship left
- **Right button** = move the ship right
- **The ship shoots by itself.** There is no fire button — auto-fire is
  always on. That is why two buttons are exactly enough.
- **3 lives.** If an enemy reaches your ship's line, you lose a life.
- **Score:** top-row enemies are worth 30 points, middle row 20,
  bottom row 10. Clearing a level gives a 100-point bonus.
- **Game over screen:** press either button to play again.
- **Title screen:** press either button to start.

## What you need

- The LilyGO T-Display C5 board
- A USB-C cable that carries **data**, not just power
  (a charge-only cable will not work — if the computer cannot see
  the board, try another cable first)
- A Windows computer
- A built merged flash file (see “How to build it yourself” below — this repo holds source only; prebuilt `.bin` files are not included)

## How to flash the game (the easy way)

First build the game (see “How to build it yourself” below), then flash the merged file you built.

### Step 1 — Install Python and esptool (one time only)

1. Go to **python.org**, download Python, and install it. On the
   very first install screen, tick the box that says
   **"Add python.exe to PATH"**, then click Install.
2. Open **Command Prompt** (press the Windows key, type `cmd`,
   press Enter).
3. Type this and press Enter:

```
pip install esptool
```

### Step 2 — Find the board's COM port

1. Plug the board into the computer with the USB-C cable.
2. Open **Device Manager** (press the Windows key, type
   `Device Manager`, press Enter).
3. Look under **Ports (COM & LPT)**. You should see something like
   **USB Serial Device (COM3)**. Remember the number — yours might
   be COM4, COM5, or something else. Use your number in the next step.

### Step 3 — Put the board into download mode

Most of the time you can skip this step — just try Step 4 first.
If Step 4 fails, do this:

1. Hold down the **BOOT** button (one of the two small buttons
   next to the USB-C port).
2. While still holding BOOT, press and release the **Reset** button.
3. Let go of the BOOT button.

The board is now waiting for new firmware.

### Step 4 — Flash it

In Command Prompt, type this (change COM3 to your port number)
and press Enter:

```
esptool.py --chip esp32c5 --port COM3 write_flash --flash_mode dio --flash_size 16MB 0x0 pocket-retro-merged.bin
```

You must run this command from the folder where the .bin file is,
or type the full path to the file instead of just its name.

When it says the write is done, unplug the board and plug it back
in (or press Reset). The title screen should appear:
**POCKET RETRO — PRESS A BUTTON**.

> Prefer buttons over commands? The same file works with
> Espressif's **Flash Download Tool** (a free program with a
> window and buttons): pick chip ESP32-C5, load
> `pocket-retro-merged.bin` at address `0x0`, and click Start.

## First checks (do these right after flashing)

**1. Is the picture the right way up?**
The title screen should read normally with the USB-C port at the
bottom. If the picture is sideways or upside down, tell Spliff —
the screen rotation setting in the code needs one change.

**2. Does left mean left?**
Hold the board with the screen facing you and the USB-C port at
the bottom. Press the **left** button — the ship should move
left. If it moves right instead, the two buttons are swapped.
The fix is one line in `main/main.c`: swap the two numbers in
these lines, rebuild, and flash again:

```c
#define BTN_LEFT_GPIO   0   /* User button */
#define BTN_RIGHT_GPIO  28  /* BOOT button */
```

**3. Does the ship shoot by itself?**
It should, about 5 shots a second, without you pressing anything.
If nothing shoots, something is wrong — tell Spliff.

**4. Colors look wrong?**
The ship should be green, the enemies red, orange, and yellow,
the bullets cyan-blue. If everything looks like a photo negative
(black and white swapped), the color-invert setting needs
flipping — tell Spliff.

## If the screen stays black

First, check the easy things: a data USB cable (not charge-only),
the COM port number, and try download mode (Step 3 above).

If it is still black, there is one known puzzle with this board:
LilyGO's own example code disagrees with LilyGO's own pin table
about two display wires (chip-select and reset). This game uses
the pin table (CS = GPIO 26, RST = GPIO 23), because that table
does not clash with the button or the battery chip. If your
screen stays black, the fallback is to try the example's values
(CS = GPIO 3, RST = GPIO 0) in `main/main.c` — but only try
that if the screen is dead, because those values clash with the
User button on paper.

## How to build it yourself (optional)

You only need this if you want to change the game. It needs
ESP-IDF (Espressif's own toolkit) version 5.3 or newer on Windows:

1. Install ESP-IDF for Windows (follow the guide at
   docs.espressif.com — pick the "ESP-IDF Tools Installer").
2. Open the **ESP-IDF Command Prompt** (it is installed with
   the tools) and go into this project folder.
3. Run:
   ```
   idf.py set-target esp32c5
   idf.py build
   ```
4. Flash with:
   ```
   idf.py -p COM3 flash
   ```
   (again, change COM3 to your port number)

The finished game file lands in `build/pocket-retro.bin`, and
the all-in-one file (the one from the easy steps above) can be
made with `esptool.py merge_bin` — see the build notes at the
end of this file.

## How this game was made

- **Board:** LilyGO T-Display C5 — ESP32-C5 chip, 1.9-inch ST7789
  screen, 170 x 320 pixels, 16 MB flash, 8 MB memory.
- **Pins:** taken from LilyGO's official T-Display-C5 repository
  pin table (github.com/Xinyuan-LilyGO/T-Display-C5):
  display CS = 26, clock = 7, data = 9, data/command = 8,
  reset = 23, backlight = 25; User button = 0, BOOT button = 28.
- **Toolkit:** ESP-IDF (version 6 here) with its built-in ST7789
  screen driver. An Arduino version was not built, because
  support for the ESP32-C5 in the usual screen libraries
  (TFT_eSPI, LovyanGFX) could not be confirmed, and the ESP-IDF
  build is the one that was actually compiled and checked.
- **Flashing over the factory demo:** flashing this game replaces
  the battery demo that came on the board. That is normal and
  expected. LilyGO's factory firmware is in their repository
  (the `firmware` folder) if you ever want it back.

## What is tested and what is not (honest list)

- **Compiled:** yes, this code compiled cleanly with ESP-IDF
  v6.0.1 for the ESP32-C5 target on the build machine (see
  "Build notes" below). No compile errors. The game itself is
  218,096 bytes; the all-in-one flash file is 283,632 bytes.
- **Tested on a real board:** flashed and played on a real LilyGO
  T-Display C5 (October 2026). The title screen reads correctly —
  a mirror setting was fixed after the first on-device test, when
  the text showed up mirrored.
- **Known limits of this first version:**
  - Enemies do not shoot back (kept simple on purpose).
  - High score is not saved — it resets when the power goes off.
  - No sound (the board has no speaker fitted).
  - No Wi-Fi or Bluetooth is used — the game is fully offline.

## Build notes (for the record)

- Built with ESP-IDF v6.0, target `esp32c5`, from this folder.
- Display: SPI2, 40 MHz, ST7789 driver from ESP-IDF, portrait
  170 x 320, mirror X on, color invert on, gap (35, 0) — the
  same orientation settings as LilyGO's own lcd.ino example.
- The game draws into a full-screen buffer in internal RAM and
  sends it to the screen once per frame, at about 30 frames
  per second.
- Merged flash file layout (for `esptool.py merge-bin`):
  bootloader at 0x2000, partition table at 0x8000, app at
  0x10000. Flash mode DIO, 16 MB, 80 MHz — those are the
  settings the build itself produced, and the ones the
  all-in-one file was made with. (Note for the curious: on
  this chip the bootloader lives at 0x2000, not 0x0 — that
  is normal for the ESP32-C5.)
