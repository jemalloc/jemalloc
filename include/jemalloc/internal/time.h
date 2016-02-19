/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#define JEMALLOC_CLOCK_GETTIME defined(_POSIX_MONOTONIC_CLOCK) \
    && _POSIX_MONOTONIC_CLOCK >= 0

/* Maximum supported number of seconds (~584 years). */
#define	TIME_SEC_MAX	18446744072

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	time_init(struct timespec *time, time_t sec, long nsec);
time_t	time_sec(const struct timespec *time);
long	time_nsec(const struct timespec *time);
void	time_copy(struct timespec *time, const struct timespec *source);
int	time_compare(const struct timespec *a, const struct timespec *b);
void	time_add(struct timespec *time, const struct timespec *addend);
void	time_subtract(struct timespec *time, const struct timespec *subtrahend);
void	time_imultiply(struct timespec *time, uint64_t multiplier);
void	time_idivide(struct timespec *time, uint64_t divisor);
uint64_t	time_divide(const struct timespec *time,
    const struct timespec *divisor);
bool	time_update(struct timespec *time);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
