# appstoreopener

A utility that launches Microsoft Store apps by name matching, working around the limitation of not being able to easily pin or shortcut their exact executable paths (unless I have completely missed something). You could use this to set up keyboard shortcuts to open MS Store apps, for instance.

`C:\Program Files\WindowsApps\` is restricted (even) from admin enumeration, but the individual package folders within are accessible if you know their name.

This utility also avoids versioning issues and updated version paths changing by doing name matching for the package (app) and executable name.

If a process is running with the same specified .exe then it switches to that instead of spawning a duplicate process.

## How it works

Duplicate and rename the exe to `appstoreopener-<package>-<executable>.exe`. On launch, it:

1. Reads its own filename and extracts the package and executable terms
2. Searches `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages` for installed packages and retrieves their installation paths
3. Searches the found path for an .exe whose name contains the executable term
4. Launches the executable, if found

To setup, use `Get-AppxPackage` in the console to find your installed package names, and then the path to go look in for the executable.

### Naming convention

```
appstoreopener-<package>-<executable>.exe
```

The package segment is matched against installed package names. The executable segment is matched against .exe filenames inside that package's directory.

| Filename | Package search | Exe search |
|---|---|---|
| `appstoreopener-raindropio-raindrop.exe` | `*raindropio*` | `raindrop*.exe` |
| `appstoreopener-spotify-spotify.exe` | `*spotify*` | `spotify*.exe` |

To find the right terms, run `(Get-AppxPackage -Name "*<package>*").InstallLocation` in a PowerShell console to confirm the package name, then rename `appstoreopener.exe` and afterwards use `--dry-run` to verify the exe is found.

The package name is normalised to lowercase [a-z0-9] before matching against your search term.

## Installation

Build the project, then copy and rename `appstoreopener.exe` for each app you want to launch. Keep all your renamed copies in a single dedicated folder and add that folder to your `PATH` - every copy is then accessible from Run (`Win+R`), the Start menu search, console, etc.

**Recommended install location:** `%userprofile%\AppData\Local\appstoreopener\`

1. Create the folder and drop your renamed exes there:
   ```
   %userprofile%\AppData\Local\appstoreopener\
     appstoreopener-raindropio-raindrop.exe
     appstoreopener-spotify-spotify.exe
   ```

2. Add the folder to your user `PATH`:
   - Open **System Properties > Environment Variables**
   - Under *User variables*, edit `Path` and add the folder path from above

3. Pin individual exes to the taskbar or Start by right-clicking them in Explorer as needed.

## Dry run / testing

To see all matching executables found in the package directory, run with `--dry-run`:

```
appstoreopener-raindropio-raindrop.exe --dry-run
```

A message will show what package path it matched and list every found exe in order. No app is launched in this mode.

## Building

Requires [MinGW](https://www.mingw-w64.org/) (which provides `gcc` on Windows).

```
make
```

Or manually:

```
gcc -Wall -O2 -mwindows -o appstoreopener.exe appstoreopener.c -lshell32
```

## MinGW

MinGW can be installed with [MSYS2](https://www.msys2.org/). After installing it, open the `UCRT64` shell and:

```
pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-make
```

Then add the following two folders to your PATH env var (assuming you installed MSYS2 to the default install location):

```
C:\msys64\ucrt64\bin
C:\msys64
```

`make` is installed as `mingw32-make` so I added the following to my Powershell profile:

`vim $PROFILE`:
```
function make { mingw32-make @args }
```

## License

MIT © Michael Champanis 2026
