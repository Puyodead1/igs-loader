# Play an IGS game through the loader (WSLg window, sound and keyboard).
# Argument: a manifest name from tools\igs-loader\manifests, e.g. .\play.ps1 oceanparty
$run = wsl -- wslpath -a ($PSScriptRoot -replace '\\', '/')
wsl -- bash "$run/run.sh" @args
