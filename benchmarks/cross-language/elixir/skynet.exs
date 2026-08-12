#!/usr/bin/env elixir

# Elixir Skynet Benchmark
# Based on https://github.com/atemerev/skynet
# Uses native Erlang/Elixir processes for each tree node (like Erlang implementation).
# For 1M leaves, the BEAM process limit must be raised (pass +P 2000000 to erl).

defmodule Skynet do
  @default_leaves 1_000_000

  defp get_leaves do
    case System.get_env("SKYNET_LEAVES") do
      nil ->
        case System.get_env("BENCHMARK_MESSAGES") do
          nil -> @default_leaves
          val -> String.to_integer(val)
        end
      val -> String.to_integer(val)
    end
  end

  # Total actors = sum of nodes at each level
  # Units created; also the divisor. See FAIRNESS.md.
  @seq_threshold 1000
  defp total_actors(n) when n <= @seq_threshold, do: 1
  defp total_actors(n), do: div(n, @seq_threshold) + total_actors(div(n, 10))

  # Leaf: send offset to parent. Internal: spawn 10 children, collect, sum, report up.
  # Same threshold as every other language here.
  def skynet_node(offset, size, parent) when size <= @seq_threshold do
    send(parent, seq_sum(offset, size, 0))
  end

  def skynet_node(offset, size, parent) do
    child_size = div(size, 10)
    self_pid = self()
    Enum.each(0..9, fn i ->
      spawn(fn -> skynet_node(offset + i * child_size, child_size, self_pid) end)
    end)
    sum = collect(10, 0)
    send(parent, sum)
  end

  defp seq_sum(_offset, 0, acc), do: acc
  defp seq_sum(offset, n, acc), do: seq_sum(offset, n - 1, acc + offset + n - 1)

  defp collect(0, acc), do: acc
  defp collect(n, acc) do
    receive do
      value -> collect(n - 1, acc + value)
    end
  end

  def run do
    num_leaves = get_leaves()
    total = total_actors(num_leaves)

    IO.puts("=== Elixir Skynet Benchmark ===")
    IO.puts("Leaves: #{num_leaves}, concurrency units: #{total} (sequential below #{@seq_threshold})")
    IO.puts("Using Erlang/OTP processes\n")

    caller = self()
    start = :erlang.monotonic_time(:nanosecond)
    spawn(fn -> skynet_node(0, num_leaves, caller) end)
    sum = receive do
      value -> value
    after
      120_000 -> :timeout
    end
    finish = :erlang.monotonic_time(:nanosecond)

    elapsed_ns = finish - start
    elapsed_us = div(elapsed_ns, 1000)

    IO.puts("Sum: #{sum}")
    if elapsed_us > 0 do
      ns_per_msg   = div(elapsed_ns, total)
      throughput_m = div(total, elapsed_us)
      leftover     = total - (throughput_m * elapsed_us)
      frac         = div(leftover * 100, elapsed_us)
      frac_str     = if frac < 10, do: "0#{frac}", else: "#{frac}"
      IO.puts("ns/msg:         #{ns_per_msg}")
      IO.puts("Throughput:     #{throughput_m}.#{frac_str} M msg/sec")
    end
  end
end

Skynet.run()
