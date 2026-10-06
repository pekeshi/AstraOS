#include "ps2.h"

#include <stdint.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_WAIT_LIMIT 1000000U

static int initialized;
static uint8_t shift_state;
static int caps_lock;
static int extended;
static uint32_t pause_remaining;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int wait_input_clear(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (status == 0xFF) {
            return 0;
        }
        if ((status & 2U) == 0) {
            return 1;
        }
    }
    return 0;
}

static int wait_output_full(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (status == 0xFF) {
            return 0;
        }
        if ((status & 1U) != 0) {
            return 1;
        }
    }
    return 0;
}

static int controller_command(uint8_t command) {
    if (!wait_input_clear()) {
        return 0;
    }
    outb(PS2_STATUS_PORT, command);
    return 1;
}

static int wait_keyboard_response(uint8_t *response) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (status == 0xFF) {
            return 0;
        }
        if ((status & 1U) != 0) {
            uint8_t value = inb(PS2_DATA_PORT);

            if ((status & 0xC0U) != 0) {
                return 0;
            }
            if ((status & 0x20U) == 0) {
                *response = value;
                return 1;
            }
        }
    }
    return 0;
}

static int enable_keyboard_scanning(void) {
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        if (!wait_input_clear()) {
            return 0;
        }
        outb(PS2_DATA_PORT, 0xF4);
        for (uint32_t response_index = 0; response_index < 8;
             ++response_index) {
            uint8_t response;

            if (!wait_keyboard_response(&response)) {
                return 0;
            }
            if (response == 0xFA) {
                return 1;
            }
            if (response == 0xFE) {
                break;
            }
            if (response == 0xFC || response == 0xFD) {
                return 0;
            }
        }
    }
    return 0;
}

int ps2_keyboard_init(void) {
    uint8_t configuration;
    uint8_t status;

    initialized = 0;
    shift_state = 0;
    caps_lock = 0;
    extended = 0;
    pause_remaining = 0;

    status = inb(PS2_STATUS_PORT);
    if (status == 0xFF) {
        return 0;
    }
    for (uint32_t i = 0; (status & 1U) != 0 && i < 32; ++i) {
        (void)inb(PS2_DATA_PORT);
        status = inb(PS2_STATUS_PORT);
        if (status == 0xFF) {
            return 0;
        }
    }
    if ((status & 1U) != 0) {
        return 0;
    }
    if (!controller_command(0x20) || !wait_output_full()) {
        return 0;
    }
    configuration = inb(PS2_DATA_PORT);
    if (configuration == 0xFF ||
        !controller_command(0x60) || !wait_input_clear()) {
        return 0;
    }
    configuration = (uint8_t)((configuration | 0x40U) & ~0x13U);
    outb(PS2_DATA_PORT, configuration);

    if (!controller_command(0xAE) || !enable_keyboard_scanning()) {
        return 0;
    }
    initialized = 1;
    return 1;
}

static int decode_scan_code(uint8_t scan_code) {
    int shifted = shift_state != 0;

    if (pause_remaining != 0) {
        --pause_remaining;
        return -1;
    }
    if (scan_code == 0xE1) {
        pause_remaining = 5;
        return -1;
    }
    if (extended) {
        extended = 0;
        switch (scan_code) {
        case 0x1C: return '\r';
        case 0x35: return shifted ? '?' : '/';
        default: return -1;
        }
    }
    if (scan_code == 0xE0) {
        extended = 1;
        return -1;
    }

    if (scan_code == 0x2A) {
        shift_state |= 1U;
        return -1;
    }
    if (scan_code == 0x36) {
        shift_state |= 2U;
        return -1;
    }
    if (scan_code == 0xAA) {
        shift_state &= (uint8_t)~1U;
        return -1;
    }
    if (scan_code == 0xB6) {
        shift_state &= (uint8_t)~2U;
        return -1;
    }
    if ((scan_code & 0x80U) != 0) {
        return -1;
    }
    if (scan_code == 0x3A) {
        caps_lock = !caps_lock;
        return -1;
    }
    shifted = shift_state != 0;

    switch (scan_code) {
    case 0x01: return 0x1B;
    case 0x02: return shifted ? '!' : '1';
    case 0x03: return shifted ? '@' : '2';
    case 0x04: return shifted ? '#' : '3';
    case 0x05: return shifted ? '$' : '4';
    case 0x06: return shifted ? '%' : '5';
    case 0x07: return shifted ? '^' : '6';
    case 0x08: return shifted ? '&' : '7';
    case 0x09: return shifted ? '*' : '8';
    case 0x0A: return shifted ? '(' : '9';
    case 0x0B: return shifted ? ')' : '0';
    case 0x0C: return shifted ? '_' : '-';
    case 0x0D: return shifted ? '+' : '=';
    case 0x0E: return '\b';
    case 0x0F: return '\t';
    case 0x10: return shifted ? 'Q' : 'q';
    case 0x11: return shifted ? 'W' : 'w';
    case 0x12: return shifted ? 'E' : 'e';
    case 0x13: return shifted ? 'R' : 'r';
    case 0x14: return shifted ? 'T' : 't';
    case 0x15: return shifted ? 'Y' : 'y';
    case 0x16: return shifted ? 'U' : 'u';
    case 0x17: return shifted ? 'I' : 'i';
    case 0x18: return shifted ? 'O' : 'o';
    case 0x19: return shifted ? 'P' : 'p';
    case 0x1A: return shifted ? '{' : '[';
    case 0x1B: return shifted ? '}' : ']';
    case 0x1C: return '\r';
    case 0x1E: return shifted != caps_lock ? 'A' : 'a';
    case 0x1F: return shifted != caps_lock ? 'S' : 's';
    case 0x20: return shifted != caps_lock ? 'D' : 'd';
    case 0x21: return shifted != caps_lock ? 'F' : 'f';
    case 0x22: return shifted != caps_lock ? 'G' : 'g';
    case 0x23: return shifted != caps_lock ? 'H' : 'h';
    case 0x24: return shifted != caps_lock ? 'J' : 'j';
    case 0x25: return shifted != caps_lock ? 'K' : 'k';
    case 0x26: return shifted != caps_lock ? 'L' : 'l';
    case 0x27: return shifted ? ':' : ';';
    case 0x28: return shifted ? '"' : '\'';
    case 0x29: return shifted ? '~' : '`';
    case 0x2B: return shifted ? '|' : '\\';
    case 0x2C: return shifted != caps_lock ? 'Z' : 'z';
    case 0x2D: return shifted != caps_lock ? 'X' : 'x';
    case 0x2E: return shifted != caps_lock ? 'C' : 'c';
    case 0x2F: return shifted != caps_lock ? 'V' : 'v';
    case 0x30: return shifted != caps_lock ? 'B' : 'b';
    case 0x31: return shifted != caps_lock ? 'N' : 'n';
    case 0x32: return shifted != caps_lock ? 'M' : 'm';
    case 0x33: return shifted ? '<' : ',';
    case 0x34: return shifted ? '>' : '.';
    case 0x35: return shifted ? '?' : '/';
    case 0x39: return ' ';
    default: return -1;
    }
}

int ps2_keyboard_read_char(void) {
    uint8_t status;

    if (!initialized) {
        return -1;
    }
    status = inb(PS2_STATUS_PORT);
    if ((status & 1U) == 0 || (status & 0x20U) != 0) {
        return -1;
    }
    return decode_scan_code(inb(PS2_DATA_PORT));
}
