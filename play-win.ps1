# Play an IGS game natively on Windows (no WSL): .\play-win.ps1 harpoonlagoon
# Build first in WSL: win/build-win.sh.  Options are IGS_* environment variables (docs/IGS_LOADER.md).
& "$PSScriptRoot\build\win\igs-loader.exe" @args
