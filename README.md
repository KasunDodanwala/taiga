# Taiga for Linux (unofficial fork)

An unofficial **Linux port of [Taiga](https://github.com/erengy/taiga)** (the Qt 6 rewrite on the
`develop` branch), with a few extras: Linux media detection that also catches Kodi's DLNA streaming,
a built-in DLNA media server, Discord Rich Presence, and Taiga 1.x's torrent features (feeds, the
full torrent filter system and qBittorrent downloads).

> **This fork was built entirely by Claude.** Every change here (the code, the build fixes and this
> README) was written by [Claude](https://www.anthropic.com/claude), Anthropic's AI model
> (Claude Opus 5.5 in Claude Code), at the request of [@KasunDodanwala](https://github.com/KasunDodanwala).
> Taiga itself is the work of [Eren Okka](https://github.com/erengy) and its contributors.

It's a personal-use fork, tested on Fedora 44 (KDE Plasma, Wayland) with AniList. The Windows code
paths were left as they were and weren't tested.

## What's new

### Media detection on Linux

- **Anisthesia on Linux.** Taiga's media detection library gets a Linux backend
  ([KasunDodanwala/anisthesia](https://github.com/KasunDodanwala/anisthesia), branch `linux`). It
  finds media players by their executables in `/proc` and reads their open files with the same
  checks as on Windows: read-only, regular files, outside system folders.
- **Taiga 1.x's rules** decide which file is being watched: valid video extensions only, audio
  files and openings/endings/previews skipped, and optionally files outside your library folders.
- **Linux media players** were added to the player list: Celluloid, Clapper, Dragon Player, GNOME
  Videos, Haruna, Kaffeine, MPlayer, Parole, QMPlay2 and Showtime. Cross-platform players from
  Anisthesia's list (Kodi, mpv, VLC, SMPlayer, ...) work as well.
- **Kodi** is detected like any other player, including the files it streams to other devices
  through its UPnP/DLNA server, so watching on a phone, TV or another computer updates your list
  just like Taiga does on Windows. Optionally, Taiga can also ask Kodi what it's playing over
  JSON-RPC (knows when playback is paused, works for network shares), and play episodes in Kodi.
- Limitation: Wayland doesn't let apps read other windows' titles, so window-title and web browser
  (streaming site) detection aren't available.

### Built-in DLNA media server

- Shares your library folders over UPnP/DLNA, so TVs, phones, VLC, Infuse, mpv and other players
  can browse and play your anime. Besides the folders it has **Currently watching** (episodes of
  the anime you're watching, the next one marked) and **Recently added**.
- Streams files as they are by default, with seeking. Optional transcoding in software (x264) or on
  the GPU (VA-API), with seeking, a maximum bitrate and burned-in subtitles, for devices that can't
  play a file directly.
- Knows what each device is streaming (and when it's paused), so the episode is recognized and
  your list is updated.
- Can be limited to the local network and Tailscale.

### Discord Rich Presence

- Shows what you're watching on your Discord profile as a "Watching" activity: anime title,
  episode, fansub group, elapsed time and a button to the anime's page, each optional.

### Torrents (Taiga 1.x features)

- **Discovery**: checks a torrent feed (e.g. nyaa.si) every few minutes, searches torrents for an
  anime (`%title%` in the search URL), and notifies you or downloads new episodes automatically.
- **Torrent filters**, ported from Taiga 1.x: the same elements and operators, Discard / Select /
  Prefer actions (with Taiga's strong and weak preferences), the same presets, and the same editor
  (filter list, Add New Filter wizard, condition dialog, limiting filters to certain anime).
  Import and export use Taiga 1.x's text format, so filters exported from Taiga 1.x import as-is.
- **Downloads through qBittorrent** (Web UI API): category, a subfolder per anime, magnet links,
  and a connection test.
- **Torrents page**: see what the feed found and why it was marked or discarded, download marked
  torrents, copy (magnet) links.

### Desktop integration

- `--minimized` starts in the system tray; `--page NAME` opens a page (`list`, `history`,
  `library`, `torrents`, `search`, `settings` or `settings/<section>`, e.g. `settings/filters`).
- Single instance: starting Taiga again brings the running window up (or opens the page).
- Optionally closes to the tray, and syncs your list on startup.
- New settings pages: Media server, Recognition / Kodi, Sharing / Discord, and Torrents with
  Discovery, Downloads and Filters tabs.

## Building on Linux

You need CMake 3.21+, a C++23 compiler (tested with GCC 16), Qt 6 (Base, SVG, Tools) and SQLite.
`ffmpeg`/`ffprobe` are used by the media server (durations, transcoding). On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel sqlite-devel ffmpeg
git clone --recursive https://github.com/KasunDodanwala/taiga.git
cd taiga
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./bin/taiga
```

To start it with your desktop session, minimized to the tray, create
`~/.config/autostart/moe.taiga.Taiga.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Taiga
Exec=/path/to/taiga --minimized
Icon=taiga
```

## Credits and license

- [Taiga](https://github.com/erengy/taiga), [Anisthesia](https://github.com/erengy/anisthesia) and
  [Anitomy](https://github.com/erengy/anitomy) by Eren Okka and contributors.
- The Linux port and the features above were written by Claude (Anthropic).

Like Taiga, this fork is licensed under the
[GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.html).

---

## About Taiga (upstream README)

[![](https://img.shields.io/github/license/erengy/taiga)](https://github.com/erengy/taiga/blob/master/LICENSE)
[![](https://img.shields.io/github/v/release/erengy/taiga)](https://taiga.moe/download.php)
[![](https://img.shields.io/discord/423475967051169813?logo=discord)](https://discord.gg/yeGNktZ)
[![](https://img.shields.io/github/sponsors/erengy?logo=github)](https://github.com/sponsors/erengy)

[Taiga](https://taiga.moe) is an open-source desktop application for Windows. It automatically detects the anime videos you watch on your computer and synchronizes your progress with [AniList](https://anilist.co), [Kitsu](https://kitsu.app) or [MyAnimeList](https://myanimelist.net). It helps you manage your anime library, discover new series, share watched episodes and download new ones.

### Links

- [Changelog](https://github.com/erengy/taiga/wiki/Changelog)
- [Contribution guidelines](https://github.com/erengy/taiga/wiki/Guidelines)
- [How to compile](https://github.com/erengy/taiga/wiki/How-to-Compile)

#### Related projects

- [Anime relations](https://github.com/erengy/anime-relations) (episode redirections)
- [Anisthesia](https://github.com/erengy/anisthesia) (media detection library)
- [Anitomy](https://github.com/erengy/anitomy) (anime video filename parser)
- [taiga.moe](https://github.com/erengy/taiga-moe) (home page of Taiga)

### License

Taiga is licensed under [GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.html).
