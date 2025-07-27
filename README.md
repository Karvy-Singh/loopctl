# loopctl
Repeat a media/section of media "x" number of times on Linux.

## WHY??
Well, sometimes we (i do) may find ourselves in scenarios wherein i want to play a song lets say two times before switching to next one in my playist, and some players provide either repeat once or forever without custom repeat numbers/ some provide no option to repeat.

Also most players don't allow to repeat a certain part of media, example in case of a 8 min if i want to hear a certain 1-2 min again, another one dance teachers wanting to teach on a certain part of song.

OR any other use case it may be useful to you as a tool.

## Prerequisites/Dependencies
- `libdbus-1‑dev` (or equivalent DBus development package)
- `GCC` and `pkg-config` (if not already)

## Installation

*  Clone the repository and enter its directory:
   ```bash
   git clone https://github.com/Karvy-Singh/loopctl.git
   cd loopctl
   ```
*  Run Installation script
   ### NOTE: it installs the executable to `/usr/local/bin`
   ```bash
   chmod +x install.sh
   ./install.sh
   ```
## USAGE
```bash
loopctl [OPTIONS]
```
* `loopctl`: Loop the full current media infinitely.
* `loopctl N`: Loop the full current media N times.
* `loopctl -p START END`: Loop from START to END (in seconds) infinitely.
* `loopctl -p START END N`: Loop from START to END (in seconds) N times.

## Examples
* Loop the current track 5 times:
```bash
loopctl 5
```

* Loop a segment from 30s to 60s infinitely:
```bash
loopctl -p 30 60
```

* Loop from 1m20s (80s) to 2m (120s) three times:
```bash
loopctl -p 80 120 3
```
## Contributions
Open to any betterment/features to enhance/ease the use.
