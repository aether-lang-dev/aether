- **`fs.make_temp_file` and `fs.make_temp_dir` say where and why they failed
  (#3817).** Both returned a bare "cannot create temp file" or "cannot create
  temp dir", so a refusal, a missing directory and a name that could not be
  made looked the same in a log. They now give the directory and the OS
  reason, as `fs.read` does ("C:\tmp\box/d: No such file or directory"), on
  every path: inside an enforce block, and on Windows, where
  `GetTempFileNameA`'s error now reaches `errno`.
