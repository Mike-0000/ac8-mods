# ac8-mods

Mods for ACE COMBAT 8: WINGS OF THEVE (Steam, Windows) and the script that packages them for Nexus Mods.
Offline single player only.

| Mod | What it does |
| --- | --- |
| `FovMod` (Per-View FOV) | Separate field of view for the cockpit, HUD-only and third-person cameras |
| `AcePilotsMod` (Ace Pilots) | Tougher enemy pilots, with an in-game menu and presets |

## Build

```
python build.py build            # every mod -> dist\<name>-<version>-Full.zip and -ModOnly.zip
python build.py build FovMod
python build.py check some.zip   # run the upload rules on any zip
python build.py import FovMod    # refresh mods\FovMod\files from the game's ue4ss\Mods folder
```

Needs Python 3.9 or newer, nothing else.

- **Full** holds `Game\` and `EasyAntiCheat\` to copy into the game folder: the UE4SS loader, UE4SS,
  the mod and the offline settings file. The player then sets one Steam launch option.
- **ModOnly** holds the mod folder alone, for a game that already runs UE4SS mods.

Each mod folder gets an empty `enabled.txt`, which makes UE4SS load it without an entry in `mods.txt`
or `mods.json`, so a release never touches a list shared with other mods.

## Layout

```
mods\<Mod>\mod.json     folder, title, version, zip name, files the player owns
mods\<Mod>\about.txt    the mod's part of the readme
mods\<Mod>\files\       what goes into the game's Mods\<Mod> folder
mods\<Mod>\src\         source of native parts; not shipped
runtime\                loader, UE4SS and offline settings, shared by every Full zip
templates\              the two readmes
```

## Upload rules

Nexus Mods quarantines an upload that contains executables or similar file types, that its scanners
flag, or that holds another archive. `build.py` refuses to write a zip with a script or executable
(`.exe .bat .cmd .ps1 ...`), an archive, a Windows binary under any name but `.dll`, or a hidden
extension. DLLs under their real name are fine.

## Credits

[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) by the UE4SS-RE team (MIT), in `runtime\` unmodified.
The offline launch method was first published with "FOV Change" by 4598llj.

MIT licensed. Not affiliated with Bandai Namco.
