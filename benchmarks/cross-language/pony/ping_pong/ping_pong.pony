// Pony Ping-Pong Benchmark (Savina-style)
// Two actors exchange messages back and forth.

use "time"

actor PongActor
  var _ping: (PingActor | None) = None

  be set_ping(p: PingActor) =>
    _ping = p

  be ping(value: U64) =>
    match _ping
    | let p: PingActor => p.pong(value)
    end

actor PingActor
  let _main: Main
  let _pong: PongActor
  let _target: U64
  var _received: U64 = 0
  var _errors: U64 = 0

  new create(main: Main, pong_actor: PongActor, target: U64) =>
    _main = main
    _pong = pong_actor
    _target = target

  be start() =>
    _pong.ping(0)

  be pong(value: U64) =>
    if value != _received then
      _errors = _errors + 1
    end
    _received = _received + 1
    if _received < _target then
      _pong.ping(_received)
    else
      _main.done(_received, _errors)
    end

actor Main
  let _env: Env
  var _start: U64 = 0
  var _messages: U64 = 100_000

  new create(env: Env) =>
    _env = env

    for v in env.vars.values() do
      if v.contains("BENCHMARK_MESSAGES=") then
        try
          let parts = v.split("=")
          let val_str = parts(1)?
          _messages = val_str.u64()?
        end
      end
    end

    _env.out.print("=== Pony Ping-Pong Benchmark ===")
    _env.out.print("Messages: " + _messages.string() + "\n")
    _start = Time.nanos()

    let pong = PongActor
    let ping = PingActor(this, pong, _messages)
    pong.set_ping(ping)
    ping.start()

  be done(received: U64, errors: U64) =>
    let finish = Time.nanos()
    if errors > 0 then
      _env.out.print("VALIDATION FAILED: " + errors.string() + " errors")
    end
    let elapsed_ns = finish - _start
    let ns_per_msg = elapsed_ns.f64() / received.f64()
    let throughput = 1_000_000_000.0 / ns_per_msg

    _env.out.print("ns/msg:         " + ns_per_msg.string())
    _env.out.print("Throughput:     " + (throughput / 1_000_000.0).string() + " M msg/sec")
