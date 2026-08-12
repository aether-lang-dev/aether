%% Erlang Skynet Benchmark
%% Based on https://github.com/atemerev/skynet
%% Uses native Erlang processes (lightweight ~2KB each).
%% For 1M leaves, start with: erl +P 2000000 -noshell -s skynet start
-module(skynet).
-export([start/0]).

get_leaves() ->
    case os:getenv("SKYNET_LEAVES") of
        false ->
            case os:getenv("BENCHMARK_MESSAGES") of
                false -> 1000000;
                Val -> list_to_integer(Val)
            end;
        Val -> list_to_integer(Val)
    end.

%% Concurrency units actually created; also the divisor, since every language in
%% this suite creates the same count. It used to sum every level of the tree,
%% 1,111,111 for a million leaves, which is what BEAM really spawned while the
%% thread-based languages spawned 1,111 and were scored on the same divisor.
-define(SEQ_THRESHOLD, 1000).
total_actors(N) when N =< ?SEQ_THRESHOLD -> 1;
total_actors(N) -> (N div ?SEQ_THRESHOLD) + total_actors(N div 10).

%% Each node spawns 10 children or reports its offset (leaf).
%% Results bubble up via message passing.
%% Same threshold as every other language in this suite: below it the subtree is
%% summed in this process rather than spawning more.
skynet_node(Offset, Size, Parent) when Size =< ?SEQ_THRESHOLD ->
    Parent ! lists:sum([Offset + I || I <- lists:seq(0, Size - 1)]);
skynet_node(Offset, Size, Parent) ->
    ChildSize = Size div 10,
    Self = self(),
    lists:foreach(fun(I) ->
        spawn(fun() ->
            skynet_node(Offset + I * ChildSize, ChildSize, Self)
        end)
    end, lists:seq(0, 9)),
    Sum = collect(10, 0),
    Parent ! Sum.

collect(0, Acc) -> Acc;
collect(N, Acc) ->
    receive
        Value ->
            collect(N - 1, Acc + Value)
    end.

start() ->
    NumLeaves = get_leaves(),
    TotalActors = total_actors(NumLeaves),

    io:format("=== Erlang Skynet Benchmark ===~n"),
    io:format("Leaves: ~p, concurrency units: ~p (sequential below ~p)~n~n",
              [NumLeaves, TotalActors, ?SEQ_THRESHOLD]),

    Self = self(),
    Start = erlang:monotonic_time(nanosecond),
    spawn(fun() -> skynet_node(0, NumLeaves, Self) end),
    Sum = receive V -> V after 120000 -> timeout end,
    End = erlang:monotonic_time(nanosecond),

    ElapsedNs = End - Start,
    ElapsedUs = ElapsedNs div 1000,

    io:format("Sum: ~p~n", [Sum]),
    if
        ElapsedUs > 0 ->
            NsPerMsg = ElapsedNs div TotalActors,
            ThroughputM = TotalActors div ElapsedUs,
            Leftover = TotalActors - (ThroughputM * ElapsedUs),
            ThroughputFrac = (Leftover * 100) div ElapsedUs,
            io:format("ns/msg:         ~p~n", [NsPerMsg]),
            FracStr = if ThroughputFrac < 10 -> io_lib:format("0~p", [ThroughputFrac]);
                         true -> io_lib:format("~p", [ThroughputFrac])
                      end,
            io:format("Throughput:     ~p.~s M msg/sec~n", [ThroughputM, FracStr]);
        true -> ok
    end,
    halt(0).
