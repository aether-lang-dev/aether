- **Qualified enum members now retain their lexical prefix.** `hide Color`
  blocks `Color.Red`, while `seal except Color` permits it; containment no
  longer checks the generated `Color_Red` constant name.
