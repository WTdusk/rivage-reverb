# Building Rivage Reverb

The project builds with CMake and JUCE 9.0.2. JUCE is **not** vendored in this
repository; you either fetch it or point the build at a copy you already have.

## Requirements

| | Windows | macOS / Linux |
| --- | --- | --- |
| CMake | 3.22 or newer | 3.22 or newer |
| Compiler | Visual Studio 2022 or newer (Desktop C++ workload) | Xcode 14+ / GCC 11+ / Clang 14+ |
| JUCE | 9.0.2 | 9.0.2 |
| Generator | Visual Studio or Ninja | Ninja or Make |

Only the VST3 format is enabled. There is no installer and no code signing.

## Getting JUCE

Pick one of the two options below.

### Option A — let CMake fetch it (simplest)

Configure with `-DRIVAGE_FETCH_JUCE=ON` and CMake clones tag `9.0.2` into the
build tree on the first configure. Nothing else to do.

### Option B — clone it next to `CMakeLists.txt`

```sh
git clone --depth 1 --branch 9.0.2 https://github.com/juce-framework/JUCE.git JUCE-9.0.2
```

The folder must be named `JUCE-9.0.2` and sit next to the top-level
`CMakeLists.txt`. This is what `scripts/build.cmd` assumes.

## Windows

`scripts/build.cmd` finds Visual Studio with `vswhere`, runs `vcvars64.bat`, then
configures and builds with CMake + Ninja. From a normal `cmd.exe` or PowerShell:

```bat
scripts\build.cmd all build Release
```

Arguments are:

```
build.cmd <target> <build-dir> <config> [reconfigure|relink]
```

- `<target>` — `RivageReverb_VST3`, `Analyzer`, `RivagePreview`, or `all`
- `<build-dir>` — usually `build`
- `<config>` — `Release` or `Debug` (single-config generators only read this on
  the *first* configure; use a separate build directory for the other config)
- `reconfigure` — delete the build directory first
- `relink` — reconfigure and rebuild without deleting

The plug-in ends up at:

```
build\Source\RivageReverb_artefacts\Release\VST3\Rivage Reverb.vst3
```

To do it by hand instead:

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target RivageReverb_VST3
```

`vcvars64.bat` matters on Windows: CMake invokes `cl.exe` through pipes, and without
the environment set up the build hangs instead of failing.

## macOS and Linux

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DRIVAGE_FETCH_JUCE=ON
cmake --build build --target RivageReverb_VST3
```

The bundle is written to
`build/Source/RivageReverb_artefacts/Release/VST3/Rivage Reverb.vst3`, which is a
bundle directory on macOS and a shared object elsewhere.

## Installing

Windows, per user, no administrator rights:

```bat
scripts\install-user.cmd
```

This copies the bundle to `%LOCALAPPDATA%\Programs\Common\VST3\WTdusk\` and leaves
every other plug-in alone. Delete that `WTdusk` folder to uninstall.

## Tests and tools

```sh
bash scripts/smoke.sh                      # 26 renders: 6 algorithms, 2 sample rates, 5 buffer sizes
build/tools/analyzer/Analyzer --help       # scan / render / bench / switchtest
build/tools/preview/"Rivage Preview"       # editor in a window, --screenshot for docs
```

`smoke.sh` needs the `Analyzer` target built; it renders 26 impulses and checks every exit code.

## Troubleshooting

**The build hangs on Windows.** CMake is trying to reach `cl.exe` without the MSVC
environment. Use `scripts/build.cmd`, or run `vcvars64.bat` in the same shell
before invoking CMake.

**`Could not find JUCE-9.0.2`.** Either clone JUCE into `JUCE-9.0.2/` (Option B)
or configure with `-DRIVAGE_FETCH_JUCE=ON` (Option A). The configure step stops with
a message naming both options if neither is present.

**Fetching JUCE fails behind a proxy.** `FetchContent` uses git; make sure
`https_proxy` is set for the shell that runs CMake, or fall back to Option B and
clone it by hand with whatever proxy settings work.

**Switching between Release and Debug.** Ninja is a single-config generator: the
`Debug` argument is ignored once `build/` has been configured for `Release`. Use a
second directory, for example `scripts\build.cmd RivageReverb_VST3 build-debug Debug`.

**The plug-in does not show up in the DAW.** VST3 hosts scan
`%LOCALAPPDATA%\Programs\Common\VST3` and `C:\Program Files\Common Files\VST3`.
Either run `scripts\install-user.cmd` or point the host at the build directory.
