/* RV32IM/ILP32 runtime: forward Rx I/O and byte allocation to REIMU. */
#include <stdint.h>

extern int printf(const char *format, ...);
extern int scanf(const char *format, ...);
extern void *malloc(uint32_t size);

/* Read one signed 32-bit integer from the program's input. */
int32_t get_i32(void) {
    int32_t value = 0;
    (void)scanf("%d", &value);
    return value;
}

/* Print a signed integer without adding a separator. */
void print_i32(int32_t value) {
    (void)printf("%d", value);
}

/* Print a signed integer followed by exactly one newline. */
void println_i32(int32_t value) {
    (void)printf("%d\n", value);
}

/* REIMU malloc satisfies both supported alignments. Flag invalid requests in tests. */
void *__rx_alloc(uint32_t size, uint32_t align) {
    if (size && (align == 1 || align == 4)) {
        void *pointer = malloc(size);
        if (pointer && !((uintptr_t)pointer & (align - 1)))
            return pointer;
    }
    (void)printf("invalid __rx_alloc request or result\n");
    return 0;
}
