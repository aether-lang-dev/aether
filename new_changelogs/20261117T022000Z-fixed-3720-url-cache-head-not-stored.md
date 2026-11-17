- **Under the `url` cache key the proxy no longer stores answers to HEAD
  (#3720).** The key leaves the method out, so a stored HEAD answer, which
  has no body, was served to a GET of the same URL as an empty 200 until it
  expired. A HEAD there is answered from a stored GET instead.
