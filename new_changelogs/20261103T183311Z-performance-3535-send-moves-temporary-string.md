- **Sending a message with a string built for the send no longer copies the
  string (#3535).** A string field's value was always copied into the
  message, and a temporary made for the send (`a ! Log { text: "${n}" }`,
  a call's result) was freed right after the copy. A temporary nothing else
  holds is now moved into the message, as the ownership model says a dying
  temporary is; a string the sender keeps is still copied. A send of a
  freshly built string field takes about 27% less time (154 to 112 ns per
  message on the core send benchmark).
