#include "actor_reply.h"

#include <string.h>

/* The first `reply` in an arm body, searched through branches so a reply
 * inside an if or a match still counts, but not through a nested function,
 * actor or closure: their replies belong to a different handler. */
static ASTNode* first_reply(ASTNode* node) {
    if (!node) return NULL;
    if (node->type == AST_REPLY_STATEMENT) {
        return (node->child_count > 0 && node->children[0]) ? node : NULL;
    }
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_ACTOR_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return NULL;
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* r = first_reply(node->children[i]);
        if (r) return r;
    }
    return NULL;
}

static ASTNode* find_message_def(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && c->type == AST_MESSAGE_DEFINITION && c->value &&
            strcmp(c->value, name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* The field an asker reads out of a reply message: the first declared one.
 * `_message_id` is added by codegen, not by the user, so it never appears
 * in the definition being walked here. */
static ASTNode* first_message_field(ASTNode* msg_def) {
    if (!msg_def) return NULL;
    for (int i = 0; i < msg_def->child_count; i++) {
        ASTNode* f = msg_def->children[i];
        if (f && f->type == AST_MESSAGE_FIELD && f->value) return f;
    }
    return NULL;
}

int aether_resolve_reply(ASTNode* program, const char* request_msg,
                         AetherReplyShape* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!program || !request_msg) return 0;

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* actor = program->children[i];
        if (!actor || actor->type != AST_ACTOR_DEFINITION) continue;

        for (int j = 0; j < actor->child_count; j++) {
            ASTNode* recv = actor->children[j];
            if (!recv || recv->type != AST_RECEIVE_STATEMENT) continue;

            for (int k = 0; k < recv->child_count; k++) {
                ASTNode* arm = recv->children[k];
                if (!arm || arm->type != AST_RECEIVE_ARM || arm->child_count < 2) continue;
                ASTNode* pattern = arm->children[0];
                ASTNode* body = arm->children[1];
                if (!pattern || !pattern->value || !body) continue;
                if (strcmp(pattern->value, request_msg) != 0) continue;

                ASTNode* reply = first_reply(body);
                if (!reply) continue;

                ASTNode* payload = reply->children[0];
                if (payload->type == AST_MESSAGE_CONSTRUCTOR && payload->value) {
                    ASTNode* def = find_message_def(program, payload->value);
                    ASTNode* field = first_message_field(def);
                    out->reply_msg = payload->value;
                    if (field) {
                        out->reply_field = field->value;
                        out->field_type = field->node_type;
                    }
                } else {
                    out->reply_expr = payload;
                }
                return 1;
            }
        }
    }
    return 0;
}

Type* aether_reply_type(ASTNode* program, const char* request_msg) {
    AetherReplyShape shape;
    if (!aether_resolve_reply(program, request_msg, &shape)) return NULL;

    Type* t = shape.reply_msg ? shape.field_type
                              : (shape.reply_expr ? shape.reply_expr->node_type : NULL);
    if (!t || t->kind == TYPE_UNKNOWN) return NULL;
    return t;
}
