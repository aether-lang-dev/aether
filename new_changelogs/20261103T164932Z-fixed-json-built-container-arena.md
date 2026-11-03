- **A built std.json container no longer takes a 16 KB arena.** Every
  `json.obj`/`json.arr` that gets a member roots an arena of its own, and
  that arena's first chunk was the fixed 16 KB a parsed document gets, so a
  program that builds many small objects (RexxAE's parsed program, values
  and frames) made one 16 KB allocation per object: about 23,000 of them in
  one run of RexxAE's ESP32-S3 lane, every one past ESP-IDF's
  internal-RAM threshold and into PSRAM. A built container's arena now
  starts at 256 bytes and a parsed document's at about four bytes per input
  byte (256 bytes to 16 KB), both doubling as before. RexxAE's peak heap on
  the ESP32-S3 fell from about 194 KB (76 KB internal, 118 KB PSRAM) to
  87 KB, all internal RAM.
