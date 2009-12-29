/******************************************************************************
 *
 * Copyright (C) 2002 Jason Evans <jasone@canonware.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice(s), this list of conditions and the following disclaimer
 *    unmodified other than the allowable addition of one or more
 *    copyright notices.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice(s), this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

/* Ring definitions. */
#define qr(a_type)							\
struct {								\
	a_type	*qre_next;						\
	a_type	*qre_prev;						\
}

/* Ring functions. */
#define qr_new(a_qr, a_field) do {					\
	(a_qr)->a_field.qre_next = (a_qr);				\
	(a_qr)->a_field.qre_prev = (a_qr);				\
} while (0)

#define qr_next(a_qr, a_field) ((a_qr)->a_field.qre_next)

#define qr_prev(a_qr, a_field) ((a_qr)->a_field.qre_prev)

#define qr_before_insert(a_qrelm, a_qr, a_field) do {			\
	(a_qr)->a_field.qre_prev = (a_qrelm)->a_field.qre_prev;		\
	(a_qr)->a_field.qre_next = (a_qrelm);				\
	(a_qr)->a_field.qre_prev->a_field.qre_next = (a_qr);		\
	(a_qrelm)->a_field.qre_prev = (a_qr);				\
} while (0)

#define qr_after_insert(a_qrelm, a_qr, a_field)				\
    do									\
    {									\
	(a_qr)->a_field.qre_next = (a_qrelm)->a_field.qre_next;		\
	(a_qr)->a_field.qre_prev = (a_qrelm);				\
	(a_qr)->a_field.qre_next->a_field.qre_prev = (a_qr);		\
	(a_qrelm)->a_field.qre_next = (a_qr);				\
    } while (0)

#define qr_meld(a_qr_a, a_qr_b, a_field) do {				\
	void *t;							\
	(a_qr_a)->a_field.qre_prev->a_field.qre_next = (a_qr_b);	\
	(a_qr_b)->a_field.qre_prev->a_field.qre_next = (a_qr_a);	\
	t = (a_qr_a)->a_field.qre_prev;					\
	(a_qr_a)->a_field.qre_prev = (a_qr_b)->a_field.qre_prev;	\
	(a_qr_b)->a_field.qre_prev = t;					\
} while (0)

/* qr_meld() and qr_split() are functionally equivalent, so there's no need to
 * have two copies of the code. */
#define qr_split(a_qr_a, a_qr_b, a_field)				\
	qr_meld((a_qr_a), (a_qr_b), a_field)

#define qr_remove(a_qr, a_field) do {					\
	(a_qr)->a_field.qre_prev->a_field.qre_next			\
	    = (a_qr)->a_field.qre_next;					\
	(a_qr)->a_field.qre_next->a_field.qre_prev			\
	    = (a_qr)->a_field.qre_prev;					\
	(a_qr)->a_field.qre_next = (a_qr);				\
	(a_qr)->a_field.qre_prev = (a_qr);				\
} while (0)

#define qr_foreach(var, a_qr, a_field)					\
	for ((var) = (a_qr);						\
	    (var) != NULL;						\
	    (var) = (((var)->a_field.qre_next != (a_qr))		\
	    ? (var)->a_field.qre_next : NULL))

#define qr_reverse_foreach(var, a_qr, a_field)				\
	for ((var) = ((a_qr) != NULL) ? qr_prev(a_qr, a_field) : NULL;	\
	    (var) != NULL;						\
	    (var) = (((var) != (a_qr))					\
	    ? (var)->a_field.qre_prev : NULL))
