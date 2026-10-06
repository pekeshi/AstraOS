#include "boot_info.h"
#include "acpi.h"
#include "pmm.h"
#include "ps2.h"
#include "xhci.h"

/* I/O-porttien käyttöön ei tarvita C-kirjastoa. */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

#define EFI_PAGE_SIZE 4096
#define CONSOLE_LEFT 12
#define CONSOLE_TOP 52
#define CONSOLE_RIGHT 12
#define CONSOLE_BOTTOM 8
#define CONSOLE_CELL_WIDTH 12
#define CONSOLE_LINE_HEIGHT 18
#define CONSOLE_SCALE 2

static volatile unsigned int kernel_boot_count;
static int physical_allocator_ready;
static int ps2_keyboard_ready;
static const struct boot_info *kernel_boot_info;
static const struct boot_info *console_boot_info;
static uint32_t console_cursor_x;
static uint32_t console_cursor_y;
static int console_enabled;
#define SHELL_LINE_CAPACITY 80

static void console_write_char(char character);
static int initialize_usb_keyboard(void);

/* Kirjoita ja lue yhden tavun arvo x86:n I/O-portista. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_write_char(char character) {
    while ((inb(0x3FD) & 0x20) == 0) {
    }
    outb(0x3F8, (uint8_t)character);
    console_write_char(character);
}

static void serial_write_raw(const char *text) {
    while (*text != '\0') {
        while ((inb(0x3FD) & 0x20) == 0) {
        }
        outb(0x3F8, (uint8_t)*text++);
    }
}

static void serial_flush(void) {
    while ((inb(0x3FD) & 0x40) == 0) {
    }
}

/* Aseta COM1-sarjaportti ytimen diagnostiikkaviestejä varten. */
static void serial_init(void) {
    outb(0x3F9, 0x00);  /* Poista sarjaportin keskeytykset käytöstä. */
    outb(0x3FB, 0x80);  /* Ota baudinopeuden asetus käyttöön. */
    outb(0x3F8, 0x01);  /* Aseta nopeudeksi 115200 bittiä sekunnissa. */
    outb(0x3F9, 0x00);  /* Poista sarjaportin keskeytykset käytöstä. */
    outb(0x3FB, 0x03);  /* 8 databittiä, ei pariteettia, yksi stop-bitti. */
    outb(0x3FA, 0xC7);  /* Tyhjennä puskurit ja ota FIFO käyttöön. */
    outb(0x3FC, 0x0B);  /* Ota lähetys ja vastaanotto käyttöön. */
}

/* Odota, että sarjaportti on valmis, ja lähetä teksti merkki kerrallaan. */
static void serial_write(const char *text) {
    while (*text != '\0') {
        serial_write_char(*text++);
    }
}

static void serial_write_u64(uint64_t value) {
    char digits[20];
    uint32_t length = 0;

    do {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (length > 0) {
        serial_write_char(digits[--length]);
    }
}

static void serial_write_hex_u64(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    int shift = 60;

    serial_write("0x");
    while (shift > 0 && ((value >> shift) & 0xF) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        serial_write_char(digits[(value >> shift) & 0xF]);
    }
}

static int parse_hex_u64(const char *text, uint64_t *value) {
    uint64_t result = 0;
    uint32_t digits = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    while (*text != '\0') {
        uint32_t digit;

        if (*text >= '0' && *text <= '9') {
            digit = (uint32_t)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = (uint32_t)(*text - 'a' + 10);
        } else if (*text >= 'A' && *text <= 'F') {
            digit = (uint32_t)(*text - 'A' + 10);
        } else {
            return 0;
        }
        if (result > (UINT64_MAX - digit) / 16) {
            return 0;
        }
        result = result * 16 + digit;
        ++digits;
        ++text;
    }
    if (digits == 0) {
        return 0;
    }
    *value = result;
    return 1;
}

static void print_memory_status(void) {
    uint64_t free_pages;

    if (!physical_allocator_ready) {
        serial_write("Error: physical page allocator is unavailable.\r\n");
        return;
    }
    free_pages = pmm_free_pages();
    serial_write("Free physical memory: ");
    serial_write_u64(free_pages / (1024 * 1024 / EFI_PAGE_SIZE));
    serial_write(" MiB (");
    serial_write_u64(free_pages);
    serial_write(" pages).\r\n");
}

/* Merkit ovat 5 x 7 -pikselin bittikarttoja. */
static const uint8_t font[26][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 18, 18, 12},
    {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
    {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
};

/* Numerot ja otsikossa käytetyt välimerkit. */
static const uint8_t digit_font[10][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
};

static const uint8_t hyphen_glyph[7] = {0, 0, 0, 31, 0, 0, 0};
static const uint8_t dot_glyph[7] = {0, 0, 0, 0, 0, 12, 12};
static const uint8_t colon_glyph[7] = {0, 12, 12, 0, 12, 12, 0};
static const uint8_t left_paren_glyph[7] = {2, 4, 8, 8, 8, 4, 2};
static const uint8_t right_paren_glyph[7] = {8, 4, 2, 2, 2, 4, 8};
static const uint8_t less_glyph[7] = {2, 4, 8, 16, 8, 4, 2};
static const uint8_t greater_glyph[7] = {8, 4, 2, 1, 2, 4, 8};
static const uint8_t underscore_glyph[7] = {0, 0, 0, 0, 0, 0, 31};

/* Kirjoita yksi pikseli GOP:n valitsemaa värijärjestystä käyttäen. */
static void framebuffer_pixel(const struct boot_info *boot_info, uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue) {
    volatile uint32_t *framebuffer = (volatile uint32_t *)(uintptr_t)boot_info->framebuffer_base;
    uint32_t color = boot_info->framebuffer_format == 0
        ? (uint32_t)red | ((uint32_t)green << 8) | ((uint32_t)blue << 16)
        : (uint32_t)blue | ((uint32_t)green << 8) | ((uint32_t)red << 16);
    framebuffer[(uint64_t)y * boot_info->pixels_per_scanline + x] = color;
}

/* Palauta merkin bittikartta; isot ja pienet kirjaimet käyttävät samaa kuvaa. */
static const uint8_t *get_glyph(char character) {
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    if (character >= 'A' && character <= 'Z') {
        return font[(uint32_t)(character - 'A')];
    }
    if (character >= '0' && character <= '9') {
        return digit_font[(uint32_t)(character - '0')];
    }
    if (character == '-') {
        return hyphen_glyph;
    }
    if (character == '.') {
        return dot_glyph;
    }
    if (character == ':') {
        return colon_glyph;
    }
    if (character == '(') {
        return left_paren_glyph;
    }
    if (character == ')') {
        return right_paren_glyph;
    }
    if (character == '<') {
        return less_glyph;
    }
    if (character == '>') {
        return greater_glyph;
    }
    if (character == '_') {
        return underscore_glyph;
    }
    return 0;
}

/* Piirrä merkin päällä olevat pikselit halutulla suurennuksella. */
static void draw_character(const struct boot_info *boot_info, char character,
                           uint32_t x, uint32_t y, uint32_t scale) {
    const uint8_t *glyph = get_glyph(character);
    if (glyph == 0) {
        return;
    }
    for (uint32_t row = 0; row < 7; ++row) {
        for (uint32_t column = 0; column < 5; ++column) {
            if ((glyph[row] & (1U << (4 - column))) == 0) {
                continue;
            }
            for (uint32_t dy = 0; dy < scale; ++dy) {
                for (uint32_t dx = 0; dx < scale; ++dx) {
                    framebuffer_pixel(boot_info, x + column * scale + dx,
                                      y + row * scale + dy, 96, 210, 255);
                }
            }
        }
    }
}

/* Tarkista kuvapuskurin tiedot ja piirrä tausta sekä keskitetty otsikkopalkki. */
static void draw_kernel_message(const struct boot_info *boot_info) {
    static const char title[] = "AstraOS Kernel - 0.2.0 - pekeshi";
    const uint32_t top_bar_height = 40;
    uint32_t scale = 2;
    uint32_t text_width = (uint32_t)(sizeof(title) - 1) * 6 * scale;
    uint32_t x_start;
    uint32_t y_start;
    uint64_t pixel_count;

    if (boot_info == 0 || boot_info->framebuffer_base == 0 ||
        (boot_info->framebuffer_format != 0 && boot_info->framebuffer_format != 1) ||
        boot_info->framebuffer_width == 0 || boot_info->framebuffer_height < 7 ||
        boot_info->pixels_per_scanline < boot_info->framebuffer_width ||
        boot_info->pixels_per_scanline >
            UINT64_MAX / boot_info->framebuffer_height) {
        return;
    }
    pixel_count = (uint64_t)boot_info->pixels_per_scanline * boot_info->framebuffer_height;
    if (pixel_count > boot_info->framebuffer_size / sizeof(uint32_t)) {
        return;
    }

    if (text_width > boot_info->framebuffer_width ||
        7 * scale > top_bar_height) {
        scale = 1;
        text_width = (uint32_t)(sizeof(title) - 1) * 6;
    }
    if (text_width > boot_info->framebuffer_width ||
        top_bar_height > boot_info->framebuffer_height ||
        7 * scale > top_bar_height) {
        return;
    }
    x_start = (boot_info->framebuffer_width - text_width) / 2;
    y_start = (top_bar_height - 7 * scale) / 2;

    for (uint32_t y = 0; y < boot_info->framebuffer_height; ++y) {
        for (uint32_t x = 0; x < boot_info->framebuffer_width; ++x) {
            framebuffer_pixel(boot_info, x, y, 12, 18, 32);
        }
    }

    for (uint32_t y = 0; y < top_bar_height - 2; ++y) {
        for (uint32_t x = 0; x < boot_info->framebuffer_width; ++x) {
            framebuffer_pixel(boot_info, x, y, 32, 48, 76);
        }
    }
    for (uint32_t x = 0; x < boot_info->framebuffer_width; ++x) {
        framebuffer_pixel(boot_info, x, top_bar_height - 2, 72, 160, 220);
        framebuffer_pixel(boot_info, x, top_bar_height - 1, 72, 160, 220);
    }

    for (uint32_t i = 0; title[i] != '\0'; ++i) {
        if (title[i] != ' ') {
            draw_character(boot_info, title[i], x_start + i * 6 * scale,
                           y_start, scale);
        }
    }
    console_boot_info = boot_info;
    console_cursor_x = CONSOLE_LEFT;
    console_cursor_y = CONSOLE_TOP;
    console_enabled =
        boot_info->framebuffer_width >=
            CONSOLE_LEFT + CONSOLE_RIGHT + CONSOLE_CELL_WIDTH &&
        boot_info->framebuffer_height >
            CONSOLE_TOP + CONSOLE_LINE_HEIGHT + CONSOLE_BOTTOM;
}

static void console_clear_cell(uint32_t x, uint32_t y) {
    for (uint32_t row = 0; row < CONSOLE_LINE_HEIGHT; ++row) {
        for (uint32_t column = 0; column < CONSOLE_CELL_WIDTH; ++column) {
            framebuffer_pixel(console_boot_info, x + column, y + row,
                              12, 18, 32);
        }
    }
}

static void console_scroll(void) {
    uint32_t height = console_boot_info->framebuffer_height;
    uint32_t scroll_end = height > CONSOLE_BOTTOM
        ? height - CONSOLE_BOTTOM
        : 0;

    if (scroll_end <= CONSOLE_TOP + CONSOLE_LINE_HEIGHT) {
        console_cursor_y = CONSOLE_TOP;
        return;
    }
    for (uint32_t y = CONSOLE_TOP; y + CONSOLE_LINE_HEIGHT < scroll_end; ++y) {
        for (uint32_t x = CONSOLE_LEFT;
             x < console_boot_info->framebuffer_width - CONSOLE_RIGHT; ++x) {
            uint64_t destination =
                (uint64_t)y * console_boot_info->pixels_per_scanline + x;
            uint64_t source =
                (uint64_t)(y + CONSOLE_LINE_HEIGHT) *
                    console_boot_info->pixels_per_scanline + x;
            volatile uint32_t *framebuffer =
                (volatile uint32_t *)(uintptr_t)console_boot_info->framebuffer_base;
            framebuffer[destination] = framebuffer[source];
        }
    }
    for (uint32_t y = scroll_end - CONSOLE_LINE_HEIGHT; y < scroll_end; ++y) {
        for (uint32_t x = CONSOLE_LEFT;
             x < console_boot_info->framebuffer_width - CONSOLE_RIGHT; ++x) {
            framebuffer_pixel(console_boot_info, x, y, 12, 18, 32);
        }
    }
    console_cursor_y = scroll_end - CONSOLE_LINE_HEIGHT;
}

static void console_new_line(void) {
    console_cursor_x = CONSOLE_LEFT;
    console_cursor_y += CONSOLE_LINE_HEIGHT;
    if (console_cursor_y + CONSOLE_LINE_HEIGHT >
        console_boot_info->framebuffer_height - CONSOLE_BOTTOM) {
        console_scroll();
    }
}

static void console_clear(void) {
    uint32_t width = console_boot_info->framebuffer_width;
    uint32_t height = console_boot_info->framebuffer_height;

    for (uint32_t y = CONSOLE_TOP; y < height; ++y) {
        for (uint32_t x = CONSOLE_LEFT; x < width; ++x) {
            framebuffer_pixel(console_boot_info, x, y, 12, 18, 32);
        }
    }
    console_cursor_x = CONSOLE_LEFT;
    console_cursor_y = CONSOLE_TOP;
}

static void console_write_char(char character) {
    uint32_t right_edge;

    if (!console_enabled) {
        return;
    }
    right_edge = console_boot_info->framebuffer_width > CONSOLE_RIGHT
        ? console_boot_info->framebuffer_width - CONSOLE_RIGHT
        : 0;

    if (character == '\r') {
        console_cursor_x = CONSOLE_LEFT;
    } else if (character == '\n') {
        console_new_line();
    } else if (character == '\b') {
        if (console_cursor_x > CONSOLE_LEFT) {
            console_cursor_x -= CONSOLE_CELL_WIDTH;
        } else if (console_cursor_y > CONSOLE_TOP) {
            console_cursor_y -= CONSOLE_LINE_HEIGHT;
            console_cursor_x = right_edge >= CONSOLE_LEFT + CONSOLE_CELL_WIDTH
                ? right_edge - CONSOLE_CELL_WIDTH
                : CONSOLE_LEFT;
        }
        console_clear_cell(console_cursor_x, console_cursor_y);
    } else if (character >= ' ' && character <= '~') {
        if (console_cursor_x + CONSOLE_CELL_WIDTH > right_edge) {
            console_new_line();
        }
        if (character != ' ') {
            draw_character(console_boot_info, character, console_cursor_x,
                           console_cursor_y, CONSOLE_SCALE);
        }
        console_cursor_x += CONSOLE_CELL_WIDTH;
    }
}

static int keyboard_read_char(void) {
    int ps2_character = ps2_keyboard_read_char();

    if (ps2_character >= 0) {
        return ps2_character;
    }
    return xhci_read_char();
}

static int read_input_char(int *from_serial) {
    for (;;) {
        int character = keyboard_read_char();

        if (character >= 0) {
            *from_serial = 0;
            return character;
        }
        if ((inb(0x3FD) & 0x01) != 0) {
            *from_serial = 1;
            return (unsigned char)inb(0x3F8);
        }
    }
}

/* Lue rivi näppäimistöltä tai COM1:stä ja näytä näppäillyt merkit. */
static uint32_t shell_read_line(char *buffer, uint32_t capacity) {
    static int ignore_serial_line_feed;
    uint32_t length = 0;
    int too_long = 0;

    for (;;) {
        int from_serial;
        char character = (char)read_input_char(&from_serial);

        if (from_serial && ignore_serial_line_feed) {
            ignore_serial_line_feed = 0;
            if (character == '\n') {
                continue;
            }
        }

        if (character == '\r' || character == '\n') {
            if (from_serial && character == '\r') {
                ignore_serial_line_feed = 1;
            }
            serial_write("\r\n");
            buffer[length] = '\0';
            return too_long ? capacity : length;
        }

        if (character == '\b' || character == 0x7F) {
            if (length > 0 && !too_long) {
                --length;
                buffer[length] = '\0';
                serial_write("\b \b");
            }
            continue;
        }

        if (character >= ' ' && character <= '~') {
            if (length + 1 < capacity) {
                buffer[length++] = character;
                serial_write_char(character);
            } else if (!too_long) {
                too_long = 1;
                serial_write("\a");
            }
        }
    }
}

/* Suorita yhden rivin mittainen komento. */
static void run_command(const char *command) {
    if (command[0] == '\0') {
        return;
    }
    if (command[0] == 'h' && command[1] == 'e' && command[2] == 'l' &&
        command[3] == 'p' && command[4] == '\0') {
        serial_write("Commands: help, about, clear, mem, alloc, free <hex-address>, shutdown\r\n");
    } else if (command[0] == 'a' && command[1] == 'b' &&
               command[2] == 'o' && command[3] == 'u' &&
               command[4] == 't' && command[5] == '\0') {
        serial_write("AstraOS 0.1.0 - a small x86_64 UEFI operating system.\r\n");
    } else if (command[0] == 's' && command[1] == 'h' &&
               command[2] == 'u' && command[3] == 't' &&
               command[4] == 'd' && command[5] == 'o' &&
               command[6] == 'w' && command[7] == 'n' &&
               command[8] == '\0') {
        enum acpi_shutdown_result result;

        serial_write("Requesting ACPI soft-off...\r\n");
        serial_flush();
        result = acpi_request_shutdown(
            kernel_boot_info != 0 ? kernel_boot_info->acpi_root_pointer : 0);
        if (result == ACPI_SHUTDOWN_NO_TABLES) {
            serial_write("Error: firmware did not provide ACPI tables.\r\n");
        } else if (result == ACPI_SHUTDOWN_INVALID_TABLES) {
            serial_write("Error: ACPI tables are invalid or unsupported.\r\n");
        } else if (result == ACPI_SHUTDOWN_NO_S5) {
            serial_write("Error: firmware does not describe ACPI S5 soft-off.\r\n");
        } else if (result == ACPI_SHUTDOWN_UNSUPPORTED) {
            serial_write("Error: ACPI soft-off is unavailable on this hardware.\r\n");
        } else {
            serial_write("Error: hardware did not power off after the ACPI request.\r\n");
        }
    } else if (command[0] == 'm' && command[1] == 'e' &&
               command[2] == 'm' && command[3] == '\0') {
        print_memory_status();
    } else if (command[0] == 'a' && command[1] == 'l' &&
               command[2] == 'l' && command[3] == 'o' &&
               command[4] == 'c' && command[5] == '\0') {
        uint64_t address;

        if (!physical_allocator_ready) {
            serial_write("Error: physical page allocator is unavailable.\r\n");
        } else if ((address = pmm_alloc_page()) == 0) {
            serial_write("Error: no free physical pages remain.\r\n");
        } else {
            serial_write("Allocated physical page at ");
            serial_write_hex_u64(address);
            serial_write(".\r\n");
        }
    } else if (command[0] == 'f' && command[1] == 'r' &&
               command[2] == 'e' && command[3] == 'e' &&
               command[4] == '\0') {
        serial_write("Usage: free <hex-address>\r\n");
    } else if (command[0] == 'f' && command[1] == 'r' &&
               command[2] == 'e' && command[3] == 'e' &&
               command[4] == ' ') {
        uint64_t address;

        if (!physical_allocator_ready) {
            serial_write("Error: physical page allocator is unavailable.\r\n");
        } else if (!parse_hex_u64(command + 5, &address)) {
            serial_write("Usage: free <hex-address>\r\n");
        } else if (!pmm_free_page(address)) {
            serial_write("Error: address is not an allocated page.\r\n");
        } else {
            serial_write("Freed physical page at ");
            serial_write_hex_u64(address);
            serial_write(".\r\n");
        }
    } else if (command[0] == 'c' && command[1] == 'l' &&
               command[2] == 'e' && command[3] == 'a' &&
               command[4] == 'r' && command[5] == '\0') {
        console_clear();
        serial_write_raw("\x1b[2J\x1b[H");
        serial_write("AstraOS kernel shell\r\n");
    } else {
        serial_write("Unknown command. Type 'help' for commands.\r\n");
    }
}

/* Pidä komentokehote käynnissä COM1-sarjaportissa. */
static void serial_shell(void) __attribute__((noreturn));
static void serial_shell(void) {
    char line[SHELL_LINE_CAPACITY];

    serial_write("\r\nAstraOS kernel shell\r\n");
    serial_write("Type commands here.\r\n");

    for (;;) {
        uint32_t length;

        serial_write("AstraOS> ");
        length = shell_read_line(line, sizeof(line));
        if (length == sizeof(line)) {
            serial_write("Command is too long.\r\n");
            continue;
        }
        run_command(line);
    }
}

static const char *xhci_stage_name(uint32_t stage) {
    switch (stage) {
    case 1: return "enable slot";
    case 2: return "allocate device buffers";
    case 3: return "address device";
    case 4: return "read device descriptor header";
    case 5: return "read complete device descriptor";
    case 6: return "read configuration descriptor";
    case 7: return "validate configuration descriptor";
    case 8: return "find boot-keyboard interface";
    case 9: return "find interrupt-IN endpoint";
    case 10: return "set configuration and boot protocol";
    case 11: return "allocate interrupt ring";
    case 12: return "validate endpoint interval";
    case 13: return "configure interrupt endpoint";
    default: return "scan or port reset";
    }
}

static const char *xhci_controller_stage_name(uint32_t stage) {
    switch (stage) {
    case XHCI_CONTROLLER_STAGE_VALIDATE_BAR: return "validate PCI BAR";
    case XHCI_CONTROLLER_STAGE_ENABLE_PCI: return "enable PCI memory and bus mastering";
    case XHCI_CONTROLLER_STAGE_VALIDATE_CAPABILITIES: return "read controller capabilities";
    case XHCI_CONTROLLER_STAGE_FIRMWARE_HANDOFF: return "firmware ownership handoff";
    case XHCI_CONTROLLER_STAGE_RESET: return "stop/reset controller";
    case XHCI_CONTROLLER_STAGE_ALLOCATE_RINGS: return "allocate controller rings";
    case XHCI_CONTROLLER_STAGE_ALLOCATE_SCRATCHPADS: return "allocate scratchpads";
    case XHCI_CONTROLLER_STAGE_START: return "start controller";
    case XHCI_CONTROLLER_STAGE_RUNNING: return "controller running";
    default: return "not reached";
    }
}

static const char *xhci_port_stage_name(uint32_t stage) {
    switch (stage) {
    case XHCI_PORT_STAGE_NO_CONNECTION: return "no connected root port";
    case XHCI_PORT_STAGE_DEBOUNCE: return "debouncing connected port";
    case XHCI_PORT_STAGE_CONNECTION_LOST: return "connection lost during debounce";
    case XHCI_PORT_STAGE_RESET: return "waiting for port reset";
    case XHCI_PORT_STAGE_RESET_TIMEOUT: return "port reset timed out";
    case XHCI_PORT_STAGE_NOT_ENABLED: return "reset ended without enabled port";
    case XHCI_PORT_STAGE_READY: return "port reset and enabled";
    case XHCI_PORT_STAGE_POWER_TIMEOUT: return "port power stabilization timed out";
    default: return "not reached";
    }
}

static void print_xhci_port_diagnostics(
    const struct xhci_diagnostics *diagnostics) {
    serial_write("Controller ");
    serial_write_hex_u64(diagnostics->pci_location);
    serial_write(" root-port scan:\r\n");
    for (uint32_t i = 0; i < diagnostics->port_count; ++i) {
        serial_write("  Port ");
        serial_write_u64(i + 1);
        serial_write(": PORTSC=");
        serial_write_hex_u64(diagnostics->port_status[i]);
        serial_write(", ");
        serial_write(xhci_port_stage_name(diagnostics->port_stages[i]));
        if (diagnostics->port_enumeration_stages[i] != 0) {
            serial_write(", enumeration stage=");
            serial_write_u64(diagnostics->port_enumeration_stages[i]);
            serial_write(" (");
            serial_write(xhci_stage_name(
                diagnostics->port_enumeration_stages[i]));
            serial_write("), command=");
            serial_write_u64(diagnostics->port_command_types[i]);
            serial_write(", completion=");
            serial_write_u64(diagnostics->port_completion_codes[i]);
        }
        serial_write(".\r\n");
    }
}

static int initialize_usb_keyboard(void) {
    enum xhci_init_status usb_status = xhci_init();

    if (usb_status == XHCI_INIT_READY) {
        serial_write("USB keyboard ready (xHCI).\r\n");
        return 1;
    }
    if (usb_status == XHCI_INIT_FAILED ||
        usb_status == XHCI_INIT_NO_KEYBOARD) {
        struct xhci_diagnostics diagnostics;

        if (usb_status == XHCI_INIT_FAILED) {
            serial_write("Error: xHCI initialization failed.\r\n");
        } else {
            serial_write("USB keyboard not found on xHCI root ports.\r\n");
        }
        xhci_get_diagnostics(&diagnostics);
        serial_write("xHCI PCI location: ");
        serial_write_hex_u64(diagnostics.pci_location);
        serial_write(", controller stage: ");
        serial_write_u64(diagnostics.controller_stage);
        serial_write(" (");
        serial_write(xhci_controller_stage_name(diagnostics.controller_stage));
        serial_write(")");
        serial_write(", controllers scanned: ");
        serial_write_u64(diagnostics.controllers_scanned);
        serial_write(", controllers with connected ports: ");
        serial_write_u64(diagnostics.controllers_with_connected_ports);
        serial_write(", ports: ");
        serial_write_u64(diagnostics.port_count);
        serial_write(", connected: ");
        serial_write_u64(diagnostics.connected_ports);
        serial_write(", reset: ");
        serial_write_u64(diagnostics.reset_ports);
        serial_write("\r\nLast port: ");
        serial_write_u64(diagnostics.last_port);
        serial_write(", PORTSC: ");
        serial_write_hex_u64(diagnostics.last_port_status);
        serial_write(", port stage: ");
        serial_write_u64(diagnostics.port_stage);
        serial_write(" (");
        serial_write(xhci_port_stage_name(diagnostics.port_stage));
        serial_write(")");
        serial_write(", speed: ");
        serial_write_u64(diagnostics.last_speed);
        serial_write(", enumeration stage: ");
        serial_write_u64(diagnostics.enumeration_stage);
        serial_write(" (");
        serial_write(xhci_stage_name(diagnostics.enumeration_stage));
        serial_write("), command: ");
        serial_write_u64(diagnostics.last_command_type);
        serial_write(", completion code: ");
        serial_write_u64(diagnostics.last_completion_code);
        serial_write(".\r\n");
        /* Print raw capability/register values to aid debugging BAR/MMIO issues. */
        serial_write("CAPLENGTH: ");
        serial_write_u64(diagnostics.cap_length);
        serial_write(", HCC_PARAMS: ");
        serial_write_hex_u64(diagnostics.hcc_params);
        serial_write(", HCS_PARAMS1: ");
        serial_write_hex_u64(diagnostics.hcs_params1);
        serial_write("\r\nMMIO BAR: ");
        serial_write_hex_u64(diagnostics.mmio_bar);
        serial_write(", doorbell offset: ");
        serial_write_hex_u64(diagnostics.doorbell_offset);
        serial_write(", runtime offset: ");
        serial_write_hex_u64(diagnostics.runtime_offset);
        serial_write("\r\n");
        {
            uint32_t controller_count =
                xhci_get_controller_diagnostic_count();

            for (uint32_t i = 0; i < controller_count; ++i) {
                struct xhci_diagnostics controller_diagnostics;

                if (xhci_get_controller_diagnostics(
                        i, &controller_diagnostics)) {
                    print_xhci_port_diagnostics(&controller_diagnostics);
                }
            }
            if (diagnostics.controllers_scanned > controller_count) {
                serial_write("Per-port diagnostics retained for ");
                serial_write_u64(controller_count);
                serial_write(" controllers; total scanned: ");
                serial_write_u64(diagnostics.controllers_scanned);
                serial_write(".\r\n");
            }
        }
        return 0;
    }
    serial_write("Error: no xHCI controller was found.\r\n");
    return 0;
}

__attribute__((noreturn))
void kernel_main(struct boot_info *boot_info) {
    /* Pidä nollattava muuttuja mukana ytimen BSS-osiossa. */
    kernel_boot_count = 1;
    kernel_boot_info = boot_info;
    serial_init();
    draw_kernel_message(boot_info);
    serial_write("Hello from the AstraOS C kernel.\r\n");
    ps2_keyboard_ready = ps2_keyboard_init();
    physical_allocator_ready = pmm_init(boot_info);
    if (!physical_allocator_ready) {
        serial_write("Error: could not initialize the physical page allocator.\r\n");
        if (ps2_keyboard_ready) {
            serial_write("Keyboard ready (PS/2-compatible, firmware route preserved).\r\n");
        }
    } else if (ps2_keyboard_ready) {
        serial_write("Keyboard ready (PS/2-compatible, firmware route preserved).\r\n");
    }
    if (physical_allocator_ready) {
        (void)initialize_usb_keyboard();
    }

    serial_shell();
}
