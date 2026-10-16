<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the tico ecosystem* — https://www.ticoverse.com

**mGBA** is a fast, accurate, and widely used emulator for the Game Boy Advance.

This fork adapts mGBA to work with the tico frontend and provides a standalone build for the Nintendo Switch, adding a small set of practical features while preserving the strengths that make mGBA reliable.

----------

## Summary

This fork focuses on making mGBA more usable in practice without changing what makes it solid.

It adds:

-   Custom overlay matching tico design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters  
-   Built-in save and load state support
-   Integrated RetroAchievements with custom alerts

----------

## Credits

This fork is built on top of the libretro port of mGBA, by endrift.
All core emulation work belongs to the mGBA team and its contributors.

- **Official mGBA repository** — https://github.com/mgba-emu/mgba
- **mGBA website** — https://mgba.io
- **libretro mGBA** — https://github.com/libretro/mgba

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.