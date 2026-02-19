# VR Shadow Cascade Pre-loader

A dinput8.dll proxy that loads before Fallout 4 VR's static initialization and expands the VR shadow cascade allocation from 2 to 4 cascades.

## Purpose

Fallout 4 VR only allocates 2 shadow cascades (compared to 4 in flat FO4), which limits shadow quality at distance. This pre-loader intercepts the cascade buffer allocation and expands it to 4 cascades, allowing the game to initialize all 4 with proper GPU resources.

## How It Works

1. **Early Loading**: As a dinput8.dll proxy, this DLL loads very early in the game's startup, before static initialization
2. **Allocator Hook**: Hooks the game's memory allocator (FUN_141b91950)
3. **Allocation Interception**: When the cascade buffer allocation (0x300 bytes = 2 * 0x180) is detected, it allocates 0x600 bytes instead (4 * 0x180)
4. **Count Patching**: After initialization, patches the cascade count from 2 to 4

## Building

```powershell
cd "C:\Development\F4VR mod template\VRShadowCascadePreloader"
cmake -B build -S .
cmake --build build --config Release
```

## Installation

1. Build the DLL (outputs to `build/bin/dinput8.dll`)
2. Copy `dinput8.dll` to your Fallout 4 VR game directory (same folder as `Fallout4VR.exe`)
3. The pre-loader will automatically load when the game starts

## Logs

The pre-loader writes logs to `VRShadowCascade.log` in the game directory. Check this file to verify the patch is working:

```
VR Shadow Cascade Pre-loader v1.0.0
Initializing...
Game module base: 0x140000000
Allocator function at: 0x141B91950
Trampoline created at: 0x...
Allocator hook installed successfully
Waiting for cascade allocation (size 0x300)...
Allocation of 0x300 bytes from offset 0x...
  -> Likely cascade allocation!
Intercepted cascade allocation: 0x300 -> 0x600 bytes
Cascade allocation expanded successfully at 0x...
Cascade count patch thread started
Current cascade count: 2
Patched cascade count: 2 -> 4
Cascade count patch thread finished
```

## Compatibility

- **Game Version**: Fallout 4 VR 1.2.72
- **Requirements**: Must be used alongside an F4SE plugin that allows 4-cascade shadow masks (otherwise the extra cascades won't be used)

## Technical Details

### Offsets (FO4VR 1.2.72)

| Name | Offset | Description |
|------|--------|-------------|
| Allocator | 0x1B91950 | Memory allocator function |
| CascadeArrayPtr | 0x6878B18 | Pointer to cascade array (DAT_146878b18) |
| CascadeCount | 0x6878B28 | Cascade count variable (DAT_146878b28) |

### Cascade Structure

Each cascade is 0x180 bytes containing:
- Thread synchronization primitives
- GPU shadow map resources
- Render target pointers

## Troubleshooting

**"Cascade allocation was not detected"**
- The game version may be different
- Check if VR mode is actually active
- Look at the log for allocations of 0x300 bytes and their offsets

**Game crashes on startup**
- The hook may be incompatible with the game version
- Try removing dinput8.dll and check if game works normally
- Compare offsets against a disassembly of your game version

## Credits

Part of the Fallout 4 VR Shadow Boost project.
