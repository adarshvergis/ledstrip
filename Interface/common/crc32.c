/**
 * @file crc32.c
 * @brief CRC-32 (ISO-HDLC / zlib), dependency free so it also builds on the host.
 */
#include "hw_common.h"

uint32_t hw_crc32(uint32_t crc, const void *data, size_t len)
{
    /* Nibble-wise table: small (64 B) and fast enough for config/firmware images. */
    static const uint32_t tbl[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
    }
    return ~crc;
}
