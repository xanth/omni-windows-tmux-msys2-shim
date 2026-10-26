# MSYS2 tmux shim for native Windows

A native `tmux.exe` that lets native Windows programs drive [MSYS2](https://www.msys2.org/)'s
tmux, which is unmodified upstream tmux, through the tmux CLI and control mode. It is one way
to give a tool that expects tmux, such as [Omnigent](https://github.com/omnigent-ai/omnigent)'s
native terminals, a real tmux on Windows.

## Why a shim

Stock MSYS2 tmux cannot be driven by a native Windows caller (for example Python's
`subprocess`):

- The MSYS2 runtime parses a native caller's command line itself. It globs and brace-expands
  bare words (`#{pane_pid}` arrives as `#pane_pid`), treats single quotes as quotes, and reads
  `@file` words as files.
- tmux reads `-c C:\dir` and `load-buffer C:\file` as relative paths.
- The server converts POSIX-looking pane arguments (`/k` becomes `K:/`).
- Control mode (`-C`) over pipes produces no output, because tmux on Cygwin reaches a client
  only through its tty.

## What the shim does

- It quotes each argument so the MSYS2 parser restores it exactly, and runs MSYS2's
  `tmux.exe` on the same standard handles.
- `-c X:\dir`, and the path of `load-buffer`, `save-buffer` and `source-file`, become
  `/x/...`. `-f nul` becomes `-f /dev/null`.
- A native pane program runs through `tmux.exe --native-exec`, which starts it with its argv
  unconverted and without the MSYS2 variables tmux adds.
- A control client (`-C`) on pipes runs in a pty from `script(1)`.
- A server started by `new-session -d` or `start-server` gets a console of its own, so it does
  not keep the caller's console alive.

## Install

1. Install MSYS2, then tmux: `pacman -S tmux`.
2. Recommended: in MSYS2's `/etc/nsswitch.conf`, set these two lines, the values Git for
   Windows ships:

   ```
   group: files # db
   db_home: env windows cygwin desc
   ```

   Without them, each MSYS2 process can take about 470 ms to start on an Entra ID-joined
   machine (13 ms with them, measured), and `HOME` in panes is MSYS2's home, not the Windows
   profile.
3. Build the shim in an MSYS2 UCRT64 shell (`pacman -S mingw-w64-ucrt-x86_64-gcc`):

   ```
   gcc -O2 -Wall -municode -o tmux.exe tmux-shim.c
   ```

4. Put the folder that holds the shim's `tmux.exe` first on `PATH`. Do not add MSYS2's
   `usr\bin`: the shim finds tmux by absolute path.

The shim runs `%MSYS2_TMUX%` if it is set. Otherwise it looks for `usr\bin\tmux.exe` under
`%MSYS2_ROOT%`, `%USERPROFILE%\msys64` and `C:\msys64`, in that order.

Check: `tmux -V` prints MSYS2's tmux version, for example `tmux 3.8`.

## Status

Tested on Windows 11 Pro 10.0.26300 with MSYS2 tmux 3.8:

- tmux conformance checks used by Omnigent: 104 of 110 pass with `-S` and `-L` servers, argv
  and string panes. The one gap against tmux 3.6 is cosmetic (`#{bracket_paste_flag}` prints
  `0`), and control mode has no gaps.
- Omnigent's `claude-native` harness ran through it with an earlier version of the Omnigent
  Windows changes in [omnigent-ai/omnigent#11590](https://github.com/omnigent-ai/omnigent/pull/11590):
  sessions answered, two ran at once, and the web terminal typed, pasted, resized, scrolled
  and reattached. The current version of that PR has not been tested with this shim.

Known differences from tmux on Linux:

- `#{pane_pid}` is a Cygwin PID, not the Windows PID of the pane program.
- `#{pane_current_path}` is a POSIX path (`/c/...`).
- Probing a server that has exited takes about 2 s, because Cygwin emulates its socket over
  loopback TCP.

This project is not affiliated with tmux, MSYS2 or Omnigent.
