# LangArena packed-float follow-up (#1986)

The original [issue](https://github.com/aether-lang-dev/aether/issues/1986)
predates the inlined packed-array accessors (#2161) and typed array views
(#2041). Re-run the actual port before attributing a remaining gap to array
handle types or vectorization. This driver selects the seven affected
workloads from LangArena's existing configurations, checks the small inputs,
and measures repeated production runs against Go on the **same machine**.
It alternates language order between repetitions, validates every checksum,
and preserves raw output, input configurations, individual samples, source
revisions, binary hashes, and host metadata. Timings are observations, never
test gates. Python 3, Go, and an Aether build are required.

[September 28 results and runtime fixes](2026-09-28-results.md).

## Build and measure

Use the `add-aether-benchmarks` branch of
[`aether-lang-dev/LangArena`](https://github.com/aether-lang-dev/LangArena/tree/add-aether-benchmarks).
Keep its working tree clean for reproducibility. From the Aether repository:

```sh
make -j4 compiler ae stdlib
export AETHER_HOME="$PWD"
arena=/path/to/LangArena
work=$(mktemp -d)
# A scratch copy keeps build products out of the source checkout.
cp -R "$arena/aether" "$arena/golang" "$work/"
(cd "$work/aether" && "$AETHER_HOME/build/ae" build src/main.ae -o "$work/bench-aether")
(cd "$work/golang" && go build -buildvcs=false -o "$work/bench-go" .)
python3 benchmarks/langarena/run.py \
    --langarena "$arena" --aether "$work/bench-aether" --go "$work/bench-go" \
    --output "$work/results" --repeats 5
```

The output directory must be new. Aether uses its default `-O2` build;
the project's `aether.toml` supplies its C helper. Record compiler versions
and any environment overrides alongside the results. The binaries must be
built from the supplied checkout: their hashes identify the artifacts but
cannot establish source provenance automatically. Avoid other CPU-heavy work
while measuring. The driver uses LangArena's internal elapsed times, including
its existing warmup rules; it does not include compilation or process startup.

The Aether port currently implements T4/T8/T16 sequentially. Their ratios
therefore measure a port limitation. NeuralNet also uses a different graph
representation: it scans all synapses to find a neuron's incoming/outgoing
edges. Integer-keyed maps and Distance::NGram are a separate follow-up, outside
this packed-float measurement.

## Actor-parallel matmul example

[`examples/actors/matmul.ae`](../../examples/actors/matmul.ae) demonstrates a
reusable pool of worker actors with typed messages and `float[]` views:

```sh
./build/ae build examples/actors/matmul.ae -o /tmp/actor-matmul
/tmp/actor-matmul 900 4 5
/tmp/actor-matmul 900 8 5
/tmp/actor-matmul 900 16 5
```

Arguments are dimension, worker actor count, and repetitions. The runtime
chooses the scheduler thread count. Workers share immutable input buffers
and write disjoint output rows. `wait_for_idle()` completes the batch before
main reads or reuses the output or frees any borrowed buffer. Every cell is
checked against an independent serial implementation using the untransposed
input, including uneven partitions and cases with more workers than rows.

The printed time includes dispatch, multiplication, and the completion
barrier; allocation, transpose, pool construction, and validation are outside
it. This is a concurrency example, **not a replacement LangArena timing**:
LangArena includes transpose and allocation in its matmul run. No speedup is
asserted; available cores, scheduling, matrix size, and machine load matter.
