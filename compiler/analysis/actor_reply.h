/* What an actor replies to a given request message.
 *
 * Two passes need this answer and must agree on it. The type checker needs it
 * to type `x = actor ? Request {}`; codegen needs it to emit the read of the
 * reply buffer. They disagreed before this existed: codegen resolved the reply
 * and the type checker did not, so the ask expression had the right C type and
 * the variable receiving it was declared `int`, and any non-int reply failed
 * the C compile (#1537).
 *
 * Resolution is over the program AST alone, with no registry, so both callers
 * get the same answer from the same data.
 */

#ifndef AETHER_ACTOR_REPLY_H
#define AETHER_ACTOR_REPLY_H

#include "../ast.h"

typedef struct {
    /* Set when the handler replies with a message constructor: the message
     * name, and the type of the field the asker reads out of it. */
    const char* reply_msg;
    const char* reply_field;
    Type*       field_type;     /* borrowed from the message definition */

    /* Set when the handler replies with a bare expression. */
    ASTNode*    reply_expr;     /* borrowed from the arm body */
} AetherReplyShape;

/* Fills `out` with the reply shape for `request_msg` and returns 1, or
 * returns 0 and leaves `out` zeroed when no handler for that request replies
 * at all. Everything in `out` is borrowed from `program`. */
int aether_resolve_reply(ASTNode* program, const char* request_msg,
                         AetherReplyShape* out);

/* The type an ask on `request_msg` yields, or NULL when it cannot be
 * resolved: no such handler, no reply, or a reply whose own type is still
 * unknown. Borrowed from `program`; clone before storing. */
Type* aether_reply_type(ASTNode* program, const char* request_msg);

#endif /* AETHER_ACTOR_REPLY_H */
