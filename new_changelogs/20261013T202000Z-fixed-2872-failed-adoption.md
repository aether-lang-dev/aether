- **A string a container fails to adopt is still freed by a caught panic.**
  The adoption helpers took the string out of the panic journal before
  calling the runtime, so when the adoption failed (an index out of range,
  a null list or map) and stored nothing, the string had no owner left and
  a caught panic leaked it. They now take it out only once the container
  has it (#2872).
