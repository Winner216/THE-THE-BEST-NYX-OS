// include/io.h
#ifndef IO_H
#define IO_H

#include <stdint.h>

// Write a byte to a hardware port
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %1, %0" : : "dN"(port), "a"(value));
}

// Read a byte from a hardware port
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 32-bit port IO — needed for PCI configuration space
static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %1, %0" : : "dN"(port), "a"(value));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

// 16-bit port IO — needed for ATA PIO sector data transfers
static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %1, %0" : : "dN"(port), "a"(value));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

#endif
