# Play an IGS game natively on Windows (no WSL): .\play-win.ps1 harpoonlagoon_m107us
# (.\play-win.ps1 --list shows the ids.)  Build first in WSL: win/build-win.sh.
# Options are IGS_* environment variables (wiki Loader).
& "$PSScriptRoot\build\win\igs-loader.exe" @args
