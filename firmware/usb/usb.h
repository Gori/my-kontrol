#pragma once
/* USB device: two MIDI cables (Keys, DAW) plus a DFU runtime interface. */

void usb_init(void);
void usb_task(void);    /* main loop: USB stack, MIDI in, MIDI out queue */
