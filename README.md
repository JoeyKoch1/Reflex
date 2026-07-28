# Reflex

Lightweight CS2 internal cheat — chams, night mode, hit sound, hit logger.

## Features

- **Chams** — visible & occluded player coloring via `GeneratePrimitives` hook
- **Night mode** — material color override for darker environments
- **Hit sound** — Neverlose-style sound on hit
- **Hit logger** — damage numbers in chat with team-colored names
- **Watermark** — green "Reflex" overlay at top-right

## Build

```
cd build
cmake ..
MSBuild Qernix.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `build/Release/Qernix.dll`

## Usage

1. Launch CS2 (Steam x64)
2. Wait for game to fully load
3. Inject the DLL (LoadLibrary injection)
4. Press **END** to unload

## Requirements

- Windows 10 / 11 x64
- VC++ Redist (included with Windows)
- CS2 on Steam

## Dependencies

- [MinHook](https://github.com/TsudaKageyu/minhook) — hooked as git submodule

## Disclaimer

For educational purposes only. Use at your own risk.
