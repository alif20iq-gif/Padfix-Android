#!/system/bin/sh
# Padfix V11 - Android. Dua mode, dikenali dari user:
#   Termux (compile)      : sh /sdcard/Code/padfix.sh
#   Shell ADB/Shizuku (jalankan): sh /sdcard/Code/padfix.sh [opsi padfix]
#   Berhenti              : sh /sdcard/Code/padfix.sh stop   (di shell ADB/Shizuku)
DIR="${PADFIX_DIR:-/sdcard/Code}"
BIN=/data/local/tmp/padfix
WWW=/data/local/tmp/padfix_index.html
LOG=/data/local/tmp/padfix.log
stop_old() { pkill -x padfix >/dev/null 2>&1; sleep 1; }

if [ "$(id -u)" = "2000" ]; then
  [ "$1" = "stop" ] && { stop_old; echo "padfix dihentikan."; exit 0; }
  [ -f "$DIR/padfix" ] && [ -f "$DIR/index.html" ] || { echo "Butuh $DIR/padfix dan $DIR/index.html. Compile dulu di Termux."; exit 1; }
  stop_old
  cp "$DIR/padfix" "$BIN" && cp "$DIR/index.html" "$WWW" && chmod 755 "$BIN" || { echo "Gagal menyalin ke /data/local/tmp"; exit 1; }
  echo "Menjalankan padfix. LEPASKAN kedua stik ~3 detik pertama."
  nohup "$BIN" -w "$WWW" "$@" >"$LOG" 2>&1 &
  sleep 5; cat "$LOG"
  [ -n "$(pidof padfix 2>/dev/null)" ] || { echo "padfix berhenti, lihat $LOG"; exit 1; }
else
  CC=$(command -v clang || command -v cc)
  [ -n "$CC" ] || { echo "Jalankan: pkg install clang"; exit 1; }
  [ -f "$DIR/padfix.c" ] || { echo "Tidak ada $DIR/padfix.c"; exit 1; }
  "$CC" -O2 -pthread -o "$HOME/padfix" "$DIR/padfix.c" || { echo "Compile gagal."; exit 1; }
  cp "$HOME/padfix" "$DIR/padfix" || { echo "Gagal menyalin ke $DIR"; exit 1; }
  echo "Compile selesai (BUKAN bukti fitur jalan; uji di perangkat). Lanjut di shell ADB/Shizuku: sh $DIR/padfix.sh"
fi