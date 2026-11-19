- **A certificate whose validity holds no proper time is refused (#3749).**
  `tls13_cert.parse_certificate` read the validity as any TLV and its
  notBefore and notAfter as any TLVs, dropping their errors, so a
  certificate whose validity was not a SEQUENCE, held no time, or held
  something other than a time parsed, with no notBefore or a notAfter of
  arbitrary bytes. The validity must now be a SEQUENCE of two times as RFC
  5280 writes them (UTCTime `YYMMDDHHMMSSZ` or GeneralizedTime
  `YYYYMMDDHHMMSSZ`). Found by the X.509 fuzz target.
