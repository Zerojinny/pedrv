# pedrv

[한국어](README.md) | **English**

**Install only the drivers that match this PC's hardware, from WinPE**

Give pedrv a driver folder or archive. It matches the INF files against the hardware IDs of the devices present and installs only the drivers that are needed. The target is either an offline Windows installation (for example `C:\` during recovery or deployment) or the running WinPE itself.

- Driver sources: INF folders, `.zip`, `.7z`, `.wim`, and the driver store of a Windows installed on disk
- Picks the single best driver per device (ID rank → date → version)
- Single executable (static CRT, no .NET or PowerShell needed)
- Shrinks multi-GB graphics drivers so they fit on the WinPE RAM disk
- Console messages are in English (WinPE usually has no Korean font)

## Download

Get the file for your PE's architecture from [Releases](https://github.com/Zerojinny/pedrv/releases) and rename it to `pedrv.exe` (`pedrv-x64.exe`, `pedrv-x86.exe`, `pedrv-arm64.exe`). To use `.zip` or `.7z` sources, put 7-Zip (`7z.exe`, `7z.dll`) in the same folder (see "Requirements").

## Quick start

```bat
:: Install drivers for this PC into an offline Windows (target is detected automatically)
pedrv D:\Drivers

:: Show which drivers would be selected, without installing
pedrv /list D:\Drivers

:: Load drivers into the running WinPE (e.g. a graphics driver for a higher resolution)
pedrv /pe D:\Drivers

:: Load the drivers of the Windows installed on disk into WinPE
pedrv /fromwin
```

Without a source, pedrv uses the `Drivers` folder next to `pedrv.exe`.

## Usage by target

| Goal | Command |
|---|---|
| Install into an offline Windows | `pedrv [/image:C:\] <source...>` |
| Install into the running WinPE | `pedrv /pe <source...>` |
| Load the drivers of the Windows on disk into WinPE | `pedrv /fromwin[:C:\]` |
| Save slimmed PE drivers to a folder | `pedrv /export:<folder> <source...>` |

### Offline Windows (`/image`)

Adds drivers to the offline image with the DISM API. Without `/image`, pedrv looks for Windows on fixed disks; if it finds more than one, it lists them and stops.

### Running WinPE (`/pe`)

Loads drivers with `drvload`. Once a controller driver is loaded, the devices behind it appear, so pedrv repeats up to 5 times until there is nothing more to load. If a device still has a problem afterwards, it reports `WARNING: device not working (code N)`.

### Windows on disk → WinPE (`/fromwin`)

Uses the driver store of the Windows installed on disk as the source. By default only vendor (third-party) drivers are used; add `/inbox` to include the drivers that ship with Windows. This loads the drivers that PC already uses into PE, with no driver files to prepare.

## Options

| Option | Description |
|---|---|
| `/image:<path>` | Offline Windows root. Detected automatically if omitted |
| `/pe` | Install into the running WinPE |
| `/fromwin[:<path>]` | Install into WinPE from the driver store of the Windows on disk. `/inbox` includes inbox drivers |
| `/list` | Only show the drivers that would be selected |
| `/all` | Install every driver in the sources, without hardware matching |
| `/allmatches` | Install every matching driver instead of one per device |
| `/hwonly` | Match hardware IDs only (ignore compatible IDs) |
| `/exclude:<class,...>` | Exclude device classes (e.g. `/exclude:Display,Media`) |
| `/slim` | Always use a slimmed copy when loading into WinPE |
| `/slim:max` | Like `/slim`, and also empties the NVIDIA and AMD OpenGL and Vulkan drivers. Also applies to `/export` |
| `/export:<folder>` | Save slimmed copies to a folder instead of installing. `/keep32` keeps 32-bit files |
| `/forceunsigned` | Allow unsigned drivers (offline install) |
| `/scratch:<folder>` | Work folder. Default: the target Windows drive for offline installs; for `/pe`, the drive PE booted from (or `%TEMP%` on the RAM disk if that drive is not writable) |
| `/7z:<path>` | Location of 7-Zip |
| `/log:<file>` | DISM log file |
| `/version` | Show the version |

Select a WIM image index with `drivers.wim:2`. Archives directly inside a source folder are added as sources automatically. Use `?:` instead of a drive letter (`?:\PEDrivers`) to search C: through Z: and use the first drive that has the path. This helps with boot media whose drive letter differs from PC to PC.

Exit codes: `0` success · `1` some drivers failed or a device still has a problem · `2` usage or environment error · `3` canceled (Ctrl+C)

## Shrinking large graphics drivers

A single NVIDIA driver package is about 2.7 GB. Loading it into WinPE copies the whole package to the RAM disk, which does not fit on PCs with little memory.

pedrv builds a **slimmed copy** in which files PE does not need to bring up the display are replaced with 0-byte files, and loads that instead. Emptied files include WSL/Linux files, 32-bit files, INFs for other devices, CUDA, OptiX, DLSS and video codecs in NVIDIA packages, and FidelityFX, the AMF runtime and installer files in AMD packages. An NVIDIA package (about 2.7 GB) becomes about 730–780 MB, and an AMD package (about 1.2 GB) about 490 MB. The selected INF, `.cat` and `.sys` files are kept as is. Subfolders with their own INF (such as Intel child drivers) are treated as separate packages and left out of the copy.

- By default, only packages that do not fit on the RAM disk are slimmed. With `/slim`, packages are always slimmed.
- `/slim:max` also empties the OpenGL and Vulkan drivers (AMD about 330 MB). OpenGL and Vulkan programs will not work in that PE.
- `/export:<folder>` creates slimmed copies ahead of time.
- File lists per rule and measurements are in [docs/slim-display-drivers.md](docs/slim-display-drivers.md) (Korean).

> [!WARNING]
> Slimmed copies work **only when loaded into a booted WinPE** (`pedrv /pe`, `drvload`).
> - **They do not work when integrated into the PE image (boot.wim) in advance**, for example with DISM `/Add-Driver /Image:` or PhoenixPE's driver integration. The Windows that builds the image detects the signature mismatch and registers the package as unsigned, so it is not installed on the device at boot.
> - To put drivers into the PE image in advance, use the full package. Otherwise keep the slimmed copies on the boot media and run `pedrv /pe` when PE starts.
> - Never install them into a regular Windows.

## PhoenixPE script

[`phoenixpe/PedrvDriverLoader.script`](phoenixpe/PedrvDriverLoader.script) adds pedrv to a PE built with PhoenixPE so drivers can be loaded in the booted PE.

- **Driver location**: one of two.
  - A folder on the boot media or a disk (default `\PEDrivers`, searched on every drive)
  - Files included in the PE image (copied to `X:\Program Files\pedrv`)
- **Run at boot** (optional): choose before or after the desktop, and the window mode: minimized (default, taskbar button only), hidden (output in `X:\Windows\Temp\pedrv.log`), normal, or normal and wait for a key.
- **Excluded device classes**: 12 check boxes (Display, Media, Net, Bluetooth, USB, SCSIAdapter, ...) and an "other" field build `/exclude:`.
- **Shortcuts**: desktop and Start menu. The console window stays open so you can see the result.
- **`/fromwin` shortcut**: a separate shortcut that loads the drivers of the Windows on disk into PE. `/inbox` (include inbox drivers) is available too.
- **Without-display shortcuts**: for each of the two shortcuts above, an extra shortcut that loads everything except graphics drivers (`/exclude:Display`).
- **Extra options**: append pedrv options such as `/slim` or `/scratch:`.
- **Files needed**: `pedrv.exe` (x64) is encoded in the script, so nothing else is needed. To use a newer build or a build for an x86 or arm64 PE, choose "external pedrv.exe" and pick the file. 7-Zip is taken from PhoenixPE.

Copy it anywhere you like, such as `Projects\PhoenixPE\Drivers`. Because drivers are loaded while PE runs, slimmed copies (`/export`) work too. For graphics drivers, also select `DisplayDriverSupport.script`.

## When graphics drivers do not work in WinPE

Depending on how the PE is built, vendor graphics drivers may be blocked. When pedrv detects either case below, it prints a warning and skips display drivers.

| Symptom | Cause | Fix |
|---|---|---|
| Device code 31 | `GraphicsDrivers\ForceBddFallbackOnly=1` in the PE registry | Set it to 0 when building PE (changing it at run time has no effect) |
| Device code 43, DWM keeps restarting | `System32\DDisplay.dll` missing from a PE based on Windows 11 24H2 or later | Add `DDisplay.dll` to the PE build. With `/fromwin`, it is copied automatically from a Windows of the same build |

With PhoenixPE, [`phoenixpe/DisplayDriverSupport.script`](phoenixpe/DisplayDriverSupport.script) handles both. The investigation is in [docs/pe-24h2-display-driver.md](docs/pe-24h2-display-driver.md) (Korean).

## Requirements and limitations

- WinPE based on Windows 10 / 11 (x64, x86, ARM64)
- `.zip` and `.7z` sources need 7-Zip. Put `7z.exe` and `7z.dll` (or `7za.exe`) in the same folder as `pedrv.exe`. Without 7-Zip, zip files are handled only when `tar.exe` is available.
- Devices PE cannot see, such as devices behind a controller that has no driver, cannot be matched. Load the controller driver first with `pedrv /pe`, then run again.
- pedrv does not install into the running Windows itself (`/pe` installs only in WinPE; on a regular Windows only `/list` works).
- BitLocker-locked drives cannot be read.

## Build

Run on a PC with Visual Studio (C++ workload).

```bat
build.cmd          :: x64 → bin\x64\pedrv.exe
build.cmd x86
build.cmd arm64
```

For x64 builds, `7z.exe` and `7z.dll` are copied too if `C:\Program Files\7-Zip` exists.

The MSVC toolset is chosen automatically from those that can build the architecture (if the default toolset has no ARM64 tools, an older toolset that has them is used). You can also pick a version, as in `build.cmd arm64 14.44`.

## More details

Detailed behavior of each option, INF selection rules, work folder and RAM disk usage, and the reasoning behind design decisions are in [CLAUDE.md](CLAUDE.md) (Korean). Investigation notes are in [docs/](docs/) (Korean).

## License

[MIT License](LICENSE)

7-Zip is not included in this repository. If you distribute 7-Zip with pedrv, follow the 7-Zip license (GNU LGPL and others).
