#!/bin/bash
# Boots every copy of the game in ../ISOs to its main menu on the PC build and checks the log: the menu mouse is on,
# the menu screens are recognized and nothing crashed. One line per copy; a copy that does not reach the main menu in
# three minutes, or crashes, fails. The game module of each edition is passed as the launcher would.
#
#   tools/probar_isos.sh            (from the repository; the PC build in out/pc must be current)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISOS="$ROOT/../ISOs"
cd "$ROOT/out/pc" || exit 1
fallos=0
for par in "es:Need for Speed Most Wanted (España).iso" "es:Need for Speed - Most Wanted (France).iso" \
           "es:Need for Speed - Most Wanted (Germany).iso" "es:Need for Speed - Most Wanted (Italy).iso" \
           "usa:Need for Speed - Most Wanted (USA).iso" "eu:Need for Speed - Most Wanted (Europe).iso"; do
  modulo=${par%%:*}
  iso=${par#*:}
  [ -f "$ISOS/$iso" ] || { echo "$iso: no esta"; continue; }
  antes=$(ls logs/nfsmw_*.log 2>/dev/null | wc -l)
  ./nfsmw.exe --nfsmw_lanzador_omitir=true --nfsmw_menu_raton=true --nfsmw_modulo=$modulo \
    "--nfsmw_juego=$ISOS/$iso" >/dev/null 2>&1 &
  inicio=$(date +%s)
  menu=no
  while [ $(( $(date +%s) - inicio )) -lt 180 ]; do
    if [ "$(ls logs/nfsmw_*.log | wc -l)" -gt "$antes" ]; then
      log=$(ls -t logs/nfsmw_*.log | head -1)
      grep -q "pantalla: MainMenu" "$log" && { menu=si; break; }
      grep -q "violation\|No function registered" "$log" && break
    fi
    read -t 3 < /dev/zero 2>/dev/null || true
  done
  log=$(ls -t logs/nfsmw_*.log | head -1)
  raton=$(grep -c "mando virtual del raton" "$log")
  pantallas=$(grep -c "pantalla: " "$log")
  errores=$(grep -c "violation\|No function registered" "$log")
  bien=si
  [ "$menu" = si ] && [ "$raton" -ge 1 ] && [ "$pantallas" -ge 3 ] && [ "$errores" -eq 0 ] || { bien=NO; fallos=$((fallos + 1)); }
  echo "$iso ($modulo): $bien  menu=$menu en $(( $(date +%s) - inicio ))s, raton=$raton, pantallas=$pantallas, errores=$errores"
  taskkill //IM nfsmw.exe //F >/dev/null 2>&1
  read -t 3 < /dev/zero 2>/dev/null || true
done
echo "fallos: $fallos"
exit $fallos
