#define	assert(e) do {							\
	if (config_debug && !(e)) {					\
		malloc_write("<jemalloc>: Failed assertion\n");		\
		abort();						\
	}								\
} while (0)

#define	not_reached() do {						\
	if (config_debug) {						\
		malloc_write("<jemalloc>: Unreachable code reached\n");	\
		abort();						\
	}								\
} while (0)

#define	not_implemented() do {						\
	if (config_debug) {						\
		malloc_write("<jemalloc>: Not implemented\n");		\
		abort();						\
	}								\
} while (0)

#define	JEMALLOC_UTIL_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	wrtmessage(void *cbopaque, const char *s);
#define	U2S_BUFSIZE	((1U << (LG_SIZEOF_INTMAX_T + 3)) + 1)
static char	*u2s(uintmax_t x, unsigned base, bool uppercase, char *s,
    size_t *slen_p);
#define	D2S_BUFSIZE	(1 + U2S_BUFSIZE)
static char	*d2s(intmax_t x, char sign, char *s, size_t *slen_p);
#define	O2S_BUFSIZE	(1 + U2S_BUFSIZE)
static char	*o2s(uintmax_t x, bool alt_form, char *s, size_t *slen_p);
#define	X2S_BUFSIZE	(2 + U2S_BUFSIZE)
static char	*x2s(uintmax_t x, bool alt_form, bool uppercase, char *s,
    size_t *slen_p);

/******************************************************************************/

/* malloc_message() setup. */
JEMALLOC_CATTR(visibility("hidden"), static)
void
wrtmessage(void *cbopaque, const char *s)
{
	UNUSED int result = write(STDERR_FILENO, s, strlen(s));
}

void	(*je_malloc_message)(void *, const char *s)
    JEMALLOC_ATTR(visibility("default")) = wrtmessage;

/*
 * glibc provides a non-standard strerror_r() when _GNU_SOURCE is defined, so
 * provide a wrapper.
 */
int
buferror(int errnum, char *buf, size_t buflen)
{
#ifdef _GNU_SOURCE
	char *b = strerror_r(errno, buf, buflen);
	if (b != buf) {
		strncpy(buf, b, buflen);
		buf[buflen-1] = '\0';
	}
	return (0);
#else
	return (strerror_r(errno, buf, buflen));
#endif
}

static char *
u2s(uintmax_t x, unsigned base, bool uppercase, char *s, size_t *slen_p)
{
	unsigned i;

	i = U2S_BUFSIZE - 1;
	s[i] = '\0';
	switch (base) {
	case 10:
		do {
			i--;
			s[i] = "0123456789"[x % (uint64_t)10];
			x /= (uint64_t)10;
		} while (x > 0);
		break;
	case 16: {
		const char *digits = (uppercase)
		    ? "0123456789ABCDEF"
		    : "0123456789abcdef";

		do {
			i--;
			s[i] = digits[x & 0xf];
			x >>= 4;
		} while (x > 0);
		break;
	} default: {
		const char *digits = (uppercase)
		    ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    : "0123456789abcdefghijklmnopqrstuvwxyz";

		assert(base >= 2 && base <= 36);
		do {
			i--;
			s[i] = digits[x % (uint64_t)base];
			x /= (uint64_t)base;
		} while (x > 0);
	}}

	*slen_p = U2S_BUFSIZE - 1 - i;
	return (&s[i]);
}

static char *
d2s(intmax_t x, char sign, char *s, size_t *slen_p)
{
	bool neg;

	if ((neg = (x < 0)))
		x = -x;
	s = u2s(x, 10, false, s, slen_p);
	if (neg)
		sign = '-';
	switch (sign) {
	case '-':
		if (neg == false)
			break;
		/* Fall through. */
	case ' ':
	case '+':
		s--;
		(*slen_p)++;
		*s = sign;
		break;
	default: not_reached();
	}
	return (s);
}

static char *
o2s(uintmax_t x, bool alt_form, char *s, size_t *slen_p)
{

	s = u2s(x, 8, false, s, slen_p);
	if (alt_form && *s != '0') {
		s--;
		(*slen_p)++;
		*s = '0';
	}
	return (s);
}

static char *
x2s(uintmax_t x, bool alt_form, bool uppercase, char *s, size_t *slen_p)
{

	s = u2s(x, 16, uppercase, s, slen_p);
	if (alt_form) {
		s -= 2;
		(*slen_p) += 2;
		memcpy(s, uppercase ? "0X" : "0x", 2);
	}
	return (s);
}

int
malloc_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	int ret;
	size_t i;
	const char *f;
	va_list tap;

#define	APPEND_C(c) do {						\
	if (i < size)							\
		str[i] = (c);						\
	i++;								\
} while (0)
#define	APPEND_S(s, slen) do {						\
	if (i < size) {							\
		size_t cpylen = (slen <= size - i) ? slen : size - i;	\
		memcpy(&str[i], s, cpylen);				\
	}								\
	i += slen;							\
} while (0)
#define	APPEND_PADDED_S(s, slen, width, left_justify) do {		\
	/* Left padding. */						\
	size_t pad_len = (width == -1) ? 0 : ((slen < (size_t)width) ?	\
	    (size_t)width - slen : 0);					\
	if (left_justify == false && pad_len != 0) {			\
		size_t j;						\
		for (j = 0; j < pad_len; j++)				\
			APPEND_C(' ');					\
	}								\
	/* Value. */							\
	APPEND_S(s, slen);						\
	/* Right padding. */						\
	if (left_justify && pad_len != 0) {				\
		size_t j;						\
		for (j = 0; j < pad_len; j++)				\
			APPEND_C(' ');					\
	}								\
} while (0)
#define GET_ARG_NUMERIC(val, len) do {					\
	switch (len) {							\
	case '?':							\
		val = va_arg(ap, int);					\
		break;							\
	case 'l':							\
		val = va_arg(ap, long);					\
		break;							\
	case 'q':							\
		val = va_arg(ap, long long);				\
		break;							\
	case 'j':							\
		val = va_arg(ap, intmax_t);				\
		break;							\
	case 't':							\
		val = va_arg(ap, ptrdiff_t);				\
		break;							\
	case 'z':							\
		val = va_arg(ap, size_t);				\
		break;							\
	case 'p': /* Synthetic; used for %p. */				\
		val = va_arg(ap, uintptr_t);				\
		break;							\
	default: not_reached();						\
	}								\
} while (0)

	if (config_debug)
		va_copy(tap, ap);

	i = 0;
	f = format;
	while (true) {
		switch (*f) {
		case '\0': goto OUT;
		case '%': {
			bool alt_form = false;
			bool zero_pad = false;
			bool left_justify = false;
			bool plus_space = false;
			bool plus_plus = false;
			int prec = -1;
			int width = -1;
			char len = '?';

			f++;
			if (*f == '%') {
				/* %% */
				APPEND_C(*f);
				break;
			}
			/* Flags. */
			while (true) {
				switch (*f) {
				case '#':
					assert(alt_form == false);
					alt_form = true;
					break;
				case '0':
					assert(zero_pad == false);
					zero_pad = true;
					break;
				case '-':
					assert(left_justify == false);
					left_justify = true;
					break;
				case ' ':
					assert(plus_space == false);
					plus_space = true;
					break;
				case '+':
					assert(plus_plus == false);
					plus_plus = true;
					break;
				default: goto WIDTH;
				}
				f++;
			}
			/* Width. */
			WIDTH:
			switch (*f) {
			case '*':
				width = va_arg(ap, int);
				f++;
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9': {
				unsigned long uwidth;
				errno = 0;
				uwidth = strtoul(f, (char **)&f, 10);
				assert(uwidth != ULONG_MAX || errno != ERANGE);
				width = (int)uwidth;
				if (*f == '.') {
					f++;
					goto PRECISION;
				} else
					goto LENGTH;
				break;
			} case '.':
				f++;
				goto PRECISION;
			default: goto LENGTH;
			}
			/* Precision. */
			PRECISION:
			switch (*f) {
			case '*':
				prec = va_arg(ap, int);
				f++;
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9': {
				unsigned long uprec;
				errno = 0;
				uprec = strtoul(f, (char **)&f, 10);
				assert(uprec != ULONG_MAX || errno != ERANGE);
				prec = (int)uprec;
				break;
			}
			default: break;
			}
			/* Length. */
			LENGTH:
			switch (*f) {
			case 'l':
				f++;
				if (*f == 'l') {
					len = 'q';
					f++;
				} else
					len = 'l';
				break;
			case 'j':
				len = 'j';
				f++;
				break;
			case 't':
				len = 't';
				f++;
				break;
			case 'z':
				len = 'z';
				f++;
				break;
			default: break;
			}
			/* Conversion specifier. */
			switch (*f) {
				char *s;
				size_t slen;
			case 'd': case 'i': {
				intmax_t val JEMALLOC_CC_SILENCE_INIT(0);
				char buf[D2S_BUFSIZE];

				GET_ARG_NUMERIC(val, len);
				s = d2s(val, (plus_plus ? '+' : (plus_space ?
				    ' ' : '-')), buf, &slen);
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			} case 'o': {
				uintmax_t val JEMALLOC_CC_SILENCE_INIT(0);
				char buf[O2S_BUFSIZE];

				GET_ARG_NUMERIC(val, len);
				s = o2s(val, alt_form, buf, &slen);
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			} case 'u': {
				uintmax_t val JEMALLOC_CC_SILENCE_INIT(0);
				char buf[U2S_BUFSIZE];

				GET_ARG_NUMERIC(val, len);
				s = u2s(val, 10, false, buf, &slen);
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			} case 'x': case 'X': {
				uintmax_t val JEMALLOC_CC_SILENCE_INIT(0);
				char buf[X2S_BUFSIZE];

				GET_ARG_NUMERIC(val, len);
				s = x2s(val, alt_form, *f == 'X', buf, &slen);
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			} case 'c': {
				unsigned char val;
				char buf[2];

				assert(len == '?' || len == 'l');
				assert_not_implemented(len != 'l');
				val = va_arg(ap, int);
				buf[0] = val;
				buf[1] = '\0';
				APPEND_PADDED_S(buf, 1, width, left_justify);
				f++;
				break;
			} case 's':
				assert(len == '?' || len == 'l');
				assert_not_implemented(len != 'l');
				s = va_arg(ap, char *);
				slen = (prec == -1) ? strlen(s) : prec;
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			case 'p': {
				uintmax_t val;
				char buf[X2S_BUFSIZE];

				GET_ARG_NUMERIC(val, 'p');
				s = x2s(val, true, false, buf, &slen);
				APPEND_PADDED_S(s, slen, width, left_justify);
				f++;
				break;
			}
			default: not_implemented();
			}
			break;
		} default: {
			APPEND_C(*f);
			f++;
			break;
		}}
	}
	OUT:
	if (i < size)
		str[i] = '\0';
	else
		str[size - 1] = '\0';
	ret = i;

	if (config_debug) {
		char buf[MALLOC_PRINTF_BUFSIZE];
		int tret;

		/*
		 * Verify that the resulting string matches what vsnprintf()
		 * would have created.
		 */
		tret = vsnprintf(buf, sizeof(buf), format, tap);
		assert(tret == ret);
		assert(strcmp(buf, str) == 0);
	}

#undef APPEND_C
#undef APPEND_S
#undef APPEND_PADDED_S
#undef GET_ARG_NUMERIC
	return (ret);
}

JEMALLOC_ATTR(format(printf, 3, 4))
int
malloc_snprintf(char *str, size_t size, const char *format, ...)
{
	int ret;
	va_list ap;

	va_start(ap, format);
	ret = malloc_vsnprintf(str, size, format, ap);
	va_end(ap);

	return (ret);
}

void
malloc_vcprintf(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *format, va_list ap)
{
	char buf[MALLOC_PRINTF_BUFSIZE];

	if (write_cb == NULL) {
		/*
		 * The caller did not provide an alternate write_cb callback
		 * function, so use the default one.  malloc_write() is an
		 * inline function, so use malloc_message() directly here.
		 */
		write_cb = je_malloc_message;
		cbopaque = NULL;
	}

	malloc_vsnprintf(buf, sizeof(buf), format, ap);
	write_cb(cbopaque, buf);
}

/*
 * Print to a callback function in such a way as to (hopefully) avoid memory
 * allocation.
 */
JEMALLOC_ATTR(format(printf, 3, 4))
void
malloc_cprintf(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(write_cb, cbopaque, format, ap);
	va_end(ap);
}

/* Print to stderr in such a way as to avoid memory allocation. */
JEMALLOC_ATTR(format(printf, 1, 2))
void
malloc_printf(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
}
