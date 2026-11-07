# Darkest Dungeon for PSP

A fan-made demake of **Darkest Dungeon** for the Sony PSP. It is a from-scratch game engine written in C that plays the original game's heroes, monsters, skills, dungeons, town and music on a PSP's 480x272 screen.

This project contains no game files. You supply your own copy of Darkest Dungeon from Steam, and a small extractor tool converts the art, sound and data into a single file, `assets.pak`, that the game reads. You end up with two files on your memory stick:

| File | What it is | Where it comes from |
|---|---|---|
| `EBOOT.PBP` | the game program (our code only) | download from the releases page (see step 6) |
| `assets.pak` | art, sound and game data from your copy | you build it with the extractor (steps 1-5) |

It is an unofficial fan project and is not affiliated with or endorsed by Red Hook Studios. It only works with a genuine copy of the original game (Darkest Dungeon 1, not Darkest Dungeon II). Please do not share `assets.pak` or any files extracted from the game.

## What you need

- A PSP running custom firmware (for example ARK-4 or PRO) that can launch homebrew from the memory stick.
- A PC (Windows or Linux; macOS should work too) with **Darkest Dungeon** installed from Steam.
- About 2 GB of free disk space during extraction. The finished `assets.pak` is around 65 MB.

The extractor has been tested on Linux. Windows and macOS should work but have not been tested yet.

## Step 1: Find your Darkest Dungeon folder

In Steam, right-click Darkest Dungeon, then Manage, then Browse local files. Typical locations:

- Windows: `C:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon`
- Linux: `~/.steam/steam/steamapps/common/DarkestDungeon`
- macOS: `~/Library/Application Support/Steam/steamapps/common/DarkestDungeon`

The folder should contain subfolders named `heroes`, `monsters`, `audio`, `shared` and `fonts`.

## Step 2: Install Python and Pillow

The extractor is a Python program with a small window. You need Python 3.9 or newer with Tk (the window toolkit) and the Pillow image library.

- **Windows:** install Python from https://www.python.org/downloads/ . On the first installer screen tick "Add python.exe to PATH". Tk is included. Then open Command Prompt and run:
  ```
  py -m pip install pillow
  ```
- **Ubuntu / Debian / Linux Mint:**
  ```
  sudo apt install python3 python3-tk python3-pip python3-pil
  ```
- **Fedora:**
  ```
  sudo dnf install python3 python3-tkinter python3-pillow
  ```
- **macOS (Homebrew):**
  ```
  brew install python python-tk
  python3 -m pip install pillow
  ```

Check it worked: `python3 -c "import tkinter, PIL; print('ok')"` (on Windows use `py` instead of `python3`). It should print `ok`.

## Step 3: Get vgmstream-cli (decodes the game's sound banks)

Download the command-line build for your system from https://github.com/vgmstream/vgmstream/releases/latest (look for the `win64`, `linux-cli` or `mac-cli` file). Unzip it somewhere you will remember. The program you will point the extractor at is `vgmstream-cli` (`vgmstream-cli.exe` on Windows).

## Step 4: Get ffmpeg with the libvorbis encoder (compresses the sounds)

The sounds are stored as Ogg Vorbis, so your ffmpeg must include the `libvorbis` encoder. Most full builds do.

- **Windows:** download a "full" build from https://www.gyan.dev/ffmpeg/builds/ (the `ffmpeg-release-full` zip) or from https://github.com/BtbN/FFmpeg-Builds/releases . Unzip it. The program is `bin\ffmpeg.exe`.
- **Ubuntu / Debian / Linux Mint:** `sudo apt install ffmpeg`
- **Fedora:** install ffmpeg from RPM Fusion (`sudo dnf install ffmpeg`).
- **macOS:** `brew install ffmpeg`

Check that it has the encoder:

```
ffmpeg -encoders | grep libvorbis        (Windows Command Prompt: ffmpeg -encoders | findstr libvorbis)
```

You should see a line mentioning `libvorbis`. The extractor also checks this for you.

## Step 5: Get SpineConverter2.1.27 (reads the game's animations)

The game's character animations are in Spine's binary format. A small Python tool called SpineConverter2.1.27 reads them. Download it from: **[SPINECONVERTER LINK GOES HERE]**

Unzip it. The folder you will pick in the extractor is the one that contains `spBinaryReader.py`.

## Step 6: Download the extractor and the EBOOT

Download both from the project's releases page: **[LINK TO THIS PROJECT'S RELEASES GOES HERE]**

- `DDExtractor.zip` is the extractor tool. Unzip it anywhere.
- `EBOOT.PBP` is the game program for the PSP.

## Step 7: Run the extractor

- **Windows:** double-click `run.bat` inside the unzipped `DDExtractor` folder.
- **Linux / macOS:** open a terminal in that folder and run `./run.sh` (or `python3 darkest/extractor/extractor.py`).

The window has five boxes. Fill them in from top to bottom; each gets a green check mark when it looks right.

1. **Darkest Dungeon folder:** the folder from step 1 (it may be filled in automatically).
2. **vgmstream-cli:** the program from step 3.
3. **ffmpeg:** the program from step 4.
4. **SpineConverter2.1.27 folder:** the folder from step 5.
5. **Save assets.pak as:** where to write the result.

Press **Extract**. It takes a few minutes and shows progress as it works through the game data, sprites, interface art, town, sounds and music. When it finishes you have an `assets.pak` file.

If you prefer the command line:

```
python3 darkest/extractor/extractor.py --cli "<Darkest Dungeon folder>" \
    --vgmstream <path to vgmstream-cli> --ffmpeg <path to ffmpeg> \
    --spine <SpineConverter folder> --out assets.pak
```

## Step 8: Put the game on your PSP

1. Connect your PSP (or take out the memory stick) and open the `PSP/GAME/` folder.
2. Create a new folder in it, for example `DarkestDemake`.
3. Copy **`EBOOT.PBP`** and **`assets.pak`** into that folder. Both files must be in the same folder.
4. On the PSP, open Game, then Memory Stick, and launch Darkest Dungeon.

The game writes its save file (`save.bin`) and a diagnostic `log.txt` into the same folder. Delete `save.bin` to start a new campaign.

## Optional: your own menu icon and background

The picture shown in the PSP menu is stored inside `EBOOT.PBP`. In the extractor window open the **Menu art (optional)** tab, pick your `EBOOT.PBP`, a 144x80 PNG for the icon and a 480x272 PNG for the background (other sizes are resized), and press **Put art into EBOOT**. This edits the EBOOT in place, so keep a copy first. From the command line:

```
python3 darkest/extractor/xmb_patch.py EBOOT.PBP --icon icon.png --background background.png
```

## Troubleshooting

- **PSP shows "The game data was not found in assets.pak":** `assets.pak` is missing, is in a different folder from `EBOOT.PBP`, or was made by a different version of the extractor. Re-run the extractor from the same release as your EBOOT.
- **The extractor says a box is wrong:** read the message under it. "not a Darkest Dungeon folder" means you picked the wrong folder; "no libvorbis encoder" means your ffmpeg build is too minimal (use a full build from step 4).
- **There is no sound:** the extraction ran with an ffmpeg that cannot encode Vorbis, or vgmstream-cli was the wrong program. Fix step 3 or 4 and run the extractor again.
- **Brief stutters the first time a sound plays:** the PSP is decoding that sound for the first time. It stays loaded afterwards.
- **Something else:** open `log.txt` in the game folder on the memory stick. It records start-up, memory use and audio details; include it if you report a problem.

## For developers

Source code layout:

| Path | What |
|---|---|
| `darkest/` | the game: CMake project (open in CLion). `src/core` portable C, `src/platform/psp` PSP, `src/platform/host` PC test build |
| `darkest/tools/` | Python converters, `ddbin.py` (data tables to binary), `deploy.sh` |
| `darkest/extractor/` | the extractor (window and `--cli`), `build_release.sh`, `xmb_patch.py` |
| `tools/` | earlier converter scripts and `third_party/` |
| `research/`, `darkest/docs/` | reverse-engineering notes, recovered game rules, tutorial |

Build the PSP EBOOT (needs the [PSPDEV toolchain](https://pspdev.github.io/)):

```
cmake -S darkest -B darkest/cmake-build-psp
cmake --build darkest/cmake-build-psp
```

Menu art is embedded from `extracted/xmb/` if it exists; add `-DDD_XMB=/nonexistent` for a build with no art. Build the PC test version with `-DDD_HOST=ON`. Build the extractor release zip with `sh darkest/extractor/build_release.sh`.