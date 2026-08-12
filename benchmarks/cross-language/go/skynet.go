// Go Skynet Benchmark
// Based on https://github.com/atemerev/skynet
// Recursive tree of goroutines: root spawns 10 children, each spawns 10 more, etc.
// Leaves report their offset; parents aggregate 10 children's results and report up.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func getLeaves() int64 {
	if env := os.Getenv("SKYNET_LEAVES"); env != "" {
		if n, err := strconv.ParseInt(env, 10, 64); err == nil {
			return n
		}
	}
	if env := os.Getenv("BENCHMARK_MESSAGES"); env != "" {
		if n, err := strconv.ParseInt(env, 10, 64); err == nil {
			return n
		}
	}
	return 1000000
}

// skynetNode sends its subtree sum to the result channel.
// Leaves send their offset directly; internal nodes spawn 10 children and sum.
// seqSum sums a subtree on the current goroutine.
const seqThreshold int64 = 1000

func seqSum(offset, size int64) int64 {
	var sum int64
	for i := int64(0); i < size; i++ {
		sum += offset + i
	}
	return sum
}

func skynetNode(result chan<- int64, offset, size int64) {
	// Same threshold as every other language here. See FAIRNESS.md.
	if size <= seqThreshold {
		result <- seqSum(offset, size)
		return
	}
	children := make(chan int64, 10)
	childSize := size / 10
	for i := int64(0); i < 10; i++ {
		go skynetNode(children, offset+i*childSize, childSize)
	}
	var sum int64
	for i := 0; i < 10; i++ {
		sum += <-children
	}
	result <- sum
}

func main() {
	numLeaves := getLeaves()

	// Units created; also the divisor. See FAIRNESS.md.
	totalActors := int64(1)
	for n := numLeaves; n > seqThreshold; n /= 10 {
		totalActors += n / seqThreshold
	}
	rateBase := totalActors

	fmt.Println("=== Go Skynet Benchmark ===")
	fmt.Printf("Leaves: %d, concurrency units: %d (sequential below %d)\n\n",
		numLeaves, totalActors, seqThreshold)

	root := make(chan int64, 1)
	start := time.Now()
	go skynetNode(root, 0, numLeaves)
	sum := <-root
	elapsed := time.Since(start)

	elapsedNs := elapsed.Nanoseconds()
	elapsedUs := elapsedNs / 1000

	fmt.Printf("Sum: %d\n", sum)
	if elapsedUs > 0 {
		nsPerMsg := elapsedNs / rateBase
		throughputM := rateBase / elapsedUs
		leftover := rateBase - (throughputM * elapsedUs)
		throughputFrac := (leftover * 100) / elapsedUs
		fmt.Printf("ns/msg:         %d\n", nsPerMsg)
		fmt.Printf("Throughput:     %d.", throughputM)
		if throughputFrac < 10 {
			fmt.Printf("0")
		}
		fmt.Printf("%d M msg/sec\n", throughputFrac)
	}
}
