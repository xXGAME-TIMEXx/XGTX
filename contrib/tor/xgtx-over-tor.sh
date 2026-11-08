#!/bin/bash

DIR=$(realpath $(dirname $0))

echo "Checking xgtxd..."
xgtxd=""
for dir in \
  . \
  "$DIR" \
  "$DIR/../.." \
  "$DIR/build/release/bin" \
  "$DIR/../../build/release/bin" \
  "$DIR/build/Linux/mycoin-development/release/bin" \
  "$DIR/../../build/Linux/mycoin-development/release/bin" \
  "$DIR/build/Windows/mycoin-development/release/bin" \
  "$DIR/../../build/Windows/mycoin-development/release/bin"
do
  if test -x "$dir/xgtxd"
  then
    xgtxd="$dir/xgtxd"
    break
  fi
done
if test -z "$xgtxd"
then
  echo "xgtxd not found"
  exit 1
fi
echo "Found: $xgtxd"

TORDIR="$DIR/xgtx-over-tor"
TORRC="$TORDIR/torrc"
HOSTNAMEFILE="$TORDIR/hostname"
echo "Creating configuration..."
mkdir -p "$TORDIR"
chmod 700 "$TORDIR"
rm -f "$TORRC"
cat << EOF > "$TORRC"
ControlSocket $TORDIR/control
ControlSocketsGroupWritable 1
CookieAuthentication 1
CookieAuthFile $TORDIR/control.authcookie
CookieAuthFileGroupReadable 1
HiddenServiceDir $TORDIR
HiddenServicePort 48083 127.0.0.1:48083
EOF

echo "Starting Tor..."
nohup tor -f "$TORRC" 2> "$TORDIR/tor.stderr" 1> "$TORDIR/tor.stdout" &
ready=0
for i in `seq 10`
do
  sleep 1
  if test -f "$HOSTNAMEFILE"
  then
    ready=1
    break
  fi
done
if test "$ready" = 0
then
  echo "Error starting Tor"
  cat "$TORDIR/tor.stdout"
  exit 1
fi

echo "Starting xgtxd..."
HOSTNAME=$(cat "$HOSTNAMEFILE")
"$xgtxd" \
  --anonymous-inbound "$HOSTNAME":48083,127.0.0.1:48083,25 --tx-proxy tor,127.0.0.1:9050,10 \
  --detach
ready=0
for i in `seq 10`
do
  sleep 1
  status=$("$xgtxd" status)
  echo "$status" | grep -q "Height:"
  if test $? = 0
  then
    ready=1
    break
  fi
done
if test "$ready" = 0
then
  echo "Error starting xgtxd"
  tail -n 400 "$HOME/.xXGAME-TIMEXx/xgtxd.log" | grep -Ev stacktrace\|"Error: Couldn't connect to daemon:" | tail -n 20
  exit 1
fi

echo "Ready. Your Tor hidden service is $HOSTNAME"
