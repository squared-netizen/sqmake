# Lua

[Lua](https://www.lua.org) 5.4.7, from the Lua team's repository
(github.com/lua/lua, tag `v5.4.7`, commit
`1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3`), MIT (`LICENSE`, from the end
of `lua.h`). The library's sources, copied unchanged; not the interpreter
(`lua.c`), the compiler (`luac.c`) or the test library (`ltests.*`). Built
with `LUA_USE_POSIX` (no `dlopen`: build files load no C modules).

sqmake describes builds in Lua 5.4 (docs/spec.md §1.1, §7.2).
