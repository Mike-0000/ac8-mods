{title} {version}  -  ACE COMBAT 8: WINGS OF THEVE (Steam)
Offline single player only.

This is the complete package: the mod plus the UE4SS mod loader it runs on.
Already running another UE4SS mod? Use the "Mod only" download instead, so your
existing loader and mod list stay as they are.

INSTALL
  1. Close the game. In Steam: right-click ACE COMBAT 8 > Manage > Browse local files.
  2. Copy the "Game" and "EasyAntiCheat" folders from this zip into that folder.
     Choose "merge" if Windows asks. No game file is replaced.
  3. In Steam: right-click ACE COMBAT 8 > Properties > General > Launch Options.
     Paste this whole line:

     cmd /d /c "set EOS_USE_ANTICHEATCLIENTNULL=1&& %command% -anticheat_settings=AC8_Offline.json"

  4. Start the game from Steam as usual.

  If you already use a launch option like this from another mod, keep yours. Any of
  them works.

{about}

WHAT GETS INSTALLED
  Game\Binaries\Win64\dwmapi.dll               UE4SS loader
  Game\Binaries\Win64\ue4ss\                   UE4SS, unmodified
  Game\Binaries\Win64\ue4ss\Mods\{folder}\
  EasyAntiCheat\AC8_Offline.json               copy of the game's Settings.json with
                                               "allow_null_client" added

GOING BACK ONLINE
  The launch option starts the game without Easy Anti-Cheat, so online modes do not
  work while it is set. To play online: clear the launch option and delete
  Game\Binaries\Win64\dwmapi.dll. Put both back to play with mods again.

UNINSTALL
  Clear the launch option, then delete dwmapi.dll, the ue4ss folder (or only
  ue4ss\Mods\{folder} if other mods use it) and EasyAntiCheat\AC8_Offline.json.

UPDATING
  Copy the new folders over the old ones. Your settings are kept:
  {user_files}

CREDITS
  UE4SS by the UE4SS-RE team, MIT licence, included unmodified.
  https://github.com/UE4SS-RE/RE-UE4SS
  Offline launch method as first published with "FOV Change" by 4598llj.
