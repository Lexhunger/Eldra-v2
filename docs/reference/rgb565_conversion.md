# RGB565 Conversion Reference

Quick cheat sheet for converting 24-bit RGB to 16-bit RGB565.

---

## Formula

```c
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) |
           ((g & 0xFC) << 3) |
           (b >> 3);
}
```

## Notes
- Keep values in the 0-255 range before conversion.
- When defining palettes, store values as RGB565 constants to avoid recomputing on every draw.
- Swapping byte order may be required depending on display bus endianness; confirm with the panel driver before use.
