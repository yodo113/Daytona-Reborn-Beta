# Daytona Arcade Native: beta 0.11

Public release for Windows. Setup guide:
[INSTALLATION-EN.md](INSTALLATION-EN.md) (French:
[INSTALLATION-FR.md](INSTALLATION-FR.md)).

## Where it comes from

Daytona Arcade Native builds on alphanu1's static recompilation
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp).
The cabinet's program is translated to native C++, with no emulator. This
edition keeps the original simulation, so the cabinet's driving and race,
and works on the picture and on comfort on PC.

## What’s new in 0.11

- **Seven launcher languages**. English, French, Portuguese, Spanish,
  Chinese, Japanese and Arabic.
- **Arcade cabinet colours**. New profiles use the Cabinet preset
  (gamma 2.4). Saved profiles keep their settings.
- **Experimental ray tracing**, off by default. Direct3D 12 with DXR 1.1
  adds sun shadows on scenery and the player’s car, optional sky lighting
  and light shafts anchored to the scenery. The game uses standard rendering
  if ray tracing is unavailable. Changes take effect on the next launch.
- **Race music**. Choose Arcade, Saturn / Windows or Dreamcast in the
  audio settings. Arcade uses the sound from your ROM. The other modes use
  local WAV files from your own copy of the game. Missing tracks fall back
  to the original music.
- **Optional scenery**. Use `M2_CANYON_FILL=1` and
  `M2_CANYON_ROCK_ABUTMENT=1` for the Dinosaur Canyon supports. On Expert
  with draw distance +2, `M2_EXPERT_TOWER=1` reveals the game’s original
  tower earlier on its approach.
- **Smoother presentation**. Interpolation remains optional. Ray tracing
  processes large supersampled frames at display resolution to reduce load.

### Local music files

Create `musiques-locales` beside `daytona.exe`. The game expects stereo
44.1 kHz, 16-bit PCM WAV files.

| Course | In `musiques-locales/saturn/` or `musiques-locales/windows/` | In `musiques-locales/dreamcast/` |
| --- | --- | --- |
| Beginner | `07-lets-go-away.wav` (or `07.wav`) | `adxb_055.wav` |
| Advanced | `06-the-king-of-speed.wav` | `adxb_063.wav` |
| Expert | `08-sky-high.wav` | `adxb_060.wav` |

## New in 0.10

These answer requests from the video comments and GitHub.

- **Interface language**: open Settings in the launcher and use "Langue / Language".
  French and English first. The choice is kept, and the first run follows
  Windows' language. Portuguese, Spanish, Chinese, Japanese and Arabic are
  being prepared.
- **Picture format**: now first in the display settings. **Arcade 4:3**
  (the cabinet's picture, centred with bars), Automatic (the window's
  shape), 16:10, 16:9 or 21:9. When the window is wider than the selected
  format, the game centres the picture with side bars. A narrower window
  limits the width of the widescreen view.
- **120 frames/s smoothing repaired**: in a race, 20 of the 39 opponent
  cars shook or doubled. All of them are tracked now, and the motion is
  steadier. The option is still off until checked.

## Already in 0.9

- A PC menu: normal race or Time Attack, automatic or manual gearbox,
  settings.
- Widescreen: scenery and cars are visible at the sides.
- An adjustable draw distance.
- 3D scenery, optional: fir trees, grandstands, Dinosaur Canyon's bridge
  pillars.
- A tube-cabinet screen curve and a saturation setting.
- Opponent cars always detailed, optional: one model at every distance.

## Known limits

- **120 frames/s smoothing** needs the Hardware renderer and the "Double
  buffer" draw mode. The frame rate depends on the display and the
  graphics card.
- **Opponent cars** are the cabinet's original models. A more detailed
  reconstruction is planned.
- Only the *Daytona USA* Revision A ROM set (`daytona`) gives the PC menu and
  the enhancements.

## No game data

The repository contains no ROM and no code derived from the ROM. The game is
rebuilt on your computer from your own copy.
