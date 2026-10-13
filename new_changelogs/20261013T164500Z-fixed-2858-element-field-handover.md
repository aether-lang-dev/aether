- **An array element's string field handed to a function that frees or
  keeps it is no longer freed twice.** The hand-over of a field to such a
  function (#2566, #2843) reached the struct again through a variable, a
  pointer or a statement temporary, and stopped at an element:
  `eat(arr[0].name, 3)` freed the string and the array's scope exit freed
  it again (heap corruption), and `string.free(arr[0].name)` did the same.
  A function keeping it kept what the element's struct then freed. An
  element of a fixed-size array is now a part of its holder as a field
  is, for a local array, a struct's array field, one reached through a
  pointer or a statement temporary, and an element of a slice (#2858).
