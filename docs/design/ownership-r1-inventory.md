# R1 inventory: what every std string producer returns

Stage 2 of [the ownership model](ownership-model.md) makes every `string` the program holds a counted `AetherString`. This is the list it starts from: every `extern` in `std/` that returns a `string`, or a tuple with a `string` slot, with what its C function hands back on every path, read from the source.

## Classes

| class | what the C returns | who owns it |
|---|---|---|
| A | a fresh counted `AetherString` | the caller: one reference handed over |
| A~ | a counted `AetherString` another holder keeps (a list's element) | the holder; the caller borrows |
| B | a plain `malloc`'d (or `aether_caps_malloc`'d) `char*` | the caller, but nothing counts it |
| C | a pointer into memory C keeps: a thread-local buffer, an arena, a struct's field, an argument's characters | C; the caller borrows |
| D | a C string literal | nobody; it is static |

`C/D` is a function that returns one or the other by path (a field, or `""` when it is absent): borrowed on every path. A tuple lists its slots in order, `-` for a slot that is not a string. A NULL return is not a class: the class is that of the other paths.

## What R1 does with each

- **A** is what R1 wants. Declared `@heap`, a call is owned. Declared without it, the compiler treats it as owned only because its name is on a list in the code generator (`is_heap_string_expr`); R1 declares it `@heap` and the list goes.
- **B** is the shape R1 removes: the runtime cannot release a plain buffer (`string_release` leaves anything without the header alone). Each such C function returns a counted string instead, and is declared `@heap`.
- **A~, C, D and C/D** are borrowed, the default for an extern. A place that keeps one takes it (R3): a counted string is retained, anything else copied into a counted one, once.
- A stub compiled when a feature is not built in (no networking, no filesystem) returns what the real function returns: `""` from a `@heap` extern's stub is a literal its caller would release, so R1 returns a counted empty string there.

## Totals

259 declarations of 215 C functions (some are declared by more than one module). String slots by class:

| class | slots |
|---|---|
| A | 50 |
| A~ | 3 |
| B | 55 |
| C | 20 |
| D | 74 |
| C/D | 65 |
| UNKNOWN | 2 |

Declarations by what R1 does to them:

| R1 | declarations |
|---|---|
| borrowed | 153 |
| return counted; `@heap` | 28 |
| return counted | 26 |
| add `@heap` | 25 |
| owned | 25 |
| borrowed (see notes) | 2 |

## By module

### std.audio

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_audio_last_error` |  | borrowed | D | borrowed |

### std.audit

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_audit_entry_category` |  | borrowed | C/D | borrowed |
| `aether_audit_entry_resource` |  | borrowed | C/D | borrowed |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.brotli

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `brotli_get_compress_bytes` |  | borrowed | C/D | borrowed |
| `brotli_get_stream_bytes` |  | borrowed | C/D | borrowed |

### std.bytes

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_bytes_finish` |  | `@heap` | A | owned |
| `aether_bytes_string_from_ptr` |  | `@heap` | A | owned |
| `aether_bytes_to_string` |  | `@heap` | A | owned |

### std.cas

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `os_getenv` |  | borrowed | B | return counted; `@heap` |

### std.casper

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_casper_pwd_home` |  | `@heap` | B | return counted |
| `aether_casper_resolve` |  | `@heap` | B | return counted |
| `aether_casper_sysctl_str` |  | `@heap` | B | return counted |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.collections

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `string_list_get` |  | borrowed | A~ | borrowed |

### std.config

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_config_store_get` |  | borrowed | C/D | borrowed |
| `aether_config_store_get_or` |  | borrowed | C/D | borrowed |

### std.cryptography

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `cryptography_digest_final_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_get_binary_digest` |  | borrowed | C/D | borrowed |
| `cryptography_get_random_bytes` |  | borrowed | C/D | borrowed |
| `cryptography_hash_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_hmac_sha256_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_md4_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_md5_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_sha1_hex_raw` |  | `@heap` | B | return counted |
| `cryptography_sha256_hex_raw` |  | `@heap` | B | return counted |

### std.cryptography.tls13_client

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_tls_windows_bundle` |  | `@heap` | B | return counted |
| `aether_tls_windows_store_pem` |  | `@heap` | B | return counted |

### std.dir

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `dir_list_get` |  | borrowed | C | borrowed |

### std.dl

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_dl_last_error_raw` |  | borrowed | C | borrowed |

### std.encoding

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `cryptography_base64_encode_padded_raw` |  | borrowed | B | return counted; `@heap` |
| `cryptography_base64_encode_raw` |  | borrowed | B | return counted; `@heap` |
| `cryptography_base64_decode_owned` |  | borrowed | B | return counted; `@heap` |
| `cryptography_base64url_decode_owned` |  | borrowed | B | return counted; `@heap` |
| `cryptography_base64url_encode_padded_raw` |  | borrowed | B | return counted; `@heap` |
| `cryptography_base64url_encode_raw` |  | borrowed | B | return counted; `@heap` |

### std.file

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `file_read_all_raw` |  | borrowed | A | add `@heap` |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.fs

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `dir_list_get` |  | borrowed | C | borrowed |
| `file_read_all_raw` |  | borrowed | A | add `@heap` |
| `fs_chmod_raw` |  | borrowed | (int, int, D) | borrowed |
| `fs_copy_raw` |  | borrowed | (int, int, D) | borrowed |
| `fs_error_message` |  | borrowed | C | borrowed |
| `fs_fsync_raw` |  | borrowed | D | borrowed |
| `fs_ftruncate_raw` |  | borrowed | D | borrowed |
| `fs_get_block_transport` |  | borrowed | C | borrowed |
| `fs_get_mount_fstype` |  | borrowed | C/D | borrowed |
| `fs_get_mount_options` |  | borrowed | C/D | borrowed |
| `fs_get_mount_point` |  | borrowed | C/D | borrowed |
| `fs_get_mount_source` |  | borrowed | C/D | borrowed |
| `fs_get_pread` |  | borrowed | C/D | borrowed |
| `fs_get_read_binary` |  | borrowed | C | borrowed |
| `fs_make_temp_dir_raw` |  | `@heap` | B | return counted |
| `fs_make_temp_file_raw` |  | `@heap` | B | return counted |
| `fs_move_raw` |  | borrowed | (int, int, D) | borrowed |
| `fs_read_binary_tuple` |  | `@heap` | (A, int, C/D) | owned |
| `fs_readlink_raw` |  | `@heap` | B | return counted |
| `fs_realpath_raw` |  | `@heap` | (B, int, D) | return counted |
| `os_temp_dir_raw` |  | `@heap` | B | return counted |
| `path_basename` |  | borrowed | B | return counted; `@heap` |
| `path_clean` |  | borrowed | B | return counted; `@heap` |
| `path_dirname` |  | borrowed | B | return counted; `@heap` |
| `path_extension` |  | borrowed | B | return counted; `@heap` |
| `path_join` |  | borrowed | B | return counted; `@heap` |
| `path_rel` |  | borrowed | B | return counted; `@heap` |
| `path_separator` |  | borrowed | D | borrowed |
| `string_concat` |  | borrowed | A | add `@heap` |
| `string_substring` |  | borrowed | A | add `@heap` |

### std.host

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_caller_attribute` |  | borrowed | C/D | borrowed |
| `aether_caller_identity` |  | borrowed | C/D | borrowed |

### std.http

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `http_client_pool_configure_raw` |  | borrowed | D | borrowed |
| `http_get_header` |  | borrowed | C | borrowed |
| `http_get_path_param` |  | borrowed | C | borrowed |
| `http_get_query_param` |  | borrowed | C | borrowed |
| `http_get_request_body_read` |  | borrowed | C/D | borrowed |
| `http_mime_type` |  | borrowed | D | borrowed |
| `http_request_body` |  | borrowed | C/D | borrowed |
| `http_request_header_name` |  | borrowed | C/D | borrowed |
| `http_request_header_value` |  | borrowed | C/D | borrowed |
| `http_request_http_version` |  | borrowed | C/D | borrowed |
| `http_request_local_addr` |  | borrowed | C/D | borrowed |
| `http_request_method` |  | borrowed | C/D | borrowed |
| `http_request_path` |  | borrowed | C/D | borrowed |
| `http_request_query` |  | borrowed | C/D | borrowed |
| `http_request_remote_addr` |  | borrowed | C/D | borrowed |
| `http_request_scheme` |  | borrowed | D | borrowed |
| `http_response_body` |  | `@heap` | A | owned |
| `http_response_body_str` |  | borrowed | C/D | borrowed |
| `http_response_error` |  | borrowed | C/D | borrowed |
| `http_response_headers` |  | borrowed | C/D | borrowed |
| `http_response_headers_str` |  | borrowed | C/D | borrowed |
| `http_server_set_access_log_raw` |  | borrowed | D | borrowed |
| `http_server_set_h2_concurrent_dispatch_raw` |  | borrowed | D | borrowed |
| `http_server_set_h2_raw` |  | borrowed | D | borrowed |
| `http_server_set_health_probes_raw` |  | borrowed | D | borrowed |
| `http_server_set_keepalive_raw` |  | borrowed | D | borrowed |
| `http_server_set_metrics_raw` |  | borrowed | D | borrowed |
| `http_server_set_tls_raw` |  | borrowed | D | borrowed |
| `http_server_shutdown_graceful_raw` |  | borrowed | D | borrowed |
| `http_ws_message_data` |  | borrowed | C/D | borrowed |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.http.client

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `http_response_body` |  | `@heap` | A | owned |
| `http_response_effective_url_raw` |  | borrowed | C/D | borrowed |
| `http_response_error` |  | borrowed | C/D | borrowed |
| `http_response_header_raw` |  | borrowed | C/D | borrowed |
| `http_response_headers` |  | borrowed | C/D | borrowed |
| `http_response_read_chunk_raw` |  | `@heap` | A | owned |
| `http_response_redirect_error_raw` |  | borrowed | C/D | borrowed |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.http.middleware

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `string_substring` |  | borrowed | A | add `@heap` |

### std.http.proxy

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_proxy_breaker_configure` |  | borrowed | D | borrowed |
| `aether_proxy_health_checks_enable` |  | borrowed | D | borrowed |
| `aether_proxy_opts_bind_cache` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_body_cap` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_preserve_host` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_retry_policy` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_strip_prefix` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_trace_inject` |  | borrowed | D | borrowed |
| `aether_proxy_opts_set_xforwarded` |  | borrowed | D | borrowed |
| `aether_proxy_pool_metrics_text` |  | `@heap` | B | return counted |
| `aether_proxy_pool_set_cookie_name` |  | borrowed | D | borrowed |
| `aether_proxy_rate_limit_set` |  | borrowed | D | borrowed |
| `aether_proxy_upstream_add` |  | borrowed | D | borrowed |
| `aether_proxy_upstream_drain` |  | borrowed | D | borrowed |
| `aether_proxy_upstream_remove` |  | borrowed | D | borrowed |
| `aether_proxy_upstream_undrain` |  | borrowed | D | borrowed |
| `aether_proxy_use_reverse_proxy` |  | borrowed | D | borrowed |
| `aether_proxy_use_reverse_proxy_match` |  | borrowed | D | borrowed |
| `aether_proxy_use_reverse_proxy_methods` |  | borrowed | D | borrowed |
| `aether_proxy_use_simple_proxy` |  | borrowed | D | borrowed |

### std.http.script_gateway

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `script_gateway_mount_so_raw` |  | borrowed | (-, -, C/D) | borrowed |

### std.io

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `io_errno_message_raw` |  | borrowed | B | return counted; `@heap` |
| `io_fd_close_raw` |  | borrowed | D | borrowed |
| `io_fd_open_read_tuple` |  | borrowed | (D) | borrowed |
| `io_fd_open_write_tuple` |  | borrowed | (D) | borrowed |
| `io_fd_read_line_tuple` |  | `@heap` | (A, D) | owned |
| `io_fd_read_n_tuple` |  | `@heap` | (A, D) | owned |
| `io_getenv` |  | borrowed | B | return counted; `@heap` |
| `io_read_file_raw` |  | borrowed | B | return counted; `@heap` |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.json

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `json_get_string_raw` |  | borrowed | C | borrowed |
| `json_last_error` |  | borrowed | C/D | borrowed |
| `json_object_key_at` |  | borrowed | C | borrowed |
| `json_stringify_raw` |  | borrowed | B | return counted; `@heap` |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.lzf

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `lzf_get_compress_bytes` |  | borrowed | C/D | borrowed |
| `lzf_get_decompress_bytes` |  | borrowed | C/D | borrowed |

### std.map

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `map_keys_get_raw` |  | borrowed | A~ | borrowed |

### std.net

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `http_get_header` |  | borrowed | C | borrowed |
| `http_get_path_param` |  | borrowed | C | borrowed |
| `http_get_query_param` |  | borrowed | C | borrowed |
| `http_mime_type` |  | borrowed | D | borrowed |
| `http_request_body` |  | borrowed | C/D | borrowed |
| `http_request_method` |  | borrowed | C/D | borrowed |
| `http_request_path` |  | borrowed | C/D | borrowed |
| `http_request_query` |  | borrowed | C/D | borrowed |
| `http_response_body` |  | `@heap` | A | owned |
| `http_response_body_str` |  | borrowed | C/D | borrowed |
| `http_response_error` |  | borrowed | C/D | borrowed |
| `http_response_headers` |  | borrowed | C/D | borrowed |
| `http_response_headers_str` |  | borrowed | C/D | borrowed |
| `tcp_receive_n_raw` |  | `@heap` | (A, -, D) | owned |
| `tcp_receive_raw` |  | `@heap` | B | return counted |

### std.os

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_args_get` |  | borrowed | C | borrowed |
| `aether_argv0` |  | borrowed | C | borrowed |
| `os_arch_raw` |  | `@heap` | B | return counted |
| `os_exec_raw` |  | borrowed | B | return counted; `@heap` |
| `os_getcwd_raw` |  | borrowed | B | return counted; `@heap` |
| `os_getenv` |  | borrowed | B | return counted; `@heap` |
| `os_now_local_iso8601_raw` |  | borrowed | B | return counted; `@heap` |
| `os_now_utc_iso8601_raw` |  | borrowed | B | return counted; `@heap` |
| `os_platform_raw` |  | `@heap` | B | return counted |
| `os_run_capture_raw` |  | borrowed | B | return counted; `@heap` |
| `os_run_capture_status_raw` |  | `@heap` | (B, D) | return counted |
| `os_run_full_raw` |  | `@heap` | (B, B, D) | return counted |
| `os_run_pipe_drain_and_wait_raw` |  | `@heap` | (B, D) | return counted |
| `os_run_pipe_raw` |  | borrowed | (D) | borrowed |
| `os_run_supervised_raw` |  | borrowed | (D) | borrowed |
| `os_spawn_raw` |  | borrowed | (D) | borrowed |
| `os_spawn_to_raw` |  | borrowed | (D) | borrowed |
| `os_temp_dir_raw` |  | `@heap` | B | return counted |
| `os_wait_any_raw` |  | borrowed | (D) | borrowed |
| `os_wait_any_timeout_raw` |  | borrowed | (D) | borrowed |
| `os_wait_pid_raw` |  | borrowed | (D) | borrowed |
| `os_wait_pid_timeout_raw` |  | borrowed | (D) | borrowed |
| `os_wait_raw` |  | borrowed | (D) | borrowed |
| `os_which` |  | borrowed | B | return counted; `@heap` |
| `string_concat` |  | borrowed | A | add `@heap` |

### std.path

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `path_basename` |  | borrowed | B | return counted; `@heap` |
| `path_clean` |  | borrowed | B | return counted; `@heap` |
| `path_dirname` |  | borrowed | B | return counted; `@heap` |
| `path_extension` |  | borrowed | B | return counted; `@heap` |
| `path_join` |  | borrowed | B | return counted; `@heap` |
| `path_rel` |  | borrowed | B | return counted; `@heap` |
| `path_separator` |  | borrowed | D | borrowed |

### std.quote

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_quote_capture_base` |  | borrowed | D | borrowed |
| `aether_quote_capture_name` |  | borrowed | D | borrowed |
| `aether_quote_capture_type` |  | borrowed | D | borrowed |
| `aether_quote_lit` |  | borrowed | D | borrowed |
| `aether_quote_name` |  | borrowed | D | borrowed |
| `aether_quote_op` |  | borrowed | D | borrowed |
| `aether_quote_param_name` |  | borrowed | D | borrowed |
| `aether_quote_param_type` |  | borrowed | D | borrowed |
| `aether_quote_read_string` |  | borrowed | UNKNOWN | borrowed (see notes) |
| `aether_quote_source` |  | borrowed | D | borrowed |
| `aether_quote_type` |  | borrowed | D | borrowed |

### std.reflect

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_schema_attr_name` |  | borrowed | D | borrowed |
| `aether_schema_attr_text` |  | borrowed | D | borrowed |
| `aether_schema_field_name` |  | borrowed | D | borrowed |
| `aether_schema_field_type` |  | borrowed | D | borrowed |
| `aether_schema_name` |  | borrowed | D | borrowed |

### std.regex

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_regex_captures_get` |  | `@heap` | A | owned |
| `aether_regex_last_error` |  | borrowed | C | borrowed |
| `aether_regex_replace` |  | `@heap` | A | owned |
| `aether_regex_replace_all` |  | `@heap` | A | owned |

### std.set

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `map_keys_get_raw` |  | borrowed | A~ | borrowed |

### std.strarr

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `strarr_data` |  | borrowed | C | borrowed |
| `strarr_get_raw` |  | borrowed | A~, C/D | borrowed |

### std.strbuilder

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_strbuilder_finish` |  | `@heap` | A | owned |

### std.string

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `string_concat` |  | borrowed | A | add `@heap` |
| `string_concat_wrapped` |  | borrowed | A | add `@heap` |
| `string_from_double` |  | `@heap` | A | owned |
| `string_join` |  | `@heap` | A | owned |
| `string_replace` |  | borrowed | A | add `@heap` |
| `string_replace_all` |  | borrowed | A | add `@heap` |
| `string_seq_head` |  | borrowed | A~, C/D | borrowed |
| `string_seq_join` |  | borrowed | A | add `@heap` |
| `string_substring` |  | borrowed | A | add `@heap` |
| `string_substring_n` |  | borrowed | A | add `@heap` |
| `string_to_cstr` |  | borrowed | C/D | borrowed |
| `string_to_lower` |  | borrowed | A | add `@heap` |
| `string_to_upper` |  | borrowed | A | add `@heap` |
| `string_trim` |  | borrowed | A | add `@heap` |

### std.tcp

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `tcp_receive_n_raw` |  | `@heap` | (A, -, D) | owned |
| `tcp_receive_raw` |  | `@heap` | B | return counted |

### std.time.calendar

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_bytes_finish` |  | `@heap` | A | owned |

### std.udp

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `udp_addr_host_raw` |  | `@heap` | A | owned |
| `udp_recv_from_raw` |  | borrowed | (-, -, D) | borrowed |

### std.unicode

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `aether_unicode_casefold` |  | `@heap` | A | owned |
| `aether_unicode_fold_accents` |  | `@heap` | A | owned |
| `aether_unicode_grapheme_substring` |  | `@heap` | A | owned |
| `aether_unicode_nfc` |  | `@heap` | A | owned |
| `aether_unicode_nfd` |  | `@heap` | A | owned |

### std.xml

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `string_concat` |  | borrowed | A | add `@heap` |
| `xml_builder_finish` |  | borrowed | B | return counted; `@heap` |
| `xml_escape` |  | borrowed | B | return counted; `@heap` |
| `xml_event_attr_name` |  | borrowed | C/D | borrowed |
| `xml_event_attr_str` |  | borrowed | C/D | borrowed |
| `xml_event_attr_value` |  | borrowed | C/D | borrowed |
| `xml_event_name` |  | borrowed | C/D | borrowed |
| `xml_event_text` |  | borrowed | C/D | borrowed |
| `xml_parser_error` |  | borrowed | C/D | borrowed |

### std.yaml

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `string_concat` |  | borrowed | A | add `@heap` |
| `yaml_emit_document_raw` |  | borrowed | C | borrowed |
| `yaml_emit_node_raw` |  | borrowed | C | borrowed |
| `yaml_last_error` |  | borrowed | C/D | borrowed |
| `yaml_node_get_scalar_raw` |  | borrowed | UNKNOWN | borrowed (see notes) |

### std.zlib

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `zlib_get_deflate_bytes` |  | borrowed | C/D | borrowed |
| `zlib_get_inflate_bytes` |  | borrowed | C/D | borrowed |
| `zlib_get_stream_bytes` |  | borrowed | C/D | borrowed |

### std.zstd

| extern | C symbol | declared | returns | R1 |
|---|---|---|---|---|
| `zstd_get_compress_bytes` |  | borrowed | C/D | borrowed |
| `zstd_get_stream_bytes` |  | borrowed | C/D | borrowed |

## Notes

- `string_seq_head` and `strarr_get_raw` return what the sequence or array holds: a counted string they keep a reference to (A~), or a plain pointer the caller stored (C/D). Under R1 a store into either takes the value, so both hold counted strings only and return A~.
- `aether_schema_attr_text` returns the attribute literal the compiler wrote: a C literal, or the static pinned `AetherString` of a literal holding a NUL (#2520). Under R1 every literal is pinned.
- `aether_quote_read_string` reads a captured variable's current value: borrowed, of whatever shape the variable holds.
- `yaml_node_get_scalar_raw` returns libfyaml's `fy_node_get_scalar0`, which is not vendored here; it is borrowed (the document's), as the module's own code treats it.
- `fs_error_message` may return its own `fallback` argument, and `aether_config_store_get_or` its `default_value`: borrowed either way.
- `http_response_body` hands back a retained reference to the response's body on one path and a fresh string on the others: A on every path. Its stub for builds without networking returns `""`, as do `http_response_read_chunk_raw`'s, and `fs_realpath_raw`'s without the filesystem; R1 makes them counted.
- The tables list the externs declared `-> string` when the inventory was taken. #3018 found the externs std declared `-> ptr` though their C returns a string, and typed them `string`, with the same C function: class A, now `-> string @heap`, are `string_new`, `string_from_cstr`, `string_from_literal`, `string_empty`, `string_new_with_length` (std.string, and the copies in std.brotli, std.cryptography, std.encoding, std.fs, std.http, std.lzf, std.zlib, std.zstd, contrib/avcodec and contrib/sqlite), `string_from_char`, `string_from_int`, `string_from_long`, `string_from_float`, `string_from_int_radix`, `string_pad_start`, `string_pad_end`, `string_format_list`, `aether_tar_slice_string` and `aether_tar_join_name`; `string_array_get` is class C (the array's characters), now `-> string`, borrowed. The `-> ptr` externs left are not strings: the byte buffers the compression and crypto modules hand to `string_new_with_length`, `aether_string_raw_ptr`, `string.keep`, `string_seq_to_array`, and `aether_sandbox_grant_pattern`, a C string kept in a C list.
