/**
 * C++ Skynet Benchmark
 * Based on https://github.com/atemerev/skynet
 * Uses std::thread while the subtree is larger than SEQ_THRESHOLD.
 * Spawning 1M OS threads is not feasible; limits concurrent threads to ~1000.
 *
 * Compile with: g++ -O3 -std=c++17 -march=native skynet.cpp -o skynet -pthread
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <array>

// Sequential below SEQ_THRESHOLD. The same threshold in every language in this
// suite, so all of them create the same 1,111 concurrency units and perform the
// same leaf additions. The divisor is that unit count, not the tree's 1,111,111
// nodes: dividing by nodes nobody created scored the implementations that
// created fewest the highest.
static const long long SEQ_THRESHOLD = 1000;

static long long skynet_seq(long long offset, long long size) {
    if (size == 1) return offset;
    long long child_size = size / 10;
    long long sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += skynet_seq(offset + (long long)i * child_size, child_size);
    }
    return sum;
}

// Each node sends its result to a mutex-protected slot, then signals its parent.
struct ResultSlot {
    long long value = 0;
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
};

static void skynet(ResultSlot* out, long long offset, long long size, int depth) {
    if (size <= SEQ_THRESHOLD) {
        std::lock_guard<std::mutex> lock(out->mtx);
        out->value = skynet_seq(offset, size);
        out->ready = true;
        out->cv.notify_one();
        return;
    }
    long long child_size = size / 10;
    std::array<ResultSlot, 10> slots;
    std::array<std::thread, 10> threads;

    for (int i = 0; i < 10; i++) {
        long long child_offset = offset + (long long)i * child_size;
        ResultSlot* slot = &slots[i];
        int next_depth = depth + 1;
        threads[i] = std::thread([slot, child_offset, child_size, next_depth]() {
            skynet(slot, child_offset, child_size, next_depth);
        });
    }

    long long sum = 0;
    for (int i = 0; i < 10; i++) {
        threads[i].join();
        sum += slots[i].value;
    }

    std::lock_guard<std::mutex> lock(out->mtx);
    out->value = sum;
    out->ready = true;
    out->cv.notify_one();
}

static long long get_leaves() {
    const char* env = getenv("SKYNET_LEAVES");
    if (env) return atoll(env);
    env = getenv("BENCHMARK_MESSAGES");
    if (env) return atoll(env);
    return 1000000LL;
}

int main() {
    long long num_leaves = get_leaves();

    // Concurrency units actually created; also the divisor, since every language
    // in this suite creates the same count.
    long long total_actors = 1;
    for (long long n = num_leaves; n > SEQ_THRESHOLD; n /= 10) {
        total_actors += n / SEQ_THRESHOLD;
    }

    std::cout << "=== C++ Skynet Benchmark ===" << std::endl;
    std::cout << "Leaves: " << num_leaves << ", concurrency units: " << total_actors << " (sequential below " << SEQ_THRESHOLD << ")" << std::endl;
    std::cout << std::endl;

    ResultSlot root;
    auto start = std::chrono::high_resolution_clock::now();
    std::thread t([&root, num_leaves]() { skynet(&root, 0, num_leaves, 0); });
    t.join();
    auto end = std::chrono::high_resolution_clock::now();

    long long elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    long long elapsed_us = elapsed_ns / 1000;

    std::cout << "Sum: " << root.value << std::endl;
    if (elapsed_us > 0) {
        long long ns_per_msg = elapsed_ns / total_actors;
        long long throughput_m = total_actors / elapsed_us;
        long long leftover = total_actors - (throughput_m * elapsed_us);
        long long throughput_frac = (leftover * 100) / elapsed_us;
        std::cout << "ns/msg:         " << ns_per_msg << std::endl;
        std::cout << "Throughput:     " << throughput_m << ".";
        if (throughput_frac < 10) std::cout << "0";
        std::cout << throughput_frac << " M msg/sec" << std::endl;
    }
    return 0;
}
