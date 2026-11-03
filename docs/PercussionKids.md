# Percussion Kids (E1000)

Version M105CN (2005).  A kids' rhythm game on three drums: a
large one in the middle, a blue one on the left and a red one on the right.
Notes fall down three lanes to the pads; 22 nursery songs and classics,
Junior or Senior charts, one or two songs per credit.

**Status:**
- Extracted from the stock image (plain ext2).
- **Plays in the loader** (machine `e1000`, plugin `percussionkids`), both
  loaders: IGS logo, coin page with sound, coins, select mode, select song,
  loading, songs with the drums judged, combo, gauge and score, result, the
  second song, game over, back to the coin page; the operator menu on F2
  (2026-11-01).
- The IGS027A runs the whole game and its firmware is not dumped: the
  plugin is a rebuilt firmware (`IGS_E1000.md`).  Flow, timing, judgement
  windows and scores are invented; the charts are the executable's own.

## Playing

```powershell
.\play.ps1 percussionkids_m105cn        # WSL
.\play-win.ps1 percussionkids_m105cn    # native Windows
```

The coin page comes after about 5 s.  A coin and the large drum start a
game.

| Key | Cabinet |
| --- | --- |
| Space, Down, C, Enter, 1 | large drum (start, decide) |
| Left, Z, F | blue drum (left) |
| Right, X, J | red drum (right) |
| Left + Right together | both small drums |
| 5 | coin |
| 9 | service (a credit) |
| F2 | test (operator menu, from the logo or the coin page) |
| F1 | show / hide the key help |

- **Drums:** switches; no hit strength.  Charts use four lanes: left, right,
  large (with rolls: hit as often as you can until the roll ends) and both
  small drums at once (within 0.12 s).
- **Menus:** select mode and song with the small drums, decide with the
  large one; the operator menu as on the cabinet: blue up, red down, large
  drum enter.  Leave the key test by holding both small drums.
- `PK_REGION=n`: the region the chip reports: 1 China (default; Simplified
  Chinese), 2 Hong Kong, 0 Taiwan (Traditional, the "Tempo Kiddo" title),
  3..6 English.
- `PK_AUTOPLAY=1`: the chip plays every note itself (testing).
- `PK_NO_FASTPATH=1`: draw bitmaps through the emulated 2D engine line by
  line (about 18 fps in songs on Windows) instead of the plugin's fast path.
- **Settings:** volumes, credits per play, songs per play and free play are
  the chip's, in `/pkids/log/igs027a.nv` of the rom set (for a `.7z` set, the
  cache's `<cache>/<linux|windows>/percussionkids_m105cn/pkids/log/`); the
  game keeps song volumes and top scores in `/pkids/gamedata`.

## Extracting

`igs-mkromset percussionkids_m105cn <image>` builds the rom set (see
the size table in `IGS_LOADER.md`).

| Path | What |
| --- | --- |
| p1 ext2 | root: GRUB, kernel `bzImage-2.4.24-GX_M-CM_K-PCI_K`, BusyBox, uClibc 0.9.26, SDL 1.2 and SDL_mixer |
| p2 ext2 | `/pkids/gamedata`: `system/gx2show`, `load.bmp` |
| p3 ext2 | `/pkids/linux`: `pkids`, `main/*.rom` (sprites), `band1/` (songs: `BG.wav` and four instrument wavs each, MTV sprites), `sound/`, `system/` (fonts) |
| p5 ext2 | `/pkids/log`: `assert.txt` |

## How the cabinet starts it

BusyBox `init` runs `/etc/init.d/rc.sysinit`: it mounts p3 read-only on
`/pkids/linux`, p2 on `/pkids/gamedata` and p5 on `/pkids/log`, shows
`load.bmp` with `gx2show`, then runs `cd /pkids/linux; ./pkids`.

## The executable

`pkids` (i386, uClibc, 450 KB, symbols kept) links SDL and SDL_mixer for
sound only; it draws with the Geode GX2's 2D engine through `/dev/mem`
(`IGS_E1000.md`).  Everything it shows comes from the IGS027A's command
lists.

- **Songs** (`g_raBand1List`, 31 entries for 22 songs): 0 world, 1
  littlestar, 2 ebyy, 3 littleducks, 4 toys, 5 london, 6 indians, 7 rowboat,
  8 abcsong, 9 airplane, 10 newyear, 11 rfclassic01, 12 rfclassic02, 13
  pmclassic01, 14 pmclassic02, 15 matsuri, 16 chiquita, 17 mickey, 18
  kantin, 19 jinglebells, 20 xmas, 21 birthday; ranks Rookie and Easy
  (Junior plays Rookie, Senior Easy, either when only one exists).
- **Charts:** 36 tracks per song: 1..8 the lanes (left 0, centre 1 with
  rolls, both 2, right 5), 17..24 background events (BG.wav at tick 32),
  25..36 MTV animation.  A tick is 1/8 beat at the song's tempo; each event
  names the wav the hit plays.  The PC never reads tracks 1..16 (its
  `record/*.rec` path is a recording tool and the files are not on the disk):
  the chip had its own copy; the plugin reads the executable's.

## Loader status

Done:
- [x] Rom set: `mount p1 /; mount p3 /pkids/linux; mount p2 /pkids/gamedata;
      mount p5 /pkids/log`; logical partitions in the set builder.
- [x] uClibc on glibc: `build/uclibc` (`libc.so.0`, `libm.so.0`,
      `libdl.so.0`, `__uClibc_start_main`), run with the host's
      `ld-linux.so.2` (list feature `libc`); Windows: the same names.
- [x] Core: port I/O traps and `iopl`, `mmap` at an offset, `lseek` on
      machine files, a window for a machine's frame buffer (`present`).
- [x] `machines/e1000`: Geode GX2 (PCI, VSA, MSRs, display controller, the
      2D engine's copies, fills, colour key, alpha and text), the IGS027A
      transport.
- [x] Fast path: `gfx2_color_bitmap_to_screen_blt` (`0x80766A0`) jumps to the
      plugin, which does the whole blit at once (1,300 trapped blits a frame in
      a song otherwise; 60 fps on both loaders with it).
- [x] `games/percussionkids`: start-up, logo, coin page, credits, select
      mode, select song, loading, the song (chart, judgement, sounds,
      combo, gauge, score), result, stages, game over, operator menu (sound
      and game settings, key test, language).

Invented:
- [x] The game flow and its timing: logo 4 s, coin page 20 s, select mode
      10 s, select song 20 s, result 7 s, game over 5 s.
- [x] Judgement: Great ≤ 50 ms, Cool ≤ 100 ms, Nice ≤ 150 ms, Poor ≤ 200
      ms (counts as a miss); 100 / 80 / 50 points, 10 per roll hit; the
      gauge and the result level are the score's share of 100 per note.
- [x] Settings defaults: volumes 28 (game) / 20 (demo) all day, 1 credit,
      2 songs per play.

Open:
- [ ] Not run: the attract demo (opening and staff MTVs, how to play), the
      text warning (command 7 crashes in `sprintf` with region 1), tickets,
      bookkeeping, time setting, reset, the hardware tests other than keys,
      the developer test menu, ranking names.
- [ ] A fault in the executable runs its own handler, which logs to
      `/pkids/log/assert.txt` and hangs: the window freezes; close it.
- [ ] Hit sounds: the hit plays the note's instrument wav; the cabinet's
      exact mix is unknown.
