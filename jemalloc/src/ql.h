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

/*
 * List definitions.
 */
#define ql_head(a_type)							\
struct {								\
	a_type *qlh_first;						\
}

#define ql_head_initializer(a_head) {NULL}

#define ql_elm(a_type)	qr(a_type)

/* List functions. */
#define ql_new(a_head) do {						\
	(a_head)->qlh_first = NULL;					\
} while (0)

#define ql_elm_new(a_elm, a_field) qr_new((a_elm), a_field)

#define ql_first(a_head) ((a_head)->qlh_first)

#define ql_last(a_head, a_field)					\
	((ql_first(a_head) != NULL)					\
	    ? qr_prev(ql_first(a_head), a_field) : NULL)

#define ql_next(a_head, a_elm, a_field)					\
	((ql_last(a_head, a_field) != (a_elm))				\
	    ? qr_next((a_elm), a_field)	: NULL)

#define ql_prev(a_head, a_elm, a_field)					\
	((ql_first(a_head) != (a_elm)) ? qr_prev((a_elm), a_field)	\
				       : NULL)

#define ql_before_insert(a_head, a_qlelm, a_elm, a_field) do {		\
	qr_before_insert((a_qlelm), (a_elm), a_field);			\
	if (ql_first(a_head) == (a_qlelm)) {				\
		ql_first(a_head) = (a_elm);				\
	}								\
} while (0)

#define ql_after_insert(a_qlelm, a_elm, a_field)			\
	qr_after_insert((a_qlelm), (a_elm), a_field)

#define ql_head_insert(a_head, a_elm, a_field) do {			\
	if (ql_first(a_head) != NULL) {					\
		qr_before_insert(ql_first(a_head), (a_elm), a_field);	\
	}								\
	ql_first(a_head) = (a_elm);					\
} while (0)

#define ql_tail_insert(a_head, a_elm, a_field) do {			\
	if (ql_first(a_head) != NULL) {					\
		qr_before_insert(ql_first(a_head), (a_elm), a_field);	\
	}								\
	ql_first(a_head) = qr_next((a_elm), a_field);			\
} while (0)

#define ql_remove(a_head, a_elm, a_field) do {				\
	if (ql_first(a_head) == (a_elm)) {				\
		ql_first(a_head) = qr_next(ql_first(a_head), a_field);	\
	}								\
	if (ql_first(a_head) != (a_elm)) {				\
		qr_remove((a_elm), a_field);				\
	} else {							\
		ql_first(a_head) = NULL;				\
	}								\
} while (0)

#define ql_head_remove(a_head, a_type, a_field) do {			\
	a_type *t = ql_first(a_head);					\
	ql_remove((a_head), t, a_field);				\
} while (0)

#define ql_tail_remove(a_head, a_type, a_field) do {			\
	a_type *t = ql_last(a_head, a_field);				\
	ql_remove((a_head), t, a_field);				\
} while (0)

#define ql_foreach(a_var, a_head, a_field)				\
	qr_foreach((a_var), ql_first(a_head), a_field)

#define ql_reverse_foreach(a_var, a_head, a_field)			\
	qr_reverse_foreach((a_var), ql_first(a_head), a_field)
