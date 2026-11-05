- **aetherc waits for an output file another process holds (#3574).** On
  Windows a virus scanner, an indexer or an editor can hold a file it has
  just seen written without sharing it for writing, and a compile that
  wrote that path next failed with "Error opening output file:
  Permission denied": about one compile in a thousand in a sweep writing
  one output path a program after another. aetherc now waits up to about
  two seconds for such a hold to go, and a path it can never write is
  still refused at once, now by name with the Windows error.
