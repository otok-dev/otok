# Lua 5.5.1

`src/` holds the sources of the Lua library from lua.org's `lua-5.5.1.tar.gz` (SHA-256
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`), unchanged, without the standalone
interpreter and compiler (`lua.c`, `luac.c`) and the Makefile. License: `LICENSE` (MIT).

otok compiles them as C++ (`otok-lua` in `CMakeLists.txt` and `projects/vs/otok-lua`), so Lua raises errors as
C++ exceptions and destructors run (`docs/PLAN.md` decisions 18 and 50). Homebrew's and the systems' Lua are C
builds, which is why the sources are here. To update: replace `src/` with the new release's, keep the list of
removed files, and change the version and hash here and in `docs/PLAN.md`.
