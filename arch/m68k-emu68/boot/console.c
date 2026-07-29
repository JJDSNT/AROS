/*
 * Minimal framebuffer console for the native Emu68 boot path.
 */

#include "boot.h"

extern const unsigned int fontWidth;
extern const unsigned int fontHeight;
extern const unsigned char fontData[256][14];

static uint8_t *console_framebuffer;
static uint32_t console_pitch;
static uint32_t console_width;
static uint32_t console_height;
static uint32_t console_columns;
static uint32_t console_rows;
static uint32_t console_x;
static uint32_t console_y;
static uint32_t console_bytes_per_pixel;

static void put_pixel(uint32_t x, uint32_t y, int foreground)
{
    uint8_t *pixel = console_framebuffer + y * console_pitch +
                     x * console_bytes_per_pixel;

    if (console_bytes_per_pixel == 2)
    {
        uint16_t color = foreground ? 0xffff : 0x0010;
        *(uint16_t *)pixel = color;
    }
    else
    {
        uint32_t color = foreground ? 0x00ffffff : 0x00000020;
        *(uint32_t *)pixel = color;
    }
}

static void clear_row(uint32_t row)
{
    uint32_t y;
    uint32_t x;

    for (y = row * fontHeight; y < (row + 1) * fontHeight; y++)
        for (x = 0; x < console_width; x++)
            put_pixel(x, y, 0);
}

void emu68_console_init(void *framebuffer, uint32_t pitch,
                        uint32_t width, uint32_t height)
{
    uint32_t row;

    console_framebuffer = framebuffer;
    console_pitch = pitch;
    console_width = width;
    console_height = height;
    console_x = 0;
    console_y = 0;

    if (!framebuffer || !width || !height || pitch < width * 2)
    {
        console_framebuffer = 0;
        return;
    }

    console_bytes_per_pixel = pitch / width;
    if (console_bytes_per_pixel != 2 && console_bytes_per_pixel != 4)
    {
        console_framebuffer = 0;
        return;
    }

    console_columns = width / fontWidth;
    console_rows = height / fontHeight;
    for (row = 0; row < console_rows; row++)
        clear_row(row);
}

int emu68_console_putc(int chr)
{
    uint32_t row;
    uint32_t column;
    uint8_t bits;

    if (!console_framebuffer)
        return 1;

    if (chr == '\r')
    {
        console_x = 0;
        return 1;
    }

    if (chr == '\n')
    {
        console_x = 0;
        console_y++;
    }
    else
    {
        for (row = 0; row < fontHeight; row++)
        {
            bits = fontData[(uint8_t)chr][row];
            for (column = 0; column < fontWidth; column++)
                put_pixel(console_x * fontWidth + column,
                          console_y * fontHeight + row,
                          bits & (0x80 >> column));
        }
        console_x++;
    }

    if (console_x >= console_columns)
    {
        console_x = 0;
        console_y++;
    }

    if (console_y >= console_rows)
    {
        console_y = 0;
        clear_row(console_y);
    }

    return 1;
}

void emu68_console_puts(const char *text)
{
    while (*text)
        emu68_console_putc(*text++);
}

