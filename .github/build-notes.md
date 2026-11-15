**Fixed**
- DLC from an OunceCake dump now loads. Keep the dump's folders as they are (`base`, `updates/<version>`, `dlc/<DLC ID>/<version>/romfs`) and boot the game's `exefs/main`. The game finds the `dlc` folder next to it. Nothing needs to be packed into an NSP.
- Switch 2 Edition games now find their DLC. Its ID starts with `0100` (the Switch 1 game's), while the Switch 2 program's starts with `0400`, so installed DLC was never offered to the game. Pikmin 4's Dandori Academy is `0100B7C00933B001`.
- ExeFS mods in `user/load/<title ID>/` (subsdk modules, `main.npdm`) now load when you boot a game from its folder. Before, they had to be copied into the game's files.
