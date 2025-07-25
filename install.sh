#!/usr/bin/bash

echo "Compiling & Installing..."
gcc -o loopctl loopctl_linux.c utils/utils.c $(pkg-config --cflags --libs dbus-1)
sudo cp loopctl /usr/local/bin/
echo "Installation complete"
echo -e "usage:\n loopctl (for repeating full current media infinite times)\n loopctl N (for repeating full current media N times)\n loopctl -p START END (for repeating current media from START point to END point infinite times)\n loopctl -p START END N (for repeating current media from START point to END point N times)"
