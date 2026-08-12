package bench.ping_pong

import java.util.concurrent.ArrayBlockingQueue

// Two threads exchanging one value at a time in each direction: a strict
// request-response round trip, as in the other implementations.
object PingPongBenchmark {
  def main(args: Array[String]): Unit = {
    val messages = sys.env.getOrElse("BENCHMARK_MESSAGES", "1000000").toLong

    println("=== Scala Ping-Pong Benchmark ===")
    println(s"Messages: $messages")
    println("Using java.util.concurrent.ArrayBlockingQueue\n")

    val toPong = new ArrayBlockingQueue[java.lang.Long](1)
    val toPing = new ArrayBlockingQueue[java.lang.Long](1)
    @volatile var errors = 0L

    val start = System.nanoTime()

    val pong = new Thread(() => {
      var n = 0L
      while (n < messages) {
        val v = toPong.take()
        toPing.put(v)
        n += 1
      }
    })
    pong.start()

    var i = 0L
    while (i < messages) {
      toPong.put(java.lang.Long.valueOf(i))
      val back = toPing.take()
      if (back.longValue() != i) errors += 1
      i += 1
    }
    pong.join()

    val elapsed = System.nanoTime() - start
    if (errors > 0) println(s"VALIDATION FAILED: $errors errors")

    val nsPerMsg = elapsed / messages
    val throughput = messages.toDouble / elapsed * 1e9

    println(s"ns/msg:         $nsPerMsg")
    println(f"Throughput:     ${throughput / 1e6}%.2f M msg/sec")
  }
}
