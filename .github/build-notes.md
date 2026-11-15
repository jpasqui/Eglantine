**Fixed**
- DLC from an OunceCake dump loads with the dump's folders exactly as OunceCake writes them. Boot `updates/<version>/program/0/exefs/main` (the Switch 2 Edition), and Eglantine finds the DLC in `dlc/<DLC ID>/<version>/base/program/0/romfs`. Nothing needs to be moved or renamed.
- Dumps with the flat layout (`base`, `dlc/<DLC ID>/<version>/romfs`) still load as before.
