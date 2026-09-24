- **`std.hash.crc32` and `crc32_update`.** CRC-32 (IEEE 802.3, the
  reflected `0xEDB88320` polynomial) is the checksum on PNG chunks, gzip
  members and ZIP entries. `crc32_update` continues a checksum across pieces
  of input. It was only available privately inside `std.zip`; `contrib.png`
  is its first caller. The tests use the catalogue check value
  (`"123456789"` → `0xcbf43926`) and zlib's results.
