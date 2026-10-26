#!/usr/bin/env python3
"""Read the S MK1 bring-up probe (the hybrid image: our tile 0 next to NI's tile 1) from the
Mac, and drive its LED walk. The probe's messages come out of NI's own MIDI port as control
changes on channel 16 (firmware/boards/s-mk1/probe.c has the format).

usage:
  s49probe.py [--port NAME] [--log FILE]        print what comes in (Ctrl-C to stop)
  s49probe.py --led CHAIN BIT                   light only that bit (chain A, B or C) and listen
  s49probe.py --walk | --stop | --normal        LED walk on / off / lights back to the UI
  s49probe.py --dump                            ask for everything now
  s49probe.py --view on|off                     the hex view on the displays

Prints, as they arrive: the input bits (which are pressed, which changed), keybed bytes,
the sensor replies (0x12 knobs, 0x15 touch: the bytes that changed), the LED bit lit.
Needs mido (pip install mido python-rtmidi).
"""
import argparse, sys, time
import mido

CMD_LED, CMD_NORMAL, CMD_WALK, CMD_STOP, CMD_DUMP, CMD_VIEW = 1, 2, 3, 4, 5, 6
NAMES = {1: 'inputs', 2: 's12', 3: 's15', 4: 'keybed', 5: 'led'}


def command(out, cmd, *args):
    for i, v in enumerate(args):                # arguments first, CC 81..83, then CC 80
        out.send(mido.Message('control_change', channel=15, control=81 + i, value=v & 0x7F))
    out.send(mido.Message('control_change', channel=15, control=80, value=cmd))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', default='KOMPLETE KONTROL', help='part of the MIDI port name')
    ap.add_argument('--log')
    ap.add_argument('--led', nargs=2, metavar=('CHAIN', 'BIT'))
    ap.add_argument('--walk', action='store_true')
    ap.add_argument('--stop', action='store_true')
    ap.add_argument('--normal', action='store_true')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--view', choices=['on', 'off'])
    a = ap.parse_args()
    log = open(a.log, 'a', buffering=1) if a.log else None
    ins = [n for n in mido.get_input_names() if a.port in n]
    outs = [n for n in mido.get_output_names() if a.port in n]
    if not ins:
        sys.exit('no MIDI input with "%s" in its name: %s' % (a.port, mido.get_input_names()))
    inp = mido.open_input(ins[0])
    out = mido.open_output(outs[0]) if outs else None
    if out:
        if a.led:
            chain = 'ABC'.index(a.led[0].upper())
            bit = int(a.led[1])
            command(out, CMD_LED, chain, bit >> 7, bit)
        for flag, cmd in ((a.walk, CMD_WALK), (a.stop, CMD_STOP), (a.normal, CMD_NORMAL), (a.dump, CMD_DUMP)):
            if flag:
                command(out, cmd)
        if a.view:
            command(out, CMD_VIEW, 1 if a.view == 'on' else 0)

    def say(s):
        line = '%8.3f %s' % (time.time() - t0, s)
        print(line)
        if log:
            print(line, file=log)

    t0 = time.time()
    say('listening on %s' % ins[0])
    msg, last = None, {}
    for m in inp:
        if m.type != 'control_change' or m.channel != 15:
            say(str(m))
            continue
        if m.control == 15:
            msg = [m.value]
        elif msg is not None and m.control in (16, 17):
            msg.append(m.value | (m.control - 16) << 7)
        elif msg is not None and m.control == 14:
            kind, data = msg[0], bytes(msg[1:])
            msg = None
            if sum(data) & 0x7F != m.value:
                say('%s: bad checksum, %s' % (NAMES.get(kind, kind), data.hex(' ')))
                continue
            if kind == 1:
                bits = int.from_bytes(data, 'little')
                was = last.get(1, bits)
                ch = ['%s%d' % ('+' if bits >> i & 1 else '-', i) for i in range(32) if (bits ^ was) >> i & 1]
                say('inputs %08x pressed %s%s' % (bits, [i for i in range(32) if bits >> i & 1],
                                                  ' changed ' + ' '.join(ch) if ch else ''))
                last[1] = bits
            elif kind in (2, 3):
                was = last.get(kind, data)
                ch = ['%d:%02x' % (i, x) for i, x in enumerate(data) if i < len(was) and was[i] != x]
                say('%s %s%s' % (NAMES[kind], data.hex(' '), '  changed ' + ' '.join(ch) if ch else ''))
                last[kind] = data
            elif kind == 4:
                say('keybed %s' % data.hex(' '))
            elif kind == 5 and len(data) == 3:
                say('led chain %s bit %d' % ('ABC'[data[0]], data[1] << 8 | data[2]))


if __name__ == '__main__':
    main()
