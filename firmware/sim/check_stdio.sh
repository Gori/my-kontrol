#!/bin/sh
# End-to-end check of mk-sim --stdio --fake-time: handshake, MODES, a page
# with one slot, a knob touch that shows the slot name; then the 5-pin MIDI
# jacks. make check-stdio
sim=${1:-build/mk-sim}
out=$("$sim" --stdio --fake-time <<'EOF'
tick 1300
# HELLO v2, features: parameters; name "Sim", version "1.0"
in 1 F0 7D 4D 4B 00 02 00 01 03 53 69 6D 03 31 2E 30 F7
# MODES: plug-in, enabled, keyboard's own title
in 1 F0 7D 4D 4B 09 01 00 01 00 F7
# the keyboard answers VIEW from its main loop; a host sends the page after that
tick 10
# PAGE gen 1, plug-in, bank 0 of 1, "Synth"; SLOT knob 0 continuous "Cutoff"; VALUE "2.4 kHz"
in 1 F0 7D 4D 4B 20 01 00 00 01 05 53 79 6E 74 68 00 F7
in 1 F0 7D 4D 4B 21 01 00 01 00 00 00 00 00 00 00 06 43 75 74 6F 66 66 00 F7
in 1 F0 7D 4D 4B 22 01 00 40 00 07 32 2E 34 20 6B 48 7A F7
tick 1500
find Synth
# touch knob 0 and turn it +1
event 4 0 1
event 3 0 1
tick 30
find Cutoff
find 2.4 kHz
screen
lights
quit
EOF
)
din=$("$sim" --stdio --fake-time <<'EOF'
tick 1300
# the lowest key: Keys port and 5-pin (MIDI Out: USB + MIDI)
event 0 0 100
tick 5
din-out
# 5-pin in with running status: to the computer on EXT, not back out (MIDI Thru off)
din-in 92 28 32 29 32
tick 5
din-out
# EXT from the computer goes out the 5-pin, at MIDI speed
in 2 F0 7E 7F 06 01 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 F7
tick 3
din-out
tick 10
din-out
# unplugged: the keys go on to the 5-pin
usb 0
event 0 0 0
tick 5
din-out
quit
EOF
)
# a 1 KB sysex each way: the computer waits for the 5-pin, nothing is cut
sx="F0$(i=1; while [ $i -lt 1023 ]; do printf ' %02X' $((i * 7 % 128)); i=$((i + 1)); done) F7"
long=$("$sim" --stdio --fake-time <<EOF
tick 1300
in 2 $sx
tick 400
din-out
din-in $sx
tick 400
quit
EOF
)
# the M32 has no aftertouch or expression pedal: their settings stay hidden
menu=$("$sim" --stdio --fake-time <<'EOF'
tick 1300
event 1 25 1
event 1 25 0
event 2 0 2
event 1 25 1
event 1 25 0
event 2 0 20
find All Notes Off
find Aftertouch
event 1 22 1
event 2 0 1
event 1 25 1
event 2 0 30
find Pedal Ring CC
find Expression CC
quit
EOF
)
fail=0
check() {       # check <description> <command...>
    d=$1; shift
    if "$@"; then echo "ok    $d"; else echo "FAIL  $d"; fail=1; fi
}
has() { printf '%s\n' "$out" | grep -Eq "$1"; }
nth_find() { printf '%s\n' "$out" | grep -E '^(found|notfound)' | sed -n "$1p" | grep -q '^found '; }
count() { [ "$(printf '%s\n' "$out" | sed -n "/^$1\$/,/^end\$/p" | grep -Ec "$2")" = "$3" ]; }

check "HI"                        has '^out 1 F0 7D 4D 4B 02 01 F7$'
check "IDENT proto 2, board 1"    has '^out 1 F0 7D 4D 4B 01 02 01( [0-9A-F]{2})+ F7$'
check "VIEW plug-in after MODES"  has '^out 1 F0 7D 4D 4B 05 00 F7$'
check "page title on screen"      nth_find 1
check "knob touch to the host"    has '^out 1 9F 40 7F$'
check "knob turn to the host"     has '^out 1 BF 10 01$'
check "slot name on screen"       nth_find 2
check "slot value on screen"      nth_find 3
check "screen is 32 x 128"        count screen '^[#.]{128}$' 32
check "lights for 26 controls"    count lights '^[0-9]+ [0-9]+$' 26
check "Plug-in light on"          has '^17 255$'
check "no errors"                 eval '! has "^error"'
[ $fail = 0 ] || { echo "--- transcript"; printf '%s\n' "$out"; }

dfail=$fail
dn() { printf '%s\n' "$din" | grep '^din-out' | sed -n "$1p"; }
dhas() { printf '%s\n' "$din" | grep -Eq "$1"; }
check "5-pin: key on Keys"        dhas '^out 0 90 29 64$'
check "5-pin: key on 5-pin out"   eval '[ "$(dn 1)" = "din-out 90 29 64" ]'
check "5-pin in to EXT"           eval 'dhas "^out 2 92 28 32$" && dhas "^out 2 92 29 32$"'
check "5-pin in not echoed"       eval '[ "$(dn 2)" = "din-out" ]'
check "EXT sysex paced"           eval '[ "$(dn 3)" = "din-out F0 7E 7F 06 01 00 01 02 03 04" ]'
check "EXT sysex completes"       eval '[ "$(dn 4)" = "din-out 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 F7" ]'
check "unplugged: 5-pin only"     eval '[ "$(dn 5)" = "din-out 80 29 00" ] && ! dhas "^out 0 80"'
check "5-pin: no errors"          eval '! dhas "^error"'
check "1 KB sysex to the 5-pin"   eval 'printf "%s\n" "$long" | grep -qx "din-out $sx"'
check "1 KB sysex from the 5-pin" eval 'printf "%s\n" "$long" | grep -qx "out 2 $sx"'
mfind() { printf '%s\n' "$menu" | grep -E '^(found|notfound)' | sed -n "$1p" | grep -q "^$2"; }
check "menu: Keys ends at All Notes Off" mfind 1 'found '
check "menu: no Aftertouch on the M32"   mfind 2 notfound
check "menu: Controls ends at Pedal Ring" mfind 3 'found '
check "menu: no Expression CC on the M32" mfind 4 notfound
[ $fail = $dfail ] || { echo "--- 5-pin transcript"; printf '%s\n' "$din" "$long" | cut -c1-200; }
exit $fail
