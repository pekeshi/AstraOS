typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

static volatile unsigned int kernel_boot_count;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x01);
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);
}

static void serial_write(const char *text) {
    while (*text != '\0') {
        while ((inb(0x3FD) & 0x20) == 0) {
        }
        outb(0x3F8, (uint8_t)*text++);
    }
}

__attribute__((noreturn))
void kernel_main(void) {
    kernel_boot_count = 1;
    serial_init();
    serial_write("Hello from the PepeOS C kernel.\r\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
