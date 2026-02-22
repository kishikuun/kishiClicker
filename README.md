# 🖱️ Auto Clicker Pro (kishiClicker)

![C++20](https://img.shields.io/badge/C++-20-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20(Win32%20API)-lightgrey.svg)
![License](https://img.shields.io/badge/License-Copyright%202026-green.svg)

A highly optimized, zero-drift, stochastic auto-clicker built with modern C++20 and the native Win32 API. [cite_start]Developed under **kishi Studios**, this tool is designed for extreme precision, minimal footprint, and anti-detection capabilities.

## ✨ Key Features
* **Precision Timing Engine:** Utilizes `timeBeginPeriod` and hardware-level resolution guards for accurate 1ms interval execution.
* **Stochastic / Human-like Clicking:** Features a custom `StochasticTimer` with Gaussian noise and drift simulation (Random Offset) to bypass standard macro-detection systems.
* **Highly Customizable:** * Supports Left, Right, and Middle mouse buttons.
    * Click patterns: Single, Double, and Triple clicks.
* **Smart Hotkey System:** Global, thread-safe hotkey listener (Default: `F7`) with automatic conflict detection and dynamic remapping.
* **Persistent Configuration:** Automatically saves and loads user preferences using the Windows Registry.
* **Lightweight & Stealthy:** Native Win32 Dark Mode UI. Completely statically compiled, stripped of debug symbols, and optimized with dead-code elimination.

## 🛠️ Build Instructions
This project heavily utilizes C++20 concurrency features (`std::jthread`, `std::stop_token`) and requires a modern compiler like **GCC 15.2+ (MinGW-w64)**.

### 1. Compile Resources
First, compile the resource file containing the UI icons and version metadata:
```bash
windres resources.rc -O coff -o resources.res
```

### 2. Compile the Application (Release Build)
Build the final executable using extreme optimization flags (LTO, dead-code elimination, and static linking) for the smallest possible footprint:
```bash
g++ main.cpp resources.res -o "kishiClicker.exe" -std=c++20 -O3 -flto -s -mwindows -ffunction-sections -fdata-sections -Wl,--gc-sections -lcomctl32 -lgdi32 -luser32 -lkernel32 -lwinmm -static
```

🚀 Usage
1. Launch kishiClicker.exe.
2. Set your desired base interval (Hours, Minutes, Seconds, Milliseconds).
3. (Optional but recommended) Enable Random Offset to add human-like variance to the click rhythm.
4. Select the Mouse Button and Click Type.
5. Press the designated Hotkey (or click START) to begin execution. Press again to STOP.

👤 Author: kishikuun 
