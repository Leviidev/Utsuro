#!/bin/bash
# ex-games.sh: extracts the two big RARs one after the other with RARLAB's unrar (unar and 7zz cannot read these)
B="/Volumes/macOS Storage"; U=/tmp/rarbin/rar/unrar
one() {  # one <archive> <final folder name>
  W="$B/_work_$2"; rm -rf "$W"; mkdir -p "$W"
  $U x -y -idq "$1" "$W/" > /tmp/ex-$2.log 2>&1; rc=$?
  inner=$(ls "$W" | head -1)
  if [ $rc -eq 0 ] && [ -n "$inner" ]; then rm -rf "$B/utsuro-games/$2"; mv "$W/$inner" "$B/utsuro-games/$2"; rmdir "$W" 2>/dev/null; fi
  echo "$2 rc=$rc size=$(du -sh "$B/utsuro-games/$2" 2>/dev/null | cut -f1) $(tail -1 /tmp/ex-$2.log)" >> /tmp/ex-games.status
}
one "$B/Downloads/FDM/xboxonedumpz/Ubisoft.TrackmaniaNext_1.6.2.0_neutral__ngz4m417e0mpw.rar" trackmania
one "$B/Downloads/FDM/(Forza Horizon 2) Anthem_1.0.0.64_x64__8wekyb3d8bbwe.rar" forza-horizon-2
echo finished >> /tmp/ex-games.status
