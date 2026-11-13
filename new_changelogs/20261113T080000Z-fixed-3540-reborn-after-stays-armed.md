- **A restarted actor's receive timeout is not lost to a racing revival
  (#3540).** A restart in place tells the actor's core of the countdown
  while the actor is still dead, then clears the death and tells it again.
  A core that read the death for the first entry, and took the entry after
  the second push had found it there and added none, went by the death it
  had read: the countdown left the core's timer heap and the `after` never
  fired. The core reads the death again once it has taken the entry.
