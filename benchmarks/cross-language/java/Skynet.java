// Java Skynet Benchmark
// Based on https://github.com/atemerev/skynet
// Uses ForkJoinPool for recursive parallel decomposition.
// Below the sequential threshold, subtree sums are computed without forking.

import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.RecursiveTask;

public class Skynet {
    private static final int SEQ_THRESHOLD = 1000;

    static class SkynetTask extends RecursiveTask<Long> {
        private final long offset;
        private final long size;

        SkynetTask(long offset, long size) {
            this.offset = offset;
            this.size = size;
        }

        @Override
        protected Long compute() {
            if (size <= SEQ_THRESHOLD) {
                long sum = 0;
                for (long i = 0; i < size; i++) {
                    sum += offset + i;
                }
                return sum;
            }

            long childSize = size / 10;
            long remainder = size - childSize * 10;
            SkynetTask[] children = new SkynetTask[10];
            long childOffset = offset;

            for (int i = 0; i < 10; i++) {
                long cs = childSize + (i < remainder ? 1 : 0);
                children[i] = new SkynetTask(childOffset, cs);
                childOffset += cs;
            }

            for (int i = 1; i < 10; i++) {
                children[i].fork();
            }

            long sum = children[0].compute();
            for (int i = 1; i < 10; i++) {
                sum += children[i].join();
            }
            return sum;
        }
    }

    public static void main(String[] args) {
        String env = System.getenv("BENCHMARK_MESSAGES");
        long numLeaves = env != null ? Long.parseLong(env) : 1000000;

        // Concurrency units actually created; also the divisor, since every
        // language in this suite creates the same count. It used to divide by
        // the whole 1,111,111-node tree while creating 11,111 tasks.
        long totalNodes = 1;
        for (long n = numLeaves; n > SEQ_THRESHOLD; n /= 10) totalNodes += n / SEQ_THRESHOLD;

        ForkJoinPool pool = new ForkJoinPool();

        long start = System.nanoTime();
        long result = pool.invoke(new SkynetTask(0, numLeaves));
        long elapsed = System.nanoTime() - start;

        System.out.println("Leaves: " + numLeaves + ", concurrency units: " + totalNodes
                + " (sequential below " + SEQ_THRESHOLD + ")");
        System.out.println("Sum: " + result);

        if (elapsed > 0) {
            long nsPerMsg = elapsed / totalNodes;
            double throughput = (double) totalNodes / elapsed * 1_000_000_000.0;
            System.out.println("ns/msg:         " + nsPerMsg);
            System.out.printf("Throughput:     %.2f M msg/sec%n", throughput / 1_000_000.0);
        }

        pool.shutdown();
    }
}
