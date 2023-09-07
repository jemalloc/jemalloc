#include "test/jemalloc_test.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

char const * malloc_conf =
    "confirm_conf:true,hpa_hugification_threshold_ratio:0.9,hpa_dirty_mult:0.11,hpa:true,metadata_thp:always,thp:default";


TEST_BEGIN(test_basic) {
	auto foo = new long(4);
	expect_ptr_not_null(foo, "Unexpected new[] failure");
	delete foo;
	// Test nullptr handling.
	foo = nullptr;
	delete foo;

	auto bar = new long;
	expect_ptr_not_null(bar, "Unexpected new failure");
	delete bar;
	// Test nullptr handling.
	bar = nullptr;
	delete bar;
}
TEST_END


using namespace std::chrono_literals;

enum class op_type: uint8_t {
    malloc,
    free,
    mallocx,
    sdallocx,
    realloc,
    calloc
};

struct op {
    op_type type; // type of the operation,e.g. malloc/free/mallocx/...
    size_t param; // e.g. size for malloc, address for free, etc.
    size_t ret; // return value, e.g. address for malloc
    int flags;
    size_t extra_param;
};

static std::unordered_map<size_t, std::vector<op>> replay_buffer;

static std::unordered_map<size_t, void*> addr_mapping;

std::mutex g_mutex;

void* map_addr(size_t addr) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!addr_mapping.count(addr)) {
        return nullptr;
    }
    return addr_mapping[addr];
}

void erase_addr(size_t addr) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (addr_mapping[addr]) {
        addr_mapping.erase(addr);
    }
}

void fix_addr(size_t original, void* actual) {
    std::lock_guard<std::mutex> guard(g_mutex);
    addr_mapping[original] = actual;
}

// Thread function performs "replay" or simulation by walking through the replay buffer
// and performing the given operations
void* thread_func(void* v_tid) {
    size_t tid = *static_cast<size_t*>(v_tid);
    //std::cout << "Inside thread function with tid=" << tid << " \n";


    uintptr_t result = 0;
    void * addr = nullptr;
    void * addr_mapped = nullptr;
    
    // Repeat several times to increase stressing
    for (size_t i = 0; i < 10; ++i) {
        // For the current thread (given by v_tid) iterate its replay buffer
        for (auto && o : replay_buffer[tid]) {

            // Sleep only sometimes, we do this to give sleep_for(0ns) much higher than uniform (w.r.t. [0ns, 10000ns] range) probability
            if (rand() % 2) {
                // Try sleep for a random amount of time
                std::this_thread::sleep_for(1ns * (rand() % 10000));
            }

            switch (o.type) {
                case op_type::malloc:
                addr = malloc(o.param);
                assert(addr != nullptr);
                // The problem is that addresses in logs do not match (for obvious reasons) addresses that will newly be returned from malloc
                // For that reason, fix maintain an additional address mapping
                result += reinterpret_cast<uintptr_t>(addr);
                fix_addr(o.ret, addr);
                break;
                
                case op_type::free:
                addr_mapped = map_addr(o.param);
                if (addr_mapped) {
                    free(addr_mapped);
                    erase_addr(o.param);
                }
                break;
                
                case op_type::mallocx:
                //std::cout << "TID=" << tid << "MALLOCX, param=" << o.param << " flags=" << o.flags << std::endl;
                addr = mallocx(o.param, o.flags);
                //std::cout << "After jemallocx" << std::endl;
                assert(addr != nullptr);
                result += reinterpret_cast<uintptr_t>(addr);
                fix_addr(o.ret, addr);
                break;

                case op_type::sdallocx:
                //std::cout << "TID=" << tid << "SDALLOCX" << " o.ret=" << o.ret << std::endl;
                addr_mapped = map_addr(o.param);
                if (addr_mapped) {
                    sdallocx(addr_mapped, o.extra_param, o.flags);
                    erase_addr(o.param);
                }
                break;

                case op_type::realloc:
                //std::cout << "TID=" << tid << "REALLOC" << std::endl;
                addr_mapped = map_addr(o.param);
                if (addr_mapped) {
                    addr = realloc(addr_mapped, o.extra_param);
                    fix_addr(o.ret, addr);
                    erase_addr(o.param);
                }
                break;

                case op_type::calloc:
                //std::cout << "TID=" << tid << "CALLOC" << std::endl;
                addr = calloc(o.param, o.extra_param);
                result += reinterpret_cast<uintptr_t>(addr);
                fix_addr(o.ret, addr);
                break;

                default:
                //nop
                break;
            }
        }
    }

	//std::cout << "Ending" << std::endl;
    return reinterpret_cast<void*>(result);
}

// Takes string that contains line of the normalized_out file (logs gathered from Jemalloc)
// Searches for occurrence of "key" in the line, e.g. line="core.calloc.entry: num: 16, size: 8"
// and key="num: " should return 16. Parsing is done in the given base (decimal by default)
size_t parse_value(char const * key, const std::string & line, int base = 10)
{
    size_t val = std::string::npos;
    auto val_start = line.find(key);
    if (val_start != std::string::npos) {
        auto val_end = line.find(',', val_start);
        //std::cout << "KEY=" << key << " LINE=" << line << " VAL_START=" << val_start << " END_VAL=" << val_end << std::endl;
        val = std::stoul(line.substr(val_start + strlen(key), val_end - val_start), 0, base);
    }
    if (val == std::string::npos) {
        std::cout << "line=" << line << " cannot find key=" << key << std::endl;
    }
    assert(val != std::string::npos);
    return val;
}

// Fill-in the so-called "replay buffer", i.e. vector of operations
// where each operation is one of {malloc, free, mallocx, realloc, calloc, sdallocx}
// Replay bufer is read from log_file_path
void fill_replay_buffers(char const * log_file_path) {
    std::string line;
    std::ifstream infile(log_file_path);
    // Read the file line-by-line
    while (std::getline(infile, line))
    {   
        // For each line we will parse TID and operation o
        size_t tid = std::numeric_limits<size_t>::max(); //, timestamp;
        op o = {};
        
        // Decode/parse individual operations

        // Malloc
        if (line.rfind("core.malloc.exit", 0) == 0)
        {
            // Type of operation is malloc
            o.type = op_type::malloc;
            // Malloc itself takes size as a parameter
            o.param = parse_value("size: ", line);
            // And returns (hex) address pointing to the allocated region
            o.ret = parse_value("result: ", line, 16);
            // Parse the TID
            tid = parse_value("tid: ", line);
        }
        else if (line.rfind("core.free.entry", 0) == 0)
        {
            // Type of operation is free
            o.type = op_type::free;
            // Free takes address to be dealocated (hex)
            o.param = parse_value("ptr: ", line, 16);
            // Parse the TID
            tid = parse_value("tid: ", line);
            replay_buffer[tid].push_back(o);
        }
        else if (line.rfind("core.realloc.exit", 0) == 0)
        {
            // Type of operation is realloc
            o.type = op_type::realloc;
            // Realloc takes two parameters, pointer to be reallocated
            o.param = parse_value("ptr: ", line, 16);
            // and size
            o.extra_param = parse_value("size: ", line, 16);
            // Parse the TID            
            tid = parse_value("tid: ", line);
            // Returns resulting (after reallocation) address
            o.ret = parse_value("result: ", line, 16);
            
        }
        else if (line.rfind("core.mallocx.exit", 0) == 0)
        {
            // Type of operation is mallocx
            o.type = op_type::mallocx;
            // Mallocx is takes a size to be allocated
            o.param = parse_value("size: ", line);
            // And also the flags
            o.flags = static_cast<int>(parse_value("flags: ", line));
            // And returns address pointing to allocated region (hex)
            o.ret = parse_value("result: ", line, 16);
            // Parse the TID
            tid = parse_value("tid: ", line);
        }
        else if (line.rfind("core.sdallocx.exit", 0) == 0)
        {
            // Type of the operation is sdallocx
            o.type = op_type::sdallocx;
            // It takes three parameters
            // Pointer to be deallocated
            o.param = parse_value("ptr: ", line, 16);
            // Size
            o.extra_param = parse_value("size: ", line);
            // And flags
            o.flags = static_cast<int>(parse_value("flags: ", line));
            // Parse the TID
            tid = parse_value("tid: ", line);
        }
        else if (line.rfind("core.calloc.exit", 0) == 0)
        {
            // Type of the operation is calloc
            o.type = op_type::calloc;
            // Takes two parameters: num and size
            o.param = parse_value("num: ", line);
            o.extra_param = parse_value("size: ", line);
            // And returns resulting address
            o.ret = parse_value("result: ", line, 16);
            // Parse the TID
            tid = parse_value("tid: ", line);
        }

        if (tid != std::numeric_limits<size_t>::max()) {
            replay_buffer[tid].push_back(std::move(o));
        }

    }
}

TEST_BEGIN(test_hpa_inf_loop_reproducer) {
	size_t constexpr n_threads = 3;

    // Parse the log-file and create vectors with (de-)alloc operations
	fill_replay_buffers("./test/integration/cpp/normalized_out.txt");
    
    // Create and start the threads
    pthread_t threads[n_threads];
    for (size_t i = 0; i < n_threads; i++) {
        size_t * tid = static_cast<size_t*>(malloc(sizeof(size_t)));
        *tid = i;
		pthread_create(&threads[i], NULL, thread_func, static_cast<void*>(tid));
	}

    // Wait for threads to finish
    uintptr_t total = 0;
	for (size_t i = 0; i < n_threads; i++) {
		uintptr_t result;
		pthread_join(threads[i], reinterpret_cast<void**>(&result));
		total += result;
	}
}
TEST_END

int
main() {
	return test(
	    test_basic,
		test_hpa_inf_loop_reproducer
	);
}
