package bench.thread_ring

import java.util.concurrent.ArrayBlockingQueue

// 100 threads in a ring, each passing the token to the next.
object ThreadRingBenchmark {
  def main(args: Array[String]): Unit = {
    val messages = sys.env.getOrElse("BENCHMARK_MESSAGES", "1000000").toLong
    val ringSize = 100

    println("=== Scala Thread Ring Benchmark ===")
    println(s"Ring size: $ringSize, messages: $messages")
    println("Using java.util.concurrent.ArrayBlockingQueue\n")

    val queues = Array.fill(ringSize)(new ArrayBlockingQueue[java.lang.Long](1))
    val threads = new Array[Thread](ringSize)

    val start = System.nanoTime()

    var id = 0
    while (id < ringSize) {
      val me = id
      val next = (id + 1) % ringSize
      threads(me) = new Thread(() => {
        var running = true
        while (running) {
          val token = queues(me).take().longValue()
          if (token <= 0) {
            if (me != ringSize - 1) queues(next).put(java.lang.Long.valueOf(token))
            running = false
          } else {
            queues(next).put(java.lang.Long.valueOf(token - 1))
          }
        }
      })
      threads(me).start()
      id += 1
    }

    queues(0).put(java.lang.Long.valueOf(messages))
    var t = 0
    while (t < ringSize) { threads(t).join(); t += 1 }

    val elapsed = System.nanoTime() - start
    val nsPerMsg = elapsed / messages
    val throughput = messages.toDouble / elapsed * 1e9

    println(s"ns/msg:         $nsPerMsg")
    println(f"Throughput:     ${throughput / 1e6}%.2f M msg/sec")
  }
}
