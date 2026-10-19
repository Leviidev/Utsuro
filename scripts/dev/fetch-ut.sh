#!/bin/bash
set -e
B="/Volumes/macOS Storage"; W="$B/_work"; mkdir -p "$W"
curl -sL -C - -o "$W/undertale.7z" "https://archive.org/download/undertale.-7z/Undertale.7z"
7zz x -y -o"$W/ut7z" "$W/undertale.7z" > /tmp/fetch-ut7.log 2>&1
rm -f "$W/undertale.7z"
f=$(ls "$W/ut7z"/*1.0.0.8*.rar | head -1)
mkdir -p "$W/utx"; unar -q -o "$W/utx" "$f" > /tmp/fetch-utx.log 2>&1
inner=$(ls "$W/utx" | head -1)
rm -rf "$B/utsuro-games/undertale"; mv "$W/utx/$inner" "$B/utsuro-games/undertale"
rm -rf "$W"
echo "undertale ready" >> /tmp/fetch-ut.status
