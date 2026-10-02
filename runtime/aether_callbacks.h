#ifndef AETHER_CALLBACKS_H
#define AETHER_CALLBACKS_H

/* @c_callback functions by name (#2297); see aether_callbacks.c.
 *
 * aether_callback_register: record `fn` under `name` (the last registration
 * of a name wins). Generated code calls it from a constructor for each
 * @c_callback function.
 * aether_callback_lookup: the function registered under `name`, or NULL. */
void aether_callback_register(const char* name, void* fn);
void* aether_callback_lookup(const char* name);

#endif /* AETHER_CALLBACKS_H */
