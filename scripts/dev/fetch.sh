#!/bin/bash
# fetch.sh <archive.org url> <folder name>: download, extract into /Volumes/GTAV/utsuro-games/<folder>, delete the archive
set -e
D=/Volumes/GTAV/utsuro-games
mkdir -p $D/_downloads $D/_x
f=$D/_downloads/$2.7z
curl -sL -C - -o "$f" "$1"
rm -rf "$D/_x/$2"; mkdir -p "$D/_x/$2"
7zz x -y -o"$D/_x/$2" "$f" > /tmp/fetch-$2.log 2>&1
rm -f "$f"
inner=$(ls "$D/_x/$2" | head -1)
mv "$D/_x/$2/$inner" "$D/$2"
rmdir "$D/_x/$2" 2>/dev/null || true
echo "done $2: $(du -sh "$D/$2" | cut -f1)" >> /tmp/fetch-status.log
