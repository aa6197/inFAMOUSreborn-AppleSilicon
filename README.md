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
  <a href="#credits">Credits</a>
</p>

## What is this?

This is a Mac and iPad port of [inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn), the native PC port of inFamous. The game's PS3 code is recompiled ahead of time into native ARM code, so it runs as a normal app. No emulator, no Rosetta, no JIT.

- **Works now:** inFamous (Europe, `BCES00220`)
- **Not yet:** other regions
- **Tested on:** a Mac with an M5 Pro (30+ fps) and an iPad Pro 11-inch (M4, about 16 fps)
- **iPhone:** runs in the Simulator only so far
- **Game files are not included.** You bring your own disc image, and the game is built from it on your own Mac

Same idea as inFAMOUSreborn on Windows. You give it your copy of the game, and it rebuilds the game code from it.

## Install

There's no download. The finished app contains the game's code, so it can't be shared. You build it yourself on a Mac, from your own copy.

You need:

- A Mac with Apple Silicon (M1 or newer). Tested on macOS 27
- Xcode and [Homebrew](https://brew.sh)
- [RPCS3](https://rpcs3.net) for Mac. It's only used once, to unlock the game's program file
- About 10 GB free and 16 GB of RAM

1. Install the tools
   ```bash
   brew install cmake ninja python@3.12 rust sdl3 glslang spirv-cross
   ```
2. Get the code
   ```bash
   git clone -b apple-port https://github.com/SubhanHabib/inFAMOUSreborn.git
   cd inFAMOUSreborn
   ```
3. Build the installer
   ```bash
   cd installer && cargo build --release && cd ..
   ```

## Add your game

You need a disc image of inFamous (European release, `BCES00220`) as a `.iso` file.

1. Copy the game out of your disc image
   ```bash
   installer/target/release/infamous-installer extract-iso --iso "/path/to/inFamous.iso" --out ~/inFamous/BCES00220
   ```
   - If your image is encrypted, add its key: `--dkey "/path/to/inFamous.dkey"`
2. Build the game
   ```bash
   installer/target/release/infamous-installer install --game ~/inFamous/BCES00220 \
     --rpcs3 /Applications/RPCS3.app/Contents/MacOS/rpcs3 \
     --python /opt/homebrew/bin/python3.12 -y
   ```
   - It checks your game, rebuilds the game code from it, then compiles it. This is the long part
   - If your Mac runs low on memory, add `--build-jobs 2`
   - If it stops, run it again. It carries on where it left off
3. Start the game
   ```bash
   ./play-infamous.sh
   ```

If it says it's the wrong disc, it's probably a different region. Only the European release works for now.

### iPad

It runs on an iPad Pro with M4, at about half speed for now. The iPad build steps aren't written up yet. You'll need a Mac with Xcode, and it has only been tested with a paid Apple Developer account. The game files go into the app's folder (Finder → your iPad → Files → inFamous).

## Playing

- Connect a controller: MFi, Xbox, DualShock 4 and DualSense should all work
- On iPad, touch controls show up when no controller is connected
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

Please report bugs here, not on inFAMOUSreborn. The original project doesn't support forks.

For the nerdy stuff (what was changed for ARM and Metal, speed tests, every switch) see [PORT_LOG.md](PORT_LOG.md).

## Credits

This is built on top of [inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn) by whompay. The Windows port, the installer and the patches that make it possible are theirs. Go give them a star.

Also uses:

- [ps3recomp](https://github.com/sp00nznet/ps3recomp), the PS3 static recompiler
- [SDL](https://github.com/libsdl-org/SDL)
- [glslang](https://github.com/KhronosGroup/glslang) and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), to turn the game's shaders into Metal
- [sse2neon](https://github.com/DLTcollab/sse2neon), for the vector code on ARM
- [RPCS3](https://rpcs3.net), to unlock the game's program file

This is an unofficial fan project. It isn't affiliated with or endorsed by Sony Interactive Entertainment or Sucker Punch Productions. All trademarks belong to their owners.
