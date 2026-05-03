# cwatch

`cwatch` is a light local video library for Linux. It scans `~/media` by default,
groups videos by directory, shows a simple grid, and opens videos in an external
player.

`mpv` is the default player when available. When opened with `mpv`, `cwatch`
stores resume progress in `~/.local/state/cwatch/watch_later` and shows `resume`
timestamps in the library.

Supported video extensions: `.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.m4v`,
`.flv`, `.wmv`, and `.ogv`.

## Features

- Scans one or more local media folders
- Groups nested videos by folder
- Opens videos with `mpv`, `vlc`, `celluloid`, `parole`, or `xdg-open`
- Saves resume positions when using `mpv`
- Lets you pin folders and mark videos as watched
- Generates thumbnails with `ffmpeg` when available

## Layout

```text
~/media/
  Movies/
    Some Movie.mkv
  Series Name/
    Season 1/
      Episode 01.mp4
      Episode 02.mp4
```

Nested folders are supported. Videos are grouped by their directory path.
Files directly in `~/media` appear in `Videos`.

## Dependencies

Build dependencies:

- C compiler
- `make`
- `pkg-config`
- SDL2
- SDL2_ttf

Runtime dependencies:

- One supported external player: `mpv` is recommended
- `ffmpeg` for thumbnails, optional

## Build

```sh
make
```

## Install

```sh
sudo make install
```

Install the desktop launcher for application menus and `drun` launchers:

```sh
sudo make install-desktop
```

## Run

```sh
./cwatch
./cwatch /path/to/media
```

After installing:

```sh
cwatch
cwatch /path/to/media
```

## App launchers

`sudo make install-desktop` installs `cwatch.desktop` into
`/usr/local/share/applications`. Launchers that support desktop entries, such as
`rofi -show drun`, `wofi --show drun`, or an application menu can then find
`cwatch`.

```sh
rofi -show drun
```

If your launcher does not show `cwatch` immediately, refresh its desktop-entry
cache or restart the launcher.

## Controls

- Click, `Enter`, or `Space`: open selected video or folder
- Right click: pin or unpin a folder
- Arrow keys: move selection
- Mouse wheel / PageUp / PageDown: scroll
- `/`: search
- `d`: add another folder path to the current library
- `i`: pin/unpin selected folder
- `m`: mark/unmark selected video as watched
- `p`: switch player
- `r`: rescan library
- `Backspace`: go back from folder view
- `Esc`: close search, go back, or quit
