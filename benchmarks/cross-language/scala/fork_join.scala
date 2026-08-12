package bench.fork_join

import java.util.concurrent.{ArrayBlockingQueue, CountDownLatch}

// Fan-out to 8 workers, each draining its own bounded queue.
object ForkJoinBenchmark {
  def main(args: Array[String]): Unit = {
    val messages = sys.env.getOrElse("BENCHMARK_MESSAGES", "1000000").toLong
    val workers = 8
    val perWorker = messages / workers
    val total = perWorker * workers

    println("=== Scala Fork-Join Benchmark ===")
    println(s"Workers: $workers, messages: $total")
    println("Using java.util.concurrent.ArrayBlockingQueue\n")

    val queues = Array.fill(workers)(new ArrayBlockingQueue[java.lang.Long](1024))
    val done = new CountDownLatch(workers)

    val start = System.nanoTime()

    var w = 0
    while (w < workers) {
      val me = w
      new Thread(() => {
        var seen = 0L
        while (seen < perWorker) {
          queues(me).take()
          seen += 1
        }
        done.countDown()
      }).start()
      w += 1
    }

    var i = 0L
    while (i < perWorker) {
      var k = 0
      while (k < workers) {
        queues(k).put(java.lang.Long.valueOf(i))
        k += 1
      }
      i += 1
    }
    done.await()

    val elapsed = System.nanoTime() - start
    val nsPerMsg = elapsed / total
    val throughput = total.toDouble / elapsed * 1e9

    println(s"ns/msg:         $nsPerMsg")
    println(f"Throughput:     ${throughput / 1e6}%.2f M msg/sec")
  }
}
