#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    // Concatenate all arguments into a single string
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        len += strlen(argv[i]);
    }

    char* str = malloc(len + 1);
    if (!str) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    str[0] = '\0';
    for (int i = 1; i < argc; i++) {
        strcat(str, argv[i]);
    }

    printf("Concatenated string: %s\n", str);

    // Additional allocations to exercise the allocator
    size_t n_allocs = 1000;
    void** allocs = malloc(n_allocs * sizeof(void*));
    if (!allocs) {
        fprintf(stderr, "malloc for allocs array failed\n");
        free(str);
        return 1;
    }
    for (size_t i = 0; i < n_allocs; i++) {
        allocs[i] = malloc(i + 1);  // Varying sizes
        if (!allocs[i]) {
            fprintf(stderr, "malloc failed at iteration %zu\n", i);
            break;
        }
        memset(allocs[i], 'A', i + 1);  // Fill to ensure allocation is usable
    }

    // Clean up
    for (size_t i = 0; i < n_allocs; i++) {
        free(allocs[i]);
    }
    free(allocs);
    free(str);

    printf("All allocations and frees completed successfully.\n");
    return 0;
}