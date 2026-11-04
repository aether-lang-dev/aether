- **AES-CCM no longer leaks a block on every seal and open.** The CBC-MAC
  started from a zero block and stored the first step's result over it, and
  declared the AAD length prefix with an empty buffer that both branches
  replaced; neither replaced buffer was freed. Every `aes.ccm_seal` and
  `aes.ccm_open` leaked one block, and one more when there was AAD.
