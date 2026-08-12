// Pony Skynet Benchmark
// Based on https://github.com/atemerev/skynet
// Same threshold as every other language here; see FAIRNESS.md.

use "time"

primitive Leaf
  fun threshold(): I64 => 1000

  fun sum(offset: I64, size: I64): I64 =>
    var acc: I64 = 0
    var i: I64 = 0
    while i < size do
      acc = acc + offset + i
      i = i + 1
    end
    acc

actor SkynetNode
  let _parent: (SkynetNode | Main)
  var _pending: I64 = 0
  var _total: I64 = 0

  new create(parent: (SkynetNode | Main)) =>
    _parent = parent

  be compute(offset: I64, size: I64) =>
    if size <= Leaf.threshold() then
      _report(Leaf.sum(offset, size))
    else
      let child_size = size / 10
      _pending = 10
      var i: I64 = 0
      while i < 10 do
        let child = SkynetNode(this)
        child.compute(offset + (i * child_size), child_size)
        i = i + 1
      end
    end

  be result(value: I64) =>
    _total = _total + value
    _pending = _pending - 1
    if _pending == 0 then
      _report(_total)
    end

  fun ref _report(value: I64) =>
    match _parent
    | let p: SkynetNode => p.result(value)
    | let m: Main => m.done(value)
    end

actor Main
  let _env: Env
  var _start: U64 = 0
  var _leaves: I64 = 1_000_000

  new create(env: Env) =>
    _env = env

    for v in env.vars.values() do
      if v.contains("BENCHMARK_MESSAGES=") then
        try
          let parts = v.split("=")
          let val_str = parts(1)?
          _leaves = val_str.i64()?
        end
      end
    end

    var units: I64 = 1
    var n = _leaves
    while n > Leaf.threshold() do
      units = units + (n / Leaf.threshold())
      n = n / 10
    end

    _env.out.print("=== Pony Skynet Benchmark ===")
    _env.out.print("Leaves: " + _leaves.string() + ", concurrency units: " + units.string()
      + " (sequential below " + Leaf.threshold().string() + ")\n")

    _start = Time.nanos()
    let root = SkynetNode(this)
    root.compute(0, _leaves)

  be done(total: I64) =>
    let finish = Time.nanos()
    var units: I64 = 1
    var n = _leaves
    while n > Leaf.threshold() do
      units = units + (n / Leaf.threshold())
      n = n / 10
    end

    _env.out.print("Sum: " + total.string())
    let elapsed_ns = finish - _start
    let ns_per_msg = elapsed_ns.f64() / units.f64()
    let throughput = (units.f64() / (elapsed_ns.f64() / 1_000_000_000.0)) / 1_000_000.0

    _env.out.print("ns/msg:         " + ns_per_msg.string())
    _env.out.print("Throughput:     " + throughput.string() + " M msg/sec")
