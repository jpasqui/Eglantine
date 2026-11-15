**Fixed**
- DLC from an OunceCake dump now loads when the game has ExeFS mods installed, such as Tsuchi. Before, installing a mod hid the dump's `dlc` folder, so the game started without its DLC.
- OunceCake's layout works as it is. Boot `program/<version>/exefs/main`, with the `dlc` folder next to `program`. The `base` and `updates/<version>` layout still works.
- If Eglantine finds no `dlc` folder, the log now says which folder it searched.
