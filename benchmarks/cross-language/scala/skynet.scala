package bench.skynet

import java.util.concurrent.{ForkJoinPool, RecursiveTask}

// Same threshold as every other language here; see FAIRNESS.md.
object Leaf {
  val SeqThreshold = 1000L

  def sum(offset: Long, size: Long): Long = {
    var acc = 0L
    var i = 0L
    while (i < size) { acc += offset + i; i += 1 }
    acc
  }
}

final class SkynetTask(offset: Long, size: Long) extends RecursiveTask[Long] {
  def compute(): Long = {
    if (size <= Leaf.SeqThreshold) return Leaf.sum(offset, size)
    val childSize = size / 10
    val children = new Array[SkynetTask](10)
    var i = 0
    while (i < 10) {
      children(i) = new SkynetTask(offset + i * childSize, childSize)
      children(i).fork()
      i += 1
    }
    var total = 0L
    i = 9
    while (i >= 0) { total += children(i).join(); i -= 1 }
    total
  }
}

object SkynetBenchmark {
  def main(args: Array[String]): Unit = {
    val numLeaves = sys.env.getOrElse("BENCHMARK_MESSAGES", "1000000").toLong

    // Units created; also the divisor. See FAIRNESS.md.
    var units = 1L
    var n = numLeaves
    while (n > Leaf.SeqThreshold) { units += n / Leaf.SeqThreshold; n /= 10 }

    println("=== Scala Skynet Benchmark ===")
    println(s"Leaves: $numLeaves, concurrency units: $units (sequential below ${Leaf.SeqThreshold})")
    println("Using java.util.concurrent.ForkJoinPool\n")

    val start = System.nanoTime()
    val pool = new ForkJoinPool()
    val result = pool.invoke(new SkynetTask(0, numLeaves))
    val elapsed = System.nanoTime() - start

    println(s"Sum: $result")
    val nsPerMsg = elapsed / units
    val throughput = units.toDouble / elapsed * 1e9

    println(s"ns/msg:         $nsPerMsg")
    println(f"Throughput:     ${throughput / 1e6}%.2f M msg/sec")
  }
}
