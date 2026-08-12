// Zig Skynet Benchmark
// Based on https://github.com/atemerev/skynet
// Uses std.Thread while the subtree is larger than SEQ_THRESHOLD.
// Spawning 1M OS threads is not feasible; limits concurrent threads to ~1000.

const std = @import("std");
const Thread = std.Thread;
const print = std.debug.print;

const SEQ_THRESHOLD: i64 = 1000;

fn getLeaves() i64 {
    if (std.posix.getenv("SKYNET_LEAVES")) |val| {
        return std.fmt.parseInt(i64, val, 10) catch 1_000_000;
    }
    if (std.posix.getenv("BENCHMARK_MESSAGES")) |val| {
        return std.fmt.parseInt(i64, val, 10) catch 1_000_000;
    }
    return 1_000_000;
}

fn skynetSeq(offset: i64, size: i64) i64 {
    if (size == 1) return offset;
    const child_size = @divTrunc(size, 10);
    var sum: i64 = 0;
    var i: i64 = 0;
    while (i < 10) : (i += 1) {
        sum += skynetSeq(offset + i * child_size, child_size);
    }
    return sum;
}

const SkynetArg = struct {
    offset: i64,
    size: i64,
    depth: usize,
    result: i64 = 0,
};

fn skynetThread(arg: *SkynetArg) void {
    const offset = arg.offset;
    const size = arg.size;
    const depth = arg.depth;

    if (size <= SEQ_THRESHOLD) {
        arg.result = skynetSeq(offset, size);
        return;
    }

    const child_size = @divTrunc(size, 10);
    var children: [10]SkynetArg = undefined;
    var threads: [10]Thread = undefined;

    for (0..10) |i| {
        children[i] = SkynetArg{
            .offset = offset + @as(i64, @intCast(i)) * child_size,
            .size = child_size,
            .depth = depth + 1,
        };
        threads[i] = Thread.spawn(.{}, skynetThread, .{&children[i]}) catch {
            // Fallback: compute sequentially if spawn fails
            children[i].result = skynetSeq(children[i].offset, children[i].size);
            threads[i] = undefined;
            continue;
        };
    }

    var sum: i64 = 0;
    for (0..10) |i| {
        threads[i].join();
        sum += children[i].result;
    }
    arg.result = sum;
}

fn getTimeNs() u64 {
    const ts = std.posix.clock_gettime(std.posix.CLOCK.MONOTONIC) catch return 0;
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}

pub fn main() !void {
    const num_leaves = getLeaves();

    // Concurrency units actually created; also the divisor, since every language
    // in this suite creates the same count.
    var total_actors: i64 = 1;
    var n = num_leaves;
    while (n > SEQ_THRESHOLD) : (n = @divTrunc(n, 10)) {
        total_actors += @divTrunc(n, SEQ_THRESHOLD);
    }

    print("=== Zig Skynet Benchmark ===\n", .{});
    print("Leaves: {}, concurrency units: {} (sequential below {})\n\n", .{ num_leaves, total_actors, SEQ_THRESHOLD });

    var root = SkynetArg{ .offset = 0, .size = num_leaves, .depth = 0 };

    const start = getTimeNs();
    const root_thread = try Thread.spawn(.{}, skynetThread, .{&root});
    root_thread.join();
    const end = getTimeNs();

    const elapsed_ns = @as(i64, @intCast(end - start));
    const elapsed_us = @divTrunc(elapsed_ns, 1000);

    print("Sum: {}\n", .{root.result});
    if (elapsed_us > 0) {
        const ns_per_msg = @divTrunc(elapsed_ns, total_actors);
        // Float, like the other four Zig benchmarks. The manual
        // integer-and-fraction version printed "0.+4" for any rate below
        // 1 M/sec, which the runner's parser reads as garbage.
        const throughput = @as(f64, @floatFromInt(total_actors)) /
            (@as(f64, @floatFromInt(elapsed_ns)) / 1_000_000_000.0) / 1_000_000.0;
        print("ns/msg:         {}\n", .{ns_per_msg});
        print("Throughput:     {d:.2} M msg/sec\n", .{throughput});
    }
}
