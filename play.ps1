# Play an IGS game through the loader (WSLg window, sound and keyboard).
# Argument: a game id from hash\*.xml, e.g. .\play.ps1 oceanparty_md105cn
# (.\play.ps1 --list shows the ids and which rom sets are found).
$run = wsl -- wslpath -a ($PSScriptRoot -replace '\\', '/')
wsl -- bash "$run/run.sh" @args
