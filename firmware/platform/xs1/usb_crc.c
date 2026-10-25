#include "usb_crc.h"

uint8_t usb_crc5(uint16_t data, int bits)
{
    uint8_t crc = 0x1F;                 /* init all-ones (spec) */
    for (int i = 0; i < bits; i++) {
        int in = ((data >> i) & 1) ^ (crc & 1);
        crc >>= 1;
        if (in)
            crc ^= 0x14;                /* reflected 0x05 */
    }
    return (uint8_t)(~crc & 0x1F);
}

uint16_t usb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)  /* reflected 0x8005 */
                            : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

uint16_t usb_crc16_residue(const uint8_t *data_with_crc, size_t len)
{
    /* Same shift register, not inverted: the magic residue only appears on the
       raw register value. */
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data_with_crc[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                            : (uint16_t)(crc >> 1);
    }
    return crc;
}
