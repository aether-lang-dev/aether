# Aether Standard Library Guide

The standard library is documented in one place: the
[Standard Library Reference](stdlib-reference.md). It opens with an index of
every module, generated from the source and checked by `make check-docs`, and
its examples are compiled and run by the same check. This page is a way into
it by task.

Start with [Using the Standard Library](stdlib-reference.md#using-the-standard-library)
for the three conventions every module follows: errors come back as values,
the C layer is exported alongside the wrappers, and whatever you create, you
release. Then see [Platform support](stdlib-reference.md#platform-support)
for what changes on Windows, WASI and the browser.

## By task

| To... | Go to |
|---|---|
| Hold values in a list, map, set or queue | [Collections](stdlib-reference.md#collections) |
| Build, split, search or format text | [Strings](stdlib-reference.md#strings-stdstring), [Regular expressions](stdlib-reference.md#regular-expressions-stdregex) |
| Read and write files, walk directories, join paths | [File System](stdlib-reference.md#file-system) |
| Print, read standard input, set environment variables | [I/O](stdlib-reference.md#io-stdio) |
| Parse or produce JSON | [JSON](stdlib-reference.md#json-stdjson) |
| Use YAML, XML, MessagePack, CBOR or CSV | [Other structured-data formats](stdlib-reference.md#other-structured-data-formats) |
| Hash, sign, encrypt, or encode Base64 | [Cryptography](stdlib-reference.md#cryptography-stdcryptography), [Encodings](stdlib-reference.md#encodings-stdencoding) |
| Compress data | [Compression](stdlib-reference.md#compression-stdzlib) |
| Make HTTP requests or serve HTTP | [HTTP](stdlib-reference.md#http-stdhttp) |
| Open TCP connections | [TCP](stdlib-reference.md#tcp-stdtcp) |
| Run another program | [Running programs](stdlib-reference.md#running-programs) |
| Read command-line arguments | [Command-line arguments](stdlib-reference.md#command-line-arguments-stdclapae) |
| Tell the time, or measure elapsed time | [Dates and times](stdlib-reference.md#dates-and-times-stdtime) |
| Log | [Logging](stdlib-reference.md#logging-stdlog) |
| Do arithmetic beyond the operators | [Math](stdlib-reference.md#math-stdmath) |
| Generate unique ids | [Identifiers](stdlib-reference.md#identifiers-stduuid) |
| Localise messages and plurals | [Internationalisation](stdlib-reference.md#internationalisation) |
| Run work concurrently | [Concurrency](stdlib-reference.md#concurrency) |
| Control allocation, or read raw memory | [Memory](stdlib-reference.md#memory) |
| Confine what a program can reach | [Sandboxing](stdlib-reference.md#sandboxing) |
| Run as a library inside another application | [Embedding in a host](stdlib-reference.md#embedding-in-a-host-stdhost) |

A module this table does not name is in the reference's index, which links
each one to its guide and source.
