# Lua 5.4.9, vendored

The `src/` directory of the official Lua 5.4.9 release, unchanged except
that `lua.c` and `luac.c` (the stand-alone interpreter and compiler) are
left out. Mumoria's server embeds it for its scripts (`src/mumoria/script.cpp`,
`scripts/README.md`); the single-player game does not use it.

* Source: https://www.lua.org/ftp/lua-5.4.9.tar.gz
* SHA-256: `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`
  (as published on https://www.lua.org/ftp/)
* License: MIT, Copyright (C) 1994-2026 Lua.org, PUC-Rio; the full notice is
  at the end of `lua.h`.

To update it, replace these files with a newer 5.4 release's `src/` (again
without `lua.c` and `luac.c`) and update the version and checksum above.
