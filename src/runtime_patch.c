#include "jemalloc/internal/jemalloc_internal.h"

#ifdef X86_64_PATCHING

static void unprotect(void *addr, size_t len, int flags) {
        intptr_t start = (intptr_t)PAGE_ADDR2BASE(addr);
        if (mprotect((void *) start, len + (intptr_t)addr - start, flags)) {
		perror("mprotect");
		abort();
	}
}

#define JMP_INSTR_LEN 5

void malloc_patch_option(intptr_t key, bool* option) {
	extern jmp_desc_t __start_jmp_table[], __stop_jmp_table[];
	jmp_desc_t *desc;
	int64_t offset;
	int32_t offset32;
	unsigned long *dest;
	char jmp[8];
	void *newp;

	int patched = 0;

	/* Search for jmps to modify.  Note that because of inlining,
	 * multiple addresses may be found */
	for (desc = __start_jmp_table; desc < __stop_jmp_table; desc++) {
		if (desc->option != key)
			continue;
		patched++;
		offset = (desc->jump_to - desc->jump_from) - JMP_INSTR_LEN;
		if ((offset > INT32_MAX) || (offset < INT32_MIN)) {
			/* We only saved room for a 32-bit jmp.  Hard fail */
			malloc_printf("offset too big: %lx\n", offset);
			abort();
		}

		offset32 = offset;
		dest = desc->jump_from;
		newp = jmp;
		if (*option) {
			/* 32-bit jmp */
			jmp[0] = 0xe9;
			memcpy(jmp+1, &offset32, 4);
			jmp[5] = 0x00;
			jmp[6] = 0x00;
			jmp[7] = 0x00;
		} else {
			/* 8-byte nop */
			jmp[0] = 0x0f;
			jmp[1] = 0x1f;
			jmp[2] = 0x84;
			jmp[3] = 0x00;
			jmp[4] = 0x00;
			jmp[5] = 0x00;
			jmp[6] = 0x00;
			jmp[7] = 0x00;
		}
		/* Write.  Since this is 8 bytes, it is atomic on x86_64.
		 * Concurrently running threads may still take the previous
		 * path for a short while until icache flushes.
		 */
		unprotect(dest, 8, PROT_READ | PROT_EXEC | PROT_WRITE);
		*dest = *(unsigned long*)newp;
		unprotect(dest, 8, PROT_READ | PROT_EXEC);
	}
	assert(patched > 0);
}

#endif
