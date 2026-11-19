- **A proxy cache hit is no longer freed while it is served (#3756).** The
  cache lookup found an entry under the cache lock, released the lock and
  returned the entry, and the proxy then copied its headers and body into
  the response with no lock held. A store on another worker could evict and
  free the entry in between: any two concurrent requests to a cached proxy
  could make one of them read freed memory, serving another response's bytes
  or crashing the server. An entry now carries a reference count: the cache
  holds one, the request serving a hit holds another until its copy is done,
  and the last one dropped frees it. In `method_url_vary` mode an evicted or
  expired entry was also unlinked from the wrong bucket, since its bucket
  was recomputed from the hash of the key with its Vary part, so the freed
  entry stayed in its chain for the next lookup of that URL to walk. An
  entry now records the bucket it was put in.
