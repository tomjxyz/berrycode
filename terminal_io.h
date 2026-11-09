/* Route UI output through the screen/scrollback manager. Include AFTER the
 * system headers, only in translation units which write the interactive UI.
 * Non-stdout streams and non-interactive output retain stdio semantics. */
#ifndef BC_TERMINAL_IO_H
#define BC_TERMINAL_IO_H
int term_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int term_fputs(const char *s, FILE *stream);
int term_fputc(int c, FILE *stream);
size_t term_fwrite(const void *p, size_t size, size_t n, FILE *stream);
#define printf(...) term_printf(__VA_ARGS__)
#define fputs(...) term_fputs(__VA_ARGS__)
#define fputc(...) term_fputc(__VA_ARGS__)
#define fwrite(...) term_fwrite(__VA_ARGS__)
#endif
