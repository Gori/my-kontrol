#!/bin/sh
# End-to-end check of mk-sim --stdio --fake-time: handshake, MODES, a page
# with one slot, a knob touch that shows the slot name. make check-stdio
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
exit $fail
