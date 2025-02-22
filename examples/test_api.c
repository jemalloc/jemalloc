#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jemalloc/jemalloc.h>
#include <errno.h>
#include <getopt.h>

void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -h, --help                 Show this help message\n");
    printf("  -v, --verbose              Enable verbose output\n");
    printf("  --expect-stats-unavailable Expect stats to be unavailable (exit 0 if unavailable, 1 if not)\n");
    printf("                             By default, test expects stats to be available\n");
}

/*
 * This function explicitly tests stats functionality and optionally prints available stats
 * which can be disabled with --disable-stats
 * Returns 0 if stats are available, 1 if stats are disabled
 */
int test_stats_functionality(int verbose) {
    size_t allocated, active, resident, metadata, mapped, retained;
    size_t sz = sizeof(size_t);
    int ret;

    // Try to access stats.allocated - will fail if stats are disabled
    ret = mallctl("stats.allocated", &allocated, &sz, NULL, 0);

    if (ret != 0) {
        fprintf(stderr, "Stats error: Failed to get stats.allocated: %s (error code: %d)\n",
                strerror(ret), ret);
        if (ret == ENOENT) {
            fprintf(stderr, "This indicates jemalloc was compiled with --disable-stats\n");
        }
        return 1; // Stats are unavailable
    }

    // Stats are available, print them if verbose mode
    if (verbose) {
        printf("\nJemalloc Memory Stats:\n");
        printf("  Allocated: %zu bytes (%.2f MB)\n", allocated, (double)allocated / (1024 * 1024));

        // Get additional stats and print them when available
        if (mallctl("stats.active", &active, &sz, NULL, 0) == 0) {
            printf("  Active: %zu bytes (%.2f MB)\n", active, (double)active / (1024 * 1024));
        }

        if (mallctl("stats.resident", &resident, &sz, NULL, 0) == 0) {
            printf("  Resident: %zu bytes (%.2f MB)\n", resident, (double)resident / (1024 * 1024));
        }

        if (mallctl("stats.metadata", &metadata, &sz, NULL, 0) == 0) {
            printf("  Metadata: %zu bytes (%.2f MB)\n", metadata, (double)metadata / (1024 * 1024));
        }

        if (mallctl("stats.mapped", &mapped, &sz, NULL, 0) == 0) {
            printf("  Mapped: %zu bytes (%.2f MB)\n", mapped, (double)mapped / (1024 * 1024));
        }

        if (mallctl("stats.retained", &retained, &sz, NULL, 0) == 0) {
            printf("  Retained: %zu bytes (%.2f MB)\n", retained, (double)retained / (1024 * 1024));
        }
    }

    return 0; // Stats are available
}

int main(int argc, char** argv) {
    int expect_stats_available = 1; // Default: expect stats to be available
    int verbose = 0; // Default: non-verbose output

    // Define long options
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"expect-stats-unavailable", no_argument, 0, 'u'},
        {0, 0, 0, 0}
    };

    // Parse command line options
    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                verbose = 1;
                break;
            case 'u':
                expect_stats_available = 0; // Expect stats to be unavailable
                break;
            default:
                print_usage(argv[0]);
                return 2;
        }
    }

    // Basic check to ensure jemalloc is loaded
    const char *version;
    size_t version_size = sizeof(version);
    if (mallctl("version", (void*)&version, &version_size, NULL, 0) != 0) {
        fprintf(stderr, "Error: mallctl 'version' failed. Jemalloc not loaded?\n");
        return 1; // Always fail if jemalloc isn't loaded
    }

    if (verbose) {
        printf("Using jemalloc version: %s\n", version);
    }

    // Perform some allocations to have something to measure
    const int num_allocs = 10;
    void* ptrs[num_allocs];
    for (int i = 0; i < num_allocs; i++) {
        // Allocate blocks of increasing size
        size_t size = 1024 * (i + 1);
        ptrs[i] = malloc(size);
        if (!ptrs[i]) {
            fprintf(stderr, "malloc failed for size %zu\n", size);
            return 1;
        }
        // Write some data to ensure pages are committed
        memset(ptrs[i], i, size);
    }

    // Test stats functionality
    int stats_unavailable = test_stats_functionality(verbose);

    // Clean up allocations
    for (int i = 0; i < num_allocs; i++) {
        free(ptrs[i]);
    }

    // Determine exit code based on stats availability and expectation
    int exit_code;

    if (expect_stats_available) {
        // Expecting stats to be available
        exit_code = stats_unavailable ? 1 : 0;
    } else {
        // Expecting stats to be unavailable
        exit_code = stats_unavailable ? 0 : 1;
    }

    // Only print this if verbose mode is enabled
    if (verbose) {
        printf("\nStats %s - Test %s\n",
               stats_unavailable ? "UNAVAILABLE" : "AVAILABLE",
               exit_code == 0 ? "PASSED" : "FAILED");
    }

    return exit_code;
}