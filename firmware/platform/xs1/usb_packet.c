#include "usb_packet.h"
#include "usb_crc.h"

uint8_t usb_pid_byte(usb_pid_t pid)
{
    uint8_t p = (uint8_t)(pid & 0x0F);
    return (uint8_t)(p | ((uint8_t)(~p) << 4));
}

int usb_pid_valid(uint8_t b)
{
    return ((b >> 4) ^ 0x0F) == (b & 0x0F);
}

usb_pid_t usb_pid_of(uint8_t b)
{
    return (usb_pid_t)(b & 0x0F);
}

size_t usb_build_token(uint8_t out[3], usb_pid_t pid, uint8_t addr, uint8_t endp)
{
    uint16_t field = (uint16_t)((addr & 0x7F) | ((endp & 0x0F) << 7));
    uint8_t crc = usb_crc5(field, 11);
    uint16_t full = (uint16_t)(field | ((uint16_t)crc << 11));
    out[0] = usb_pid_byte(pid);
    out[1] = (uint8_t)(full & 0xFF);
    out[2] = (uint8_t)(full >> 8);
    return 3;
}

size_t usb_build_data(uint8_t *out, usb_pid_t data_pid,
                      const uint8_t *payload, size_t len)
{
    out[0] = usb_pid_byte(data_pid);
    for (size_t i = 0; i < len; i++)
        out[1 + i] = payload[i];
    uint16_t crc = usb_crc16(payload, len);
    out[1 + len] = (uint8_t)(crc & 0xFF);
    out[2 + len] = (uint8_t)(crc >> 8);
    return len + 3;
}

int usb_check_data(const uint8_t *buf, size_t total, size_t *payload_len)
{
    if (total < 3 || !usb_pid_valid(buf[0]))
        return 0;
    size_t plen = total - 3;
    uint16_t crc = usb_crc16(buf + 1, plen);
    uint16_t got = (uint16_t)(buf[1 + plen] | (buf[2 + plen] << 8));
    if (crc != got)
        return 0;
    if (payload_len)
        *payload_len = plen;
    return 1;
}

void ulpi_reg_write(ulpi_io *io, uint8_t addr, uint8_t value)
{
    io->cmd(io, ULPI_TXCMD_REGWRITE(addr));   /* TX CMD, wait NXT */
    io->write(io, value);                     /* data, wait NXT   */
    io->stp(io);                              /* STP ends the write */
}

uint8_t ulpi_reg_read(ulpi_io *io, uint8_t addr)
{
    io->cmd(io, ULPI_TXCMD_REGREAD(addr));    /* TX CMD, wait NXT */
    io->turn(io, 1);                          /* PHY drives data (DIR high) */
    uint8_t v = io->read(io);
    io->turn(io, 0);                          /* bus back to the link */
    return v;
}

uint8_t ulpi_phy_init_fs(ulpi_io *io)
{
    /* Device, full speed: powered (SUSPENDM), FS termination, FS transceiver. */
    ulpi_reg_write(io, ULPI_REG_FUNC_CTRL,
                   ULPI_FC_SUSPENDM | ULPI_FC_TERMSELECT | ULPI_FC_XCVRSEL_FS);
    /* Device role: no host pulldowns on D+/D-. */
    ulpi_reg_write(io, ULPI_REG_OTG_CTRL, 0x00);
    return ulpi_reg_read(io, ULPI_REG_FUNC_CTRL);
}
