# cwatch

`cwatch` is a light local video library for Linux. It scans `~/media` by default,
groups videos by directory, shows a simple grid, and opens videos in an external
player.

`mpv` is the default player when available. When opened with `mpv`, `cwatch`
stores resume progress in `~/.local/state/cwatch/watch_later` and shows `resume`
timestamps in the library.

## Dependencies

- C compiler
- `make`
- `pkg-config`
- SDL2
- SDL2_ttf
- `mpv` for playback and resume support
- `ffmpeg` for thumbnails

If `mpv` is not installed, `cwatch` tries other players in this order:
`vlc`, `celluloid`, `parole`, then `xdg-open`.

Examples:

- Arch Linux / Artix Linux: `sudo pacman -S --needed git base-devel pkgconf sdl2 sdl2_ttf mpv ffmpeg`
- Debian / Ubuntu: `sudo apt install git build-essential pkg-config libsdl2-dev libsdl2-ttf-dev mpv ffmpeg`
- Fedora: `sudo dnf install git gcc make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel mpv ffmpeg`
- openSUSE: `sudo zypper install git gcc make pkg-config libSDL2-devel SDL2_ttf-devel mpv ffmpeg`
- Gentoo: `sudo emerge --ask dev-vcs/git sys-devel/gcc sys-devel/make virtual/pkgconfig media-libs/libsdl2 media-libs/sdl2-ttf media-video/mpv media-video/ffmpeg`
- Alpine: `sudo apk add git build-base pkgconf sdl2-dev sdl2_ttf-dev mpv ffmpeg`

## Full Install Example

Minimal install:

```sh
git clone https://github.com/Vifuddyxg/cwatch
cd cwatch
make
sudo make install-desktop
```

`install-desktop` installs:

- `cwatch` to `/usr/local/bin`
- `cwatch.desktop` to `/usr/local/share/applications`

If you only want the binary and do not need an application launcher:

```sh
sudo make install
```

Run after installing:

```sh
cwatch
cwatch /path/to/media
```

Run without installing:

```sh
./cwatch
./cwatch /path/to/media
```

If you installed the desktop entry, `cwatch` should appear in application
launchers that support desktop files:

```sh
rofi -show drun
wofi --show drun
```

## Features

- Scans one or more local media folders
- Groups nested videos by folder
- Opens videos with `mpv`, `vlc`, `celluloid`, `parole`, or `xdg-open`
- Saves resume positions when using `mpv`
- Lets you pin folders and mark videos as watched
- Generates thumbnails with `ffmpeg` when available

Supported video extensions: `.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.m4v`,
`.flv`, `.wmv`, and `.ogv`.

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
