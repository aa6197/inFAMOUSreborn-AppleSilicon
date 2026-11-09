<h1 align="center">inFamous for Mac and iPad</h1>

<p align="center">
  <strong>inFamous on your Mac and iPad, running natively.</strong><br>
  The PS3 game, recompiled ahead of time into native Apple Silicon code and drawn with Metal. Not an emulator.
</p>

<p align="center">
  <a href="https://github.com/whompay/inFAMOUSreborn"><img src="https://img.shields.io/badge/based%20on-inFAMOUSreborn-e8a317" alt="Based on inFAMOUSreborn"></a>
  <a href="https://github.com/sp00nznet/ps3recomp"><img src="https://img.shields.io/badge/uses-ps3recomp-e8a317" alt="Uses ps3recomp"></a>
</p>

<p align="center">
  <a href="#install">Install</a> ·
  <a href="#add-your-game">Add your game</a> ·
  <a href="#playing">Playing</a> ·
  <a href="#status">Status</a> ·
  <a href="#build-it-yourself">Build it yourself</a> ·
  <a href="#credits">Credits</a>
</p>

## What is this?

This is a Mac and iPad port of [inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn), the native PC port of inFamous. The game's PS3 code is recompiled ahead of time into native ARM code, so it runs as a normal app. No emulator, no Rosetta, no JIT.

- **Works now:** inFamous (Europe, `BCES00220`)
- **Not yet:** other regions
- **Tested on:** a Mac with an M5 Pro (30+ fps) and an iPad Pro 11-inch (M4, about 20 fps)
- **iPhone:** runs in the Simulator only so far
- **Game files are not included.** You choose your own copy of the game when the app first starts. The app copies what it needs from it

## Install

Download the app from the [Releases](https://github.com/SubhanHabib/inFAMOUSreborn-AppleSilicon/releases) page. You don't need any developer tools.

### Mac

You need a Mac with Apple Silicon (M1 or newer) and macOS 13 or newer.

1. Download `inFamous-<version>-mac.zip` and open it
2. Drag `inFamous` into your Applications folder
3. Open it. The first time, macOS says it can't check the app. This is normal: the app isn't from the App Store
   - **macOS 15 or newer:** open System Settings → Privacy & Security. Scroll down and click **Open Anyway**
   - **macOS 13 or 14:** right-click the app, choose **Open**, then **Open** again

You only do this once.

### iPad

You need an iPad with an M-series chip (M1 or newer).

1. Download `inFamous-<version>-ios.ipa`
2. Install it with [SideStore](https://sidestore.io), [AltStore](https://altstore.io) or [Sideloadly](https://sideloadly.io)
   - With a free Apple ID you must refresh the app every 7 days. The tool does this for you
3. Open it

The app needs two memory permissions from Apple ("Extended Virtual Addressing" and "Increased Memory Limit"). It has only been tested with a paid Apple Developer account. With a free Apple ID these may not be granted, and then the app can't start. If that happens to you, please open an issue.

## Add your game

You need your own copy of inFamous (European release, `BCES00220`). One of these:

- a disc image (`.iso`)
- a game folder (the one with `PS3_GAME` inside)

The first time you open the app it asks for it:

1. Click **Choose disc image or folder** and pick your copy
2. Wait while it copies the game. It takes a few minutes and needs about 7.5 GB free
3. The game starts. Next time it starts straight away

Your own file isn't changed.

- **Encrypted disc image** (a 1:1 copy of the disc): it also needs the disc key (`.dkey`). Put the key next to the image with the same name and it's found by itself. If not, the app asks you for it
- **On iPad:** put the disc image on the iPad first. Use the Files app, a USB drive, or Finder → your iPad → Files. Then pick it in the app
- **Wrong disc?** It's probably a different region. Only the European release works for now

## Playing

- Connect a controller: MFi, Xbox, DualShock 4 and DualSense should all work
- **On Mac you need a controller for now.** Keyboard controls aren't done yet
- On iPad, touch controls show up when no controller is connected
- Your saves and settings are kept in the app's folder. Mac: `~/Library/Application Support/inFamous`. iPad: Files → On My iPad → inFamous
- The game runs in English

Controllers and touch controls have only been lightly tested. If yours doesn't work, open an issue and say which one.

## Status

| | Mac (M5 Pro) | iPad Pro 11 (M4) | iPhone |
|---|---|---|---|
| Runs | Yes | Yes | Simulator only |
| Speed | 30+ fps | about 16 fps | not tested |

Known issues:

- 3D surfaces close to the camera look too bright
- The iPad runs at about half speed
- Only tested on one Mac so far. Older chips (M1, M2) may not reach 30 fps

Please report bugs here, not on inFAMOUSreborn. This port isn't part of the original project.

For the nerdy stuff (what was changed for ARM and Metal, speed tests, every switch) see [PORT_LOG.md](PORT_LOG.md).

## Build it yourself

For developers. Most people should use the [download](#install) instead.

You need a Mac with Apple Silicon, Xcode, [Homebrew](https://brew.sh), your own disc image, and [RPCS3](https://rpcs3.net) for Mac (used once, to unlock the game's program file). About 10 GB free and 16 GB of RAM.

1. Install the tools
   ```bash
   brew install cmake ninja python@3.12 rust sdl3 glslang spirv-cross
   ```
2. Get the code and build the installer
   ```bash
   git clone https://github.com/SubhanHabib/inFAMOUSreborn-AppleSilicon.git
   cd inFAMOUSreborn-AppleSilicon
   cd installer && cargo build --release && cd ..
   ```
3. Copy the game out of your disc image (add `--dkey <file>` for an encrypted image)
   ```bash
   installer/target/release/infamous-installer extract-iso --iso "/path/to/inFamous.iso" --out ~/inFamous/BCES00220
   ```
4. Build the game. It checks your game, rebuilds the game code from it, then compiles it. If it stops, run it again
   ```bash
   installer/target/release/infamous-installer install --game ~/inFamous/BCES00220 \
     --rpcs3 /Applications/RPCS3.app/Contents/MacOS/rpcs3 \
     --python /opt/homebrew/bin/python3.12 -y
   ```
5. Start the game
   ```bash
   ./play-infamous.sh
   ```

The release apps are made by `infamous-recomp/tools/release_apple.sh`. See [PORT_LOG.md](PORT_LOG.md) for the release build, the iPad build and every switch.

## Credits

This is built on top of [inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn) by whompay. The Windows port, the installer and the patches that make it possible are theirs. Go give them a star.

Also uses:

- [ps3recomp](https://github.com/sp00nznet/ps3recomp), the PS3 static recompiler
- [SDL](https://github.com/libsdl-org/SDL)
- [glslang](https://github.com/KhronosGroup/glslang) and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), to turn the game's shaders into Metal
- [sse2neon](https://github.com/DLTcollab/sse2neon), for the vector code on ARM
- [RPCS3](https://rpcs3.net), to unlock the game's program file

This is an unofficial fan project. It isn't affiliated with or endorsed by Sony Interactive Entertainment or Sucker Punch Productions. All trademarks belong to their owners.
