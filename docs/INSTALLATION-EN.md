# Daytona Arcade Native beta 0.11

Daytona Arcade Native runs *Daytona USA* (Sega Model 2, Revision A, 1994)
as native code on Windows. It builds on alphanu1's
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp).

You need 64-bit Windows 10 or 11, an internet connection for first-time setup
and about 10 GB of free space. A Direct3D 12 graphics card is recommended for
the visual enhancements. You also need your own Revision A ROM set. The
project does not include ROMs or a prebuilt game.

## Install and build

1. If Git is missing, run `winget install Git.Git` in PowerShell and open
   a new PowerShell window.
2. Download this beta.

       git clone https://github.com/yodo113/Daytona-Reborn-Beta.git
       cd Daytona-Reborn-Beta

3. Create the `roms` folder and copy your Revision A ROM set into it as
   `daytona.zip` or `daytona.7z`. The archive must contain
   `epr-16722a.12`, `epr-16723a.13`, `epr-16724a.6` and
   `epr-16725a.7`.

       New-Item -ItemType Directory -Path roms -Force
       Copy-Item C:\path\to\daytona.zip roms\daytona.zip

4. Run the setup script from the project folder.

       powershell -ExecutionPolicy Bypass -File setup.ps1

   First-time setup can take half an hour or longer. Windows can request
   permission to install missing build tools. When setup prints `== Done`,
   run the command shown there. With the Visual Studio generator, it is
   typically `.\build-daytona\Release\daytona.exe`.

## Choose your settings

- Open **Settings**, then use **Langue / Language** at the top.
- Open **Settings**, then **Display** to select **Picture format**.
  **Arcade 4:3** uses the original proportions. **Automatic** follows the
  window. You can also choose 16:10, 16:9 or 21:9.
- For experimental 120 frames/s smoothing, select the Hardware renderer
  and the Double buffer draw mode, enable smoothing, then restart the game.

- New profiles use the **Cabinet** colour preset (gamma 2.4).
- To try **Sun shadows (experimental ray tracing)**, enable them in
  **Settings**, **Display** and restart. Direct3D 12 with DXR 1.1 is
  required. Sky light is optional.
- Music selection is under **Audio**. See the
  [release notes](NOTES-BETA-EN.md#local-music-files) for the filenames
  used by Saturn / Windows and Dreamcast.

[Release notes](NOTES-BETA-EN.md) · [Guide français](INSTALLATION-FR.md)
