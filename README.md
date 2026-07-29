# NarutoStormGeneration

**NarutoStormGeneration** is a native Windows recompilation of **Naruto Shippuden: Ultimate Ninja Storm Generations** built using a custom **ReXGlue** backend.

The project aims to bring the original Xbox 360 experience to modern Windows PCs while preserving the original gameplay. The game is fully playable, with online multiplayer currently under active development.

---

# Installation

## Download a Release

Download the latest release from the **Releases** page and extract it.

Place **NarutoStormGeneration.exe** in the same folder as **default.xex**.

Example folder layout:

```text
Game Folder/
├── NarutoStormGeneration.exe
├── default.xex
├── game.xex
└── ...
```

Launch **NarutoStormGeneration.exe** to start the game.

## Build From Source

Clone the repository and run:

```bat
build.bat
```

After the build finishes, copy **NarutoStormGeneration.exe** into the same folder as **default.xex**, then launch the game.

---

# Features

* Native Windows executable
* Fully playable game
* Custom ReXGlue backend
* Native controller support
* Modern PC launcher
* PC settings menu
* Windowed, Borderless, and Fullscreen modes
* Resolution selection
* Adjustable frame-rate options
* Native save support
* Runtime logging
* Faster loading times
* Performance optimizations

---

# Roadmap

The following features are currently being worked on:

* Online multiplayer
* Mod support
* Additional graphics options
* Ultrawide support
* Launcher improvements
* Additional performance optimizations
* Additional quality-of-life improvements

---

# Building

To build the project yourself:

```bat
build.bat
```

The build script automatically configures the project, generates the required files, and builds the executable.

---

# Legal

You **must legally own** a copy of **Naruto Shippuden: Ultimate Ninja Storm Generations** to use this project.

This repository does **not** include any game files or copyrighted assets.

Please do not ask for game files or other copyrighted content.

---

# Disclaimer

NarutoStormGeneration is an independent fan-made preservation project.

It is **not affiliated with or endorsed by** Bandai Namco Entertainment, CyberConnect2, Microsoft, Xbox, or the ReXGlue project.

All trademarks and copyrights belong to their respective owners.

---

# Credits

NarutoStormGeneration is built using a custom branch of **ReXGlue**.

This project builds upon the work of several outstanding open-source projects:

* **ReXGlue**
  https://github.com/rexglue/rexglue-sdk

* **Project Xenia**
  https://github.com/xenia-project/xenia

* **XenonRecomp**
  https://github.com/hedge-dev/XenonRecomp

* **XenonRecomp-SonicUnleashed**
  https://github.com/hedge-dev/XenonRecomp-SonicUnleashed

* **rexdex's Xbox 360 Recompiler**
  https://github.com/rexdex/recompiler

Thank you to the developers and contributors behind these projects and to the Xbox 360 reverse engineering and preservation community for making projects like this possible.
