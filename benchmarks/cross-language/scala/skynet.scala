package bench.skynet

// Scala Akka Skynet Benchmark
// Based on https://github.com/atemerev/skynet
// Recursive actor tree using Akka actors.
// Below the sequential threshold, subtree sums are computed without spawning.

import akka.actor.{Actor, ActorRef, ActorSystem, Props}
import scala.concurrent.{Await, Promise}
import scala.concurrent.duration._

case class Compute(offset: Long, size: Long)
case class SkynetResult(value: Long)

// One definition, shared by the actor and the reporting below: the same
// threshold every language in this suite uses, so all create 1,111 units.
object SkynetNode {
  val SeqThreshold = 1000L
}

class SkynetNode(parent: ActorRef) extends Actor {
  var pending = 0
  var total: Long = 0L
  import SkynetNode.SeqThreshold

  def receive = {
    case Compute(offset, size) =>
      if (size <= SeqThreshold) {
        var sum = 0L
        var i = 0L
        while (i < size) {
          sum += offset + i
          i += 1
        }
        parent ! SkynetResult(sum)
        context.stop(self)
      } else {
        val childSize = size / 10
        val remainder = size - childSize * 10
        pending = 10
        var childOffset = offset
        var idx = 0
        while (idx < 10) {
          val cs = childSize + (if (idx < remainder) 1 else 0)
          val child = context.actorOf(Props(new SkynetNode(self)))
          child ! Compute(childOffset, cs)
          childOffset += cs
          idx += 1
        }
      }

    case SkynetResult(value) =>
      total += value
      pending -= 1
      if (pending == 0) {
        parent ! SkynetResult(total)
        context.stop(self)
      }
  }
}

class SkynetRoot(numLeaves: Long, promise: Promise[Long]) extends Actor {
  def receive = {
    case SkynetResult(value) =>
      promise.success(value)
      context.stop(self)
  }

  override def preStart(): Unit = {
    val root = context.actorOf(Props(new SkynetNode(self)))
    root ! Compute(0, numLeaves)
  }
}

object SkynetBenchmark extends App {
  val envVal = sys.env.getOrElse("BENCHMARK_MESSAGES", "1000000")
  val numLeaves = envVal.toLong

  // Total tree nodes (same formula as all languages for fair comparison)
  // Concurrency units actually created; also the divisor, since every language
  // in this suite creates the same count.
  var totalNodes = 1L
  var nn = numLeaves
  while (nn > SkynetNode.SeqThreshold) { totalNodes += nn / SkynetNode.SeqThreshold; nn /= 10 }

  val system = ActorSystem("skynet")
  val promise = Promise[Long]()

  val start = System.nanoTime()
  system.actorOf(Props(new SkynetRoot(numLeaves, promise)))
  val result = Await.result(promise.future, 60.seconds)
  val elapsed = System.nanoTime() - start

  println(s"Leaves: $numLeaves, concurrency units: $totalNodes (sequential below ${SkynetNode.SeqThreshold})")
  println(s"Sum: $result")

  if (elapsed > 0) {
    val nsPerMsg = elapsed / totalNodes
    val throughput = totalNodes.toDouble / elapsed * 1e9
    println(s"ns/msg:         $nsPerMsg")
    println(f"Throughput:     ${throughput / 1e6}%.2f M msg/sec")
  }

  system.terminate()
  Await.result(system.whenTerminated, 10.seconds)
}
