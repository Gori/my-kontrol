"""Log everything the M32 sends on both MIDI ports, with timestamps.

usage: m32monitor.py [logfile]      (Ctrl-C to stop)
Debug events from the firmware (F0 7D kind a b F7) are decoded:
kind 1 = button index/down, 2 = encoder direction/edges, 3 = knob touch mask.
"""
import sys
import time
import mido

out = open(sys.argv[1], 'a', buffering=1) if len(sys.argv) > 1 else sys.stdout
ports = {n: mido.open_input(n) for n in mido.get_input_names() if 'M32' in n}
t0 = time.time()
print(f'# listening on {list(ports)}', file=out)
while True:
    for name, p in ports.items():
        for m in p.iter_pending():
            t = f'{time.time() - t0:8.3f}'
            tag = 'DAW ' if 'DAW' in name else 'KEYS'
            if m.type == 'sysex' and m.data[:1] == (0x7D,):
                kind, a, b = m.data[1:4]
                if kind == 1:
                    s = f'button 0x{a:02X} {"down" if b else "up"}'
                elif kind == 2:
                    s = f'encoder {"-" if a else "+"}{b} edges'
                else:
                    s = f'knob touch mask {(a | b << 7):08b}'
                print(f'{t} {tag} {s}', file=out)
            else:
                print(f'{t} {tag} {m}', file=out)
    time.sleep(0.001)
