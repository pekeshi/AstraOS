#include "boot_info.h"

/* I/O-porttien käyttöön ei tarvita C-kirjastoa. */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

static volatile unsigned int kernel_boot_count;
#define SHELL_LINE_CAPACITY 80

/* Kirjoita ja lue yhden tavun arvo x86:n I/O-portista. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Lue merkki COM1:stä. Odota, kunnes vastaanottopuskuriin tulee tietoa. */
static char serial_read_char(void) {
    while ((inb(0x3FD) & 0x01) == 0) {
    }
    return (char)inb(0x3F8);
}

static void serial_write_char(char character) {
    while ((inb(0x3FD) & 0x20) == 0) {
    }
    outb(0x3F8, (uint8_t)character);
}

/* Aseta COM1-sarjaportti ytimen diagnostiikkaviestejä varten. */
static void serial_init(void) {
    outb(0x3F9, 0x00);  /* Poista sarjaportin keskeytykset käytöstä. */
    outb(0x3FB, 0x80);  /* Ota baudinopeuden asetus käyttöön. */
    outb(0x3F8, 0x01);  /* Aseta nopeudeksi 115200 bittiä sekunnissa. */
    outb(0x3F9, 0x00);
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
    static const char title[] = "AstraOS Kernel - 0.1.0 - pekeshi";
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
}

/* Lue yksi komentorivi ja näytä kirjoitetut merkit takaisin päätteessä. */
static uint32_t serial_read_line(char *buffer, uint32_t capacity) {
    static int ignore_line_feed;
    uint32_t length = 0;
    int too_long = 0;

    for (;;) {
        char character = serial_read_char();

        if (ignore_line_feed) {
            ignore_line_feed = 0;
            if (character == '\n') {
                continue;
            }
        }

        if (character == '\r' || character == '\n') {
            if (character == '\r') {
                ignore_line_feed = 1;
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
        serial_write("Commands: help, about, clear\r\n");
    } else if (command[0] == 'a' && command[1] == 'b' &&
               command[2] == 'o' && command[3] == 'u' &&
               command[4] == 't' && command[5] == '\0') {
        serial_write("AstraOS 0.1.0 - a small x86_64 UEFI operating system.\r\n");
    } else if (command[0] == 'c' && command[1] == 'l' &&
               command[2] == 'e' && command[3] == 'a' &&
               command[4] == 'r' && command[5] == '\0') {
        serial_write("\x1b[2J\x1b[H");
        serial_write("AstraOS serial shell\r\n");
    } else {
        serial_write("Unknown command. Type 'help' for commands.\r\n");
    }
}

/* Pidä komentokehote käynnissä COM1-sarjaportissa. */
static void serial_shell(void) __attribute__((noreturn));
static void serial_shell(void) {
    char line[SHELL_LINE_CAPACITY];

    serial_write("\r\nAstraOS serial shell\r\n");
    serial_write("Type 'help' for commands.\r\n");

    for (;;) {
        uint32_t length;

        serial_write("AstraOS> ");
        length = serial_read_line(line, sizeof(line));
        if (length == sizeof(line)) {
            serial_write("Command is too long.\r\n");
            continue;
        }
        run_command(line);
    }
}

__attribute__((noreturn))
void kernel_main(struct boot_info *boot_info) {
    /* Pidä nollattava muuttuja mukana ytimen BSS-osiossa. */
    kernel_boot_count = 1;
    serial_init();
    serial_write("Hello from the AstraOS C kernel.\r\n");
    draw_kernel_message(boot_info);

    serial_shell();
}
