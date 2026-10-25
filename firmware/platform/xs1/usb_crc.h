/* USB 2.0 CRCs (spec §8.3.5), clean-room from the polynomials in the spec.
   Pure C, no hardware: built for both xcore and the host test. */
#ifndef USB_CRC_H
#define USB_CRC_H

#include <stddef.h>
#include <stdint.h>

/* CRC5 over the 11-bit token field (addr[7] | endp[4]), USB 2.0 §8.3.5.1.
   Polynomial x^5+x^2+1 (0x05), reflected. Returns the 5-bit residue already
   inverted, ready to place in bits [15:11] of the token. */
uint8_t usb_crc5(uint16_t data, int bits);

/* CRC16 over a data-packet payload, USB 2.0 §8.3.5.2. Polynomial
   x^16+x^15+x^2+1 (0x8005), reflected, init 0xFFFF, result inverted.
   Transmit low byte first. */
uint16_t usb_crc16(const uint8_t *data, size_t len);

/* Residue of CRC16 run over payload followed by the two received CRC bytes.
   Equals 0x800D for an intact data packet (USB 2.0 §8.3.5.2). */
uint16_t usb_crc16_residue(const uint8_t *data_with_crc, size_t len);

#define USB_CRC16_RESIDUE 0x800Du

#endif
