#include "codegen_internal.h"
#include <string.h>

// Helper: given the receive-arm pattern, return 1 if the identifier name `rhs_name`
// binds a pattern field whose message-def type_kind is TYPE_PTR or TYPE_STRING.
// "Bound name" is either pf->value or its first PATTERN_VARIABLE child's value
// (when explicit `Field as alias` rebinding is used).
static int rhs_ident_is_ptr_pattern_field(CodeGenerator* gen, ASTNode* pattern,
                                          const char* rhs_name) {
    if (!pattern || pattern->type != AST_MESSAGE_PATTERN || !pattern->value || !rhs_name) {
        return 0;
    }
    MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
    if (!msg_def) return 0;
    for (int k = 0; k < pattern->child_count; k++) {
        ASTNode* pf = pattern->children[k];
        if (!pf || pf->type != AST_PATTERN_FIELD || !pf->value) continue;
        const char* bound = pf->value;
        if (pf->child_count > 0 && pf->children[0] &&
            pf->children[0]->type == AST_PATTERN_VARIABLE &&
            pf->children[0]->value) {
            bound = pf->children[0]->value;
        }
        if (strcmp(bound, rhs_name) != 0) continue;
        MessageFieldDef* fdef = msg_def->fields;
        while (fdef) {
            if (strcmp(fdef->name, pf->value) == 0) {
                return (fdef->type_kind == TYPE_PTR || fdef->type_kind == TYPE_STRING ||
                        fdef->type_kind == TYPE_ACTOR_REF);
            }
            fdef = fdef->next;
        }
    }
    return 0;
}

// Walk `body` looking for assignments of `field_name` whose RHS is a
// ptr/string-typed pattern field of `pattern`. The parser may represent
// `s = expr` inside a receive arm body as either AST_ASSIGNMENT
// (children[0]=lhs ident, children[1]=rhs) or AST_VARIABLE_DECLARATION
// (node->value=lhs name, children[0]=rhs); codegen_stmt later rewrites
// the latter to `self->field = ...` when value matches a state var.
static int body_assigns_field_from_ptr_pattern(CodeGenerator* gen, ASTNode* body,
                                               ASTNode* pattern, const char* field_name) {
    if (!body) return 0;
    ASTNode* rhs = NULL;
    if (body->type == AST_ASSIGNMENT && body->child_count >= 2) {
        ASTNode* lhs = body->children[0];
        if (lhs && lhs->type == AST_IDENTIFIER && lhs->value &&
            strcmp(lhs->value, field_name) == 0) {
            rhs = body->children[1];
        }
    } else if (body->type == AST_VARIABLE_DECLARATION && body->value &&
               strcmp(body->value, field_name) == 0 && body->child_count > 0) {
        rhs = body->children[0];
    }
    if (rhs && rhs->type == AST_IDENTIFIER && rhs->value &&
        rhs_ident_is_ptr_pattern_field(gen, pattern, rhs->value)) {
        return 1;
    }
    for (int i = 0; i < body->child_count; i++) {
        if (body_assigns_field_from_ptr_pattern(gen, body->children[i], pattern, field_name)) {
            return 1;
        }
    }
    return 0;
}

// Returns 1 if any receive arm under `node` (recursively) assigns `field_name`
// from a ptr/string pattern field. Handles both V2 (AST_RECEIVE_ARM) and V1
// (AST_BLOCK containing an AST_MESSAGE_PATTERN whose last child is the body) shapes.
static int state_field_assigned_ptr(CodeGenerator* gen, ASTNode* node, const char* field_name) {
    if (!node) return 0;

    if (node->type == AST_RECEIVE_ARM && node->child_count >= 2) {
        ASTNode* pattern = node->children[0];
        ASTNode* body = node->children[1];
        if (body_assigns_field_from_ptr_pattern(gen, body, pattern, field_name)) return 1;
    }

    if (node->type == AST_BLOCK) {
        ASTNode* pattern = NULL;
        for (int k = 0; k < node->child_count; k++) {
            if (node->children[k] && node->children[k]->type == AST_MESSAGE_PATTERN) {
                pattern = node->children[k];
                break;
            }
        }
        if (pattern && pattern->child_count > 0) {
            ASTNode* last = pattern->children[pattern->child_count - 1];
            if (last && last->type == AST_BLOCK &&
                body_assigns_field_from_ptr_pattern(gen, last, pattern, field_name)) {
                return 1;
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        if (state_field_assigned_ptr(gen, node->children[i], field_name)) return 1;
    }
    return 0;
}

// Returns 1 if `node` (recursively) sends or asks through the bare name
// `field_name`: `field_name ! Msg {}`, `field_name ? Msg {}`, or
// `send(field_name, ...)`. A send target is an actor reference, whatever
// its initializer (`state peer = 0`) inferred.
static int state_field_is_send_target(ASTNode* node, const char* field_name) {
    if (!node) return 0;
    if ((node->type == AST_SEND_FIRE_FORGET || node->type == AST_SEND_ASK ||
         node->type == AST_SEND_STATEMENT) && node->child_count >= 1) {
        ASTNode* target = node->children[0];
        if (target && target->type == AST_IDENTIFIER && target->value &&
            strcmp(target->value, field_name) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (state_field_is_send_target(node->children[i], field_name)) return 1;
    }
    return 0;
}

// True when field `field` of message `msg` holds a pointer: an actor
// reference or a `ptr`.
static int message_field_is_ptr(CodeGenerator* gen, const char* msg, const char* field) {
    MessageDef* def = lookup_message(gen->message_registry, msg);
    for (MessageFieldDef* f = def ? def->fields : NULL; f; f = f->next) {
        if (strcmp(f->name, field) == 0) {
            return f->type_kind == TYPE_PTR || f->type_kind == TYPE_ACTOR_REF;
        }
    }
    return 0;
}

static int is_send_node(ASTNode* node) {
    return (node->type == AST_SEND_FIRE_FORGET || node->type == AST_SEND_ASK ||
            node->type == AST_SEND_STATEMENT) && node->child_count >= 1;
}

// Returns 1 if `node` (recursively) passes the bare name `field_name` as a
// message field that holds a pointer: `peer ! Fwd { to: back }` with
// `to: ptr` or an actor-reference field.
static int state_field_passed_as_ptr(CodeGenerator* gen, ASTNode* node, const char* field_name) {
    if (!node) return 0;
    if (node->type == AST_MESSAGE_CONSTRUCTOR && node->value) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* fi = node->children[i];
            if (!fi || fi->type != AST_FIELD_INIT || !fi->value || fi->child_count == 0) continue;
            ASTNode* v = fi->children[0];
            if (v && v->type == AST_IDENTIFIER && v->value &&
                strcmp(v->value, field_name) == 0 &&
                message_field_is_ptr(gen, node->value, fi->value)) {
                return 1;
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (state_field_passed_as_ptr(gen, node->children[i], field_name)) return 1;
    }
    return 0;
}

// True when argument `arg_idx` of `call` goes to a `ptr` or actor-reference
// parameter: `introduce(s, my_ref)` with `introduce(s: ptr, me: ptr)`, or an
// extern / stdlib function taking a pointer (`list.add(xs, peer)`).
static int call_arg_is_ptr_param(CodeGenerator* gen, ASTNode* call, int arg_idx) {
    if (!call->value) return 0;
    TypeKind k = lookup_callee_param_kind(gen, call->value, arg_idx);
    return k == TYPE_PTR || k == TYPE_ACTOR_REF;
}

// Returns 1 if `node` (recursively) passes the bare name `field_name` as an
// argument to a `ptr` / actor-reference parameter.
static int state_field_passed_to_ptr_param(CodeGenerator* gen, ASTNode* node,
                                           const char* field_name) {
    if (!node) return 0;
    if (node->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* a = node->children[i];
            if (a && a->type == AST_IDENTIFIER && a->value &&
                strcmp(a->value, field_name) == 0 && call_arg_is_ptr_param(gen, node, i)) {
                return 1;
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (state_field_passed_to_ptr_param(gen, node->children[i], field_name)) return 1;
    }
    return 0;
}

// The actor a reference expression names: `Sink` for `r` in `r.field` when
// `r` is an actor reference to a Sink. NULL for anything else.
static const char* referenced_actor(ASTNode* obj) {
    if (!obj || !obj->node_type || obj->node_type->kind != TYPE_ACTOR_REF) return NULL;
    const Type* at = obj->node_type->element_type;
    return at && at->kind == TYPE_STRUCT ? at->struct_name : NULL;
}

// Keyed `Actor.field`, whole: a cut key made two long field names of one
// actor the same entry (#2539).
static void mark_ptr_field(CodeGenerator* gen, const char* actor, const char* field) {
    strmap_put(&gen->actor_ptr_fields, cg_internf("%s.%s", actor, field), NULL);
}

// The uses of `r.field`, from outside the actor or from another one, that
// make an actor's field a pointer: it is assigned a pointer or an actor
// reference, it is sent or asked through, or it is passed as a message field
// that holds a pointer.
static void mark_ptr_field_member_uses(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    ASTNode* lhs = NULL;
    ASTNode* rhs = NULL;
    if ((node->type == AST_ASSIGNMENT ||
         (node->type == AST_BINARY_EXPRESSION && node->value && strcmp(node->value, "=") == 0)) &&
        node->child_count >= 2) {
        lhs = node->children[0];
        rhs = node->children[1];
    }
    if (lhs && lhs->type == AST_MEMBER_ACCESS && lhs->value && lhs->child_count > 0 &&
        rhs && rhs->node_type &&
        (rhs->node_type->kind == TYPE_PTR || rhs->node_type->kind == TYPE_ACTOR_REF)) {
        const char* actor = referenced_actor(lhs->children[0]);
        if (actor) mark_ptr_field(gen, actor, lhs->value);
    }
    if (is_send_node(node)) {
        ASTNode* target = node->children[0];
        if (target && target->type == AST_MEMBER_ACCESS && target->value && target->child_count > 0) {
            const char* actor = referenced_actor(target->children[0]);
            if (actor) mark_ptr_field(gen, actor, target->value);
        }
    }
    if (node->type == AST_MESSAGE_CONSTRUCTOR && node->value) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* fi = node->children[i];
            if (!fi || fi->type != AST_FIELD_INIT || !fi->value || fi->child_count == 0) continue;
            ASTNode* v = fi->children[0];
            if (v && v->type == AST_MEMBER_ACCESS && v->value && v->child_count > 0 &&
                message_field_is_ptr(gen, node->value, fi->value)) {
                const char* actor = referenced_actor(v->children[0]);
                if (actor) mark_ptr_field(gen, actor, v->value);
            }
        }
    }
    if (node->type == AST_FUNCTION_CALL) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* v = node->children[i];
            if (v && v->type == AST_MEMBER_ACCESS && v->value && v->child_count > 0 &&
                call_arg_is_ptr_param(gen, node, i)) {
                const char* actor = referenced_actor(v->children[0]);
                if (actor) mark_ptr_field(gen, actor, v->value);
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        mark_ptr_field_member_uses(gen, node->children[i]);
    }
}

// The uses inside the actor itself, where a field is a bare name.
static int state_field_used_as_ptr_in_actor(CodeGenerator* gen, ASTNode* actor,
                                            const char* field_name) {
    return state_field_is_send_target(actor, field_name) ||
           state_field_passed_as_ptr(gen, actor, field_name) ||
           state_field_passed_to_ptr_param(gen, actor, field_name) ||
           state_field_assigned_ptr(gen, actor, field_name);
}

// #2466: a state field is stored as `void*` when the program uses it as a
// pointer, inside the actor or through `r.field` anywhere else: it is
// assigned a pointer (a `ptr` / actor-reference payload or value), it is the
// target of a send/ask, or it is passed as a message field or a function
// argument that holds a pointer. That is the `state next = 0` ... `a.next = b` ... `next ! Msg {}`
// shape, whose `0` initializer infers a number. Only use decides it, never
// the field's name: `state self_ref = 0` used as a number stays a number.
//
// One walk of the program answers it for every field, on the first ask; the
// member-access read path asks once per `r.field` it emits.
static int state_field_is_ptr(CodeGenerator* gen, ASTNode* actor, const char* field_name) {
    if (!gen->program) return state_field_used_as_ptr_in_actor(gen, actor, field_name);
    if (!gen->actor_ptr_fields_ready) {
        gen->actor_ptr_fields_ready = 1;
        for (int i = 0; i < gen->program->child_count; i++) {
            ASTNode* a = gen->program->children[i];
            if (!a || a->type != AST_ACTOR_DEFINITION || !a->value) continue;
            for (int j = 0; j < a->child_count; j++) {
                ASTNode* c = a->children[j];
                if (c && c->type == AST_STATE_DECLARATION && c->value &&
                    state_field_used_as_ptr_in_actor(gen, a, c->value)) {
                    mark_ptr_field(gen, a->value, c->value);
                }
            }
        }
        mark_ptr_field_member_uses(gen, gen->program);
    }
    return strmap_has(&gen->actor_ptr_fields,
                      cg_internf("%s.%s", actor->value ? actor->value : "", field_name));
}

// The state fields generate_actor_definition emits with an atomic C type:
// the int / long / Duration ones it does not widen to a pointer.
static int state_decl_is_atomic(CodeGenerator* gen, ASTNode* actor, ASTNode* decl) {
    if (!decl->node_type || state_field_is_ptr(gen, actor, decl->value)) return 0;
    return decl->node_type->kind == TYPE_INT || decl->node_type->kind == TYPE_INT64 ||
           decl->node_type->kind == TYPE_DURATION;
}

int actor_state_field_is_atomic(CodeGenerator* gen, const char* actor_name,
                                const char* field) {
    if (!gen || !gen->program || !actor_name || !field) return 0;
    for (int i = 0; i < gen->program->child_count; i++) {
        ASTNode* actor = gen->program->children[i];
        if (!actor || actor->type != AST_ACTOR_DEFINITION || !actor->value ||
            strcmp(actor->value, actor_name) != 0) continue;
        for (int j = 0; j < actor->child_count; j++) {
            ASTNode* c = actor->children[j];
            if (c && c->type == AST_STATE_DECLARATION && c->value &&
                strcmp(c->value, field) == 0) {
                return state_decl_is_atomic(gen, actor, c);
            }
        }
        return 0;
    }
    return 0;
}

/* The variable an assignment target writes through: `x`, `x.f`, `x[i]`. */
static const char* store_target_root(ASTNode* lhs) {
    while (lhs && (lhs->type == AST_MEMBER_ACCESS || lhs->type == AST_ARRAY_ACCESS) &&
           lhs->child_count > 0) {
        lhs = lhs->children[0];
    }
    return (lhs && lhs->type == AST_IDENTIFIER) ? lhs->value : NULL;
}

static void mark_mentioned_struct_vars(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value) {
        mark_return_escaped_struct_var(gen, node->value);
    }
    /* Reading a scalar or string field shares nothing with the struct: a
     * string taken from a field is copied where it is stored (#2461). A
     * struct-typed (or untyped) field may share the struct's strings. */
    if (node->type == AST_MEMBER_ACCESS && node->node_type &&
        node->node_type->kind != TYPE_STRUCT && node->node_type->kind != TYPE_UNKNOWN) {
        return;
    }
    for (int i = 0; i < node->child_count; i++) {
        mark_mentioned_struct_vars(gen, node->children[i]);
    }
}

/* #2498: a value stored into actor state outlives the handler, as a returned
 * one outlives its function. A struct local named anywhere in a state
 * store's value (directly, inside a literal, through a call that may hand it
 * back) therefore gives its strings to the state: its scope-exit destroy is
 * suppressed the way a return suppresses it (#752). Marked before the body
 * is emitted, so an earlier `return` in a loop that later stores is covered
 * too. Naming a non-struct here is harmless: no destroy is keyed to it.
 * A closure that captures the struct holds a copy with strings of its own
 * (#2504), so a capture is no reason to keep them. */
static void mark_state_stored_struct_vars(CodeGenerator* gen, ASTNode* node) {
    if (!node) return;
    if (node->type == AST_VARIABLE_DECLARATION && is_actor_state_var(gen, node->value)) {
        /* A struct that owns strings is taken by the store (`<Name>_replace`
         * with emit_struct_take): a local is moved in on its last use and
         * copied otherwise, so it keeps its scope-exit destroy, which then
         * releases what it still owns. */
        ASTNode* v = node->child_count > 0 ? node->children[0] : NULL;
        if (!(v && struct_owning_strings(gen, v->node_type)))
            for (int i = 0; i < node->child_count; i++) mark_mentioned_struct_vars(gen, node->children[i]);
    } else if (node->type == AST_ASSIGNMENT && node->child_count >= 2) {
        const char* root = store_target_root(node->children[0]);
        if (root && (strcmp(root, "self") == 0 || is_actor_state_var(gen, root))) {
            mark_mentioned_struct_vars(gen, node->children[1]);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        mark_state_stored_struct_vars(gen, node->children[i]);
    }
}

/* #2528: a `string` state field owns what it holds, tracked by
 * `_heap_<name>` beside it (a `ptr`-widened field is a payload the user
 * manages). */
int state_field_owns_string(ASTNode* state_decl) {
    return state_decl && state_decl->type == AST_STATE_DECLARATION &&
           state_decl->node_type && state_decl->node_type->kind == TYPE_STRING;
}

/* #2528: a `string[N]` (1) or `fn[N]` (2) state field, whose elements the
 * actor owns as a struct's such field does; length in `*len`. */
int state_array_owned(ASTNode* state_decl, int* len) {
    if (!state_decl || state_decl->type != AST_STATE_DECLARATION || !state_decl->node_type) return 0;
    Type* t = state_decl->node_type;
    if (t->kind != TYPE_ARRAY || t->array_size <= 0 || !t->element_type) return 0;
    if (len) *len = t->array_size;
    if (t->element_type->kind == TYPE_STRING) return 1;
    if (t->element_type->kind == TYPE_FUNCTION && !t->element_type->is_fnptr) return 2;
    return 0;
}

/* #2528: `<Actor>_destroy_state(void*)`, the scheduler's hook for what the
 * state fields own: a heap string per its tracker, a closure's environment
 * (#2525), a struct that owns strings or closures through its own
 * destructor. Run once by the free path that ends a scheduler-owned actor,
 * after it can no longer be stepped. Idempotent, as a struct destroy is. */
static void emit_actor_destroy_state(CodeGenerator* gen, ASTNode* actor) {
    print_line(gen, "static void %s_destroy_state(void* _p) {", actor->value);
    indent(gen);
    print_line(gen, "%s* self = (%s*)_p;", actor->value, actor->value);
    print_line(gen, "(void)self;");
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (!child || child->type != AST_STATE_DECLARATION || !child->node_type) continue;
        Type* t = child->node_type;
        int oalen = 0;
        int oak = state_array_owned(child, &oalen);
        if (state_field_owns_string(child)) {
            print_line(gen, "if (self->_heap_%s) { aether_heap_str_free(self->%s); self->%s = (const char*)0; self->_heap_%s = 0; }",
                       child->value, child->value, child->value, child->value);
        } else if (oak == 1) {
            /* #2528: a `string[N]` state field owns a copy of each element,
             * a `fn[N]` one a reference per element (as the struct fields). */
            print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (self->%s[_ai]) { aether_heap_str_free(self->%s[_ai]); self->%s[_ai] = (const char*)0; }",
                       oalen, child->value, child->value, child->value);
        } else if (oak == 2) {
            print_line(gen, "for (int _ai = 0; _ai < %d; _ai++) if (self->%s[_ai].env) { _aether_closure_env_release(self->%s[_ai].env); self->%s[_ai].env = (void*)0; }",
                       oalen, child->value, child->value, child->value);
        } else if (t->kind == TYPE_FUNCTION && !t->is_fnptr &&
                   !state_field_is_ptr(gen, actor, child->value)) {
            print_line(gen, "if (self->%s.env) { _aether_closure_env_release(self->%s.env); self->%s.env = (void*)0; }",
                       child->value, child->value, child->value);
        } else {
            const char* sname = struct_owning_strings(gen, t);
            if (sname && !state_field_is_ptr(gen, actor, child->value)) {
                print_line(gen, "%s_destroy(&self->%s);", sname, child->value);
            }
        }
    }
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
}

void generate_actor_definition(CodeGenerator* gen, ASTNode* actor) {
    if (!actor || actor->type != AST_ACTOR_DEFINITION) return;
    
    gen->current_actor = strdup(actor->value);
    gen->state_var_count = 0;
    gen->actor_state_vars = NULL;
    
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_STATE_DECLARATION) {
            char** new_svars = realloc(gen->actor_state_vars,
                                       (gen->state_var_count + 1) * sizeof(char*));
            if (!new_svars) continue;
            gen->actor_state_vars = new_svars;
            gen->actor_state_vars[gen->state_var_count] = strdup(child->value);
            gen->state_var_count++;
        }
    }
    
    // Generate cache-aligned actor struct with optimized field layout
    print_line(gen, "#ifdef _MSC_VER");
    print_line(gen, "__declspec(align(64))");
    print_line(gen, "#endif");
    print_line(gen, "typedef struct");
    print_line(gen, "#if defined(__GNUC__) || defined(__clang__)");
    print_line(gen, "__attribute__((aligned(64)))");
    print_line(gen, "#endif");
    print_line(gen, "%s {", actor->value);
    indent(gen);
    
    /* The prefix of a generated actor IS an ActorBase: the scheduler is handed
     * a pointer to it and casts. The fields come from the runtime's one
     * definition (AETHER_ACTOR_BASE_FIELDS), so a field added there is here
     * too; a hand-written copy that missed one put the first state field at
     * its offset, and the scheduler wrote through it. */
    print_line(gen, "AETHER_ACTOR_BASE_FIELDS");
    print_line(gen, "");

    // State fields (user-defined)
    // NOTE: All state fields are atomic to allow safe cross-thread access
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_STATE_DECLARATION) {
            print_indent(gen);
            if (state_field_is_ptr(gen, actor, child->value)) {
                // The state field holds a pointer (an actor reference, a
                // ptr/string payload) though its initializer (typically `0`)
                // inferred a number: widen to void* so the uses compile.
                // The `0` still works as a null pointer constant.
                fprintf(gen->output, "void* %s;\n", child->value);
            } else if (type_is_sized_array(child->node_type) &&
                       child->node_type->array_size > 0) {
                /* #2464: `state int[4] hist` is the C declarator
                 * `int hist[4]`, as a struct field is
                 * (generate_extern_struct_field), not `int[4] hist`. */
                fprintf(gen->output, "%s %s[%d];\n",
                        get_c_type(child->node_type->element_type), child->value,
                        child->node_type->array_size);
            } else {
                // Use atomic types for numeric fields to enable safe concurrent access
                if (child->node_type && child->node_type->kind == TYPE_INT) {
                    fprintf(gen->output, "atomic_int %s;\n", child->value);
                } else if (child->node_type && (child->node_type->kind == TYPE_INT64 ||
                                                child->node_type->kind == TYPE_DURATION)) {
                    fprintf(gen->output, "_Atomic int64_t %s;\n", child->value);
                } else {
                    generate_type(gen, child->node_type);
                    fprintf(gen->output, " %s;\n", child->value);
                }
            }
            /* #2528: a string state field's ownership tracker lives with
             * the field, for the handlers (which alias `_heap_<name>` to
             * it) and for the destructor. It used to be a handler local,
             * reset to 0 on every message: a value stored by an earlier
             * message was never freed on overwrite, and the one stored
             * last never at all. */
            if (state_field_owns_string(child)) {
                print_line(gen, "int _heap_%s;", child->value);
            }
        }
    }

    unindent(gen);
    print_line(gen, "} %s;", actor->value);
    emit_actor_destroy_state(gen, actor);
    /* The scheduler casts this to ActorBase*, so every field it touches has to
     * sit at the ActorBase offset. Checking the last one pins the whole
     * prefix. */
    print_line(gen, "#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L");
    print_line(gen, "_Static_assert(offsetof(%s, scheduler_owned) == offsetof(ActorBase, scheduler_owned),",
               actor->value);
    print_line(gen, "               \"%s prefix must match ActorBase\");", actor->value);
    print_line(gen, "#endif");
    print_line(gen, "");
    
    // Generate individual message handler functions
    int pattern_count = 0;
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_RECEIVE_STATEMENT && child->child_count > 0) {
            // V2 syntax: receive { Pattern -> body, ... }
            // V1 syntax: receive(msg) { body }
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* arm = child->children[j];

                ASTNode* pattern = NULL;
                ASTNode* arm_body = NULL;

                // Check for V2 receive arm structure
                if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 2) {
                    pattern = arm->children[0];
                    arm_body = arm->children[1];
                }
                // Check for V1 BLOCK containing MESSAGE_PATTERN
                else if (arm->type == AST_BLOCK) {
                    for (int k = 0; k < arm->child_count; k++) {
                        if (arm->children[k]->type == AST_MESSAGE_PATTERN) {
                            pattern = arm->children[k];
                            // Find the body (last BLOCK child of pattern)
                            if (pattern->child_count > 0) {
                                ASTNode* last = pattern->children[pattern->child_count - 1];
                                if (last->type == AST_BLOCK) {
                                    arm_body = last;
                                }
                            }
                            break;
                        }
                    }
                }

                // Generate handler if we found a pattern
                if (pattern && pattern->type == AST_MESSAGE_PATTERN) {
                    MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
                    if (msg_def) {
                        print_line(gen, "static AETHER_HOT void %s_handle_%s(%s* self, void* _msg_data) {",
                                  actor->value, pattern->value, actor->value);
                        indent(gen);
                        /* #2498: the handler is a defer scope, as a function
                         * body is, opened before the pattern bindings so a
                         * binding's cell (#2492) is released with the rest:
                         * the arm's defers, its promoted cells, the env of a
                         * closure bound in it (#2494) and a struct local's
                         * destroy all run when the handler ends, on every
                         * exit. They were dropped, and leaked per message. */
                        enter_scope(gen);
                        // Reset declared-vars / heap-string state for this
                        // handler. Each handler is its own C function with
                        // its own scope; carrying entries over from the
                        // last regular function (or the previous handler)
                        // makes hoist_loop_vars believe a name is already
                        // declared and skip emitting its declaration.
                        // Without this reset, a free function whose
                        // parameter shares a name with a variable first
                        // assigned inside the handler's while body would
                        // leak into the handler and produce undeclared-
                        // identifier errors in the generated C.
                        clear_declared_vars(gen);
                        clear_fnptr_locals(gen);
                        clear_heap_string_vars(gen);
                        clear_captured_string_params(gen);
    clear_seq_vars(gen);
    clear_opt_str_vars(gen);
                        print_line(gen, "%s* _pattern = (%s*)_msg_data;", pattern->value, pattern->value);
                        mark_var_declared(gen, "_pattern");

                        // This arm's Route 1 promoted names (published
                        // below). The synthetic name matches what
                        // discover_closures_scoped emitted:
                        // `__recv_arm_<arm_ptr>`.
                        char arm_name[256];
                        snprintf(arm_name, sizeof(arm_name), "__recv_arm_%p", (void*)arm);
                        char** arm_promoted = NULL;
                        int arm_promoted_count = 0;
                        get_promoted_names_for_func(gen, arm_name, &arm_promoted, &arm_promoted_count);

                        // Extract pattern fields with correct types from message definition.
                        // Single-int-field messages use intptr_t (matches payload_int width).
                        // Composite-type fields (arrays, structs) use the resolved c_type
                        // stored on MessageFieldDef at registration time, which preserves
                        // element type info that the bare type_kind drops.
                        const char* single_int_name = get_single_int_field(msg_def);
                        for (int k = 0; k < pattern->child_count; k++) {
                            ASTNode* field = pattern->children[k];
                            if (field->type == AST_PATTERN_FIELD) {
                                const char* c_type = "int";
                                if (msg_def && msg_def->fields) {
                                    MessageFieldDef* fdef = msg_def->fields;
                                    while (fdef) {
                                        if (strcmp(fdef->name, field->value) == 0) {
                                            if (single_int_name && fdef->type_kind == TYPE_INT) {
                                                c_type = "intptr_t";
                                            } else if (fdef->c_type) {
                                                c_type = fdef->c_type;
                                            } else {
                                                Type temp_type = { .kind = fdef->type_kind, .element_type = NULL, .array_size = 0, .struct_name = NULL };
                                                c_type = get_c_type(&temp_type);
                                            }
                                            break;
                                        }
                                        fdef = fdef->next;
                                    }
                                }
                                const char* var_name = field->value;
                                if (field->child_count > 0 && field->children[0] &&
                                    field->children[0]->type == AST_PATTERN_VARIABLE && field->children[0]->value) {
                                    var_name = field->children[0]->value;
                                }
                                int promoted = 0;
                                for (int pp = 0; pp < arm_promoted_count; pp++) {
                                    if (arm_promoted[pp] && strcmp(arm_promoted[pp], var_name) == 0) {
                                        promoted = 1;
                                        break;
                                    }
                                }
                                if (promoted) {
                                    /* #2492: a closure in the arm writes this
                                     * binding, so it lives in a shared cell
                                     * seeded from the message, as a promoted
                                     * parameter does. The cell holds the
                                     * field's own type, which is what the
                                     * closures' envs point at; intptr_t is
                                     * only the payload width. */
                                    const char* from = cg_internf("_pattern->%s", field->value);
                                    print_indent(gen);
                                    emit_promoted_param_cell(gen, var_name,
                                        strcmp(c_type, "intptr_t") == 0 ? "int" : c_type,
                                        from, field->line, field->column);
                                    continue;
                                }
                                print_line(gen, "%s %s = _pattern->%s;", c_type, var_name, field->value);
                                // Pattern fields are now C-locals at the
                                // top of the handler — record them so a
                                // subsequent hoist pass doesn't attempt
                                // a duplicate declaration.
                                mark_var_declared(gen, var_name);
                            }
                        }

                        // Publish this arm's Route 1 promoted names so
                        // variable decls in the handler malloc heap cells
                        // and reads/writes dereference.
                        char** prev_promoted = gen->current_promoted_captures;
                        int prev_promoted_count = gen->current_promoted_capture_count;
                        gen->current_promoted_captures = arm_promoted;
                        gen->current_promoted_capture_count = arm_promoted_count;
                        const char* prev_closure_var_scope = gen->closure_var_scope;
                        gen->closure_var_scope = arm_name;   /* #2513 */

                        // Pre-hoist `_heap_<name>` companions for string
                        // locals in the handler body, exactly as
                        // generate_function_definition does for regular
                        // functions (codegen_func.c) and main (codegen.c).
                        // Without this, a handler that builds a heap string
                        // — e.g. `n = string.concat(in_n, "")` to retain a
                        // message field into state — emits a `_heap_n`
                        // tracker reference that is never declared, producing
                        // `'_heap_n' undeclared` in the generated C. The
                        // handler is its own C function, so it needs the same
                        // function-scope hoist pass.
                        if (arm_body && arm_body->type == AST_BLOCK) {
                            /* #2124: the handler is the hoist scope for
                             * its own branch-local joins, as a function
                             * body is. */
                            gen->hoist_scope_body = arm_body;
                            hoist_heap_string_trackers(gen, arm_body);
                        }

                        // Generate handler body
                        /* The handler's own escape marks: the ones the last
                         * function left would suppress the destroy of a
                         * local that merely shares a name, and this
                         * handler's must not reach the next function. */
                        char** outer_escaped = gen->return_escaped_struct_vars;
                        int outer_escaped_count = gen->return_escaped_struct_var_count;
                        gen->return_escaped_struct_vars = NULL;
                        gen->return_escaped_struct_var_count = 0;
                        if (arm_body && arm_body->type == AST_BLOCK) {
                            mark_state_stored_struct_vars(gen, arm_body);
                            for (int k = 0; k < arm_body->child_count; k++) {
                                generate_statement(gen, arm_body->children[k]);
                            }
                        }
                        exit_scope(gen);
                        for (int k = 0; k < gen->return_escaped_struct_var_count; k++) {
                            free(gen->return_escaped_struct_vars[k]);
                        }
                        free(gen->return_escaped_struct_vars);
                        gen->return_escaped_struct_vars = outer_escaped;
                        gen->return_escaped_struct_var_count = outer_escaped_count;

                        gen->current_promoted_captures = prev_promoted;
                        gen->current_promoted_capture_count = prev_promoted_count;
                        gen->closure_var_scope = prev_closure_var_scope;

                        unindent(gen);
                        print_line(gen, "}");
                        print_line(gen, "");
                        pattern_count++;
                    }
                }
            }
        }
    }
    
    // Generate function pointer table
    if (pattern_count > 0) {
        print_line(gen, "typedef void (*%s_MessageHandler)(%s*, void*);", actor->value, actor->value);
        print_line(gen, "static %s_MessageHandler %s_handlers[256] = {0};", actor->value, actor->value);
        print_line(gen, "static int %s_handlers_initialized = 0;", actor->value);
        print_line(gen, "");
        
        print_line(gen, "static void %s_init_handlers(%s* self) {", actor->value, actor->value);
        indent(gen);
        print_line(gen, "if (%s_handlers_initialized) return;", actor->value);
        
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* child = actor->children[i];
            if (child->type == AST_RECEIVE_STATEMENT && child->child_count > 0) {
                for (int j = 0; j < child->child_count; j++) {
                    ASTNode* arm = child->children[j];
                    ASTNode* pattern = NULL;

                    if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 1) {
                        pattern = arm->children[0];
                    } else if (arm->type == AST_BLOCK) {
                        for (int k = 0; k < arm->child_count; k++) {
                            if (arm->children[k]->type == AST_MESSAGE_PATTERN) {
                                pattern = arm->children[k];
                                break;
                            }
                        }
                    }

                    if (pattern && pattern->type == AST_MESSAGE_PATTERN) {
                        MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
                        if (msg_def) {
                            print_line(gen, "%s_handlers[%d] = %s_handle_%s;",
                                      actor->value, msg_def->message_id, actor->value, pattern->value);
                        }
                    }
                }
            }
        }
        
        print_line(gen, "%s_handlers_initialized = 1;", actor->value);
        unindent(gen);
        print_line(gen, "}");
        print_line(gen, "");
    }
    
    // Check if any receive block has a timeout arm
    ASTNode* timeout_arm = NULL;
    for (int i = 0; i < actor->child_count && !timeout_arm; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_RECEIVE_STATEMENT) {
            for (int j = 0; j < child->child_count; j++) {
                if (child->children[j]->type == AST_TIMEOUT_ARM) {
                    timeout_arm = child->children[j];
                    break;
                }
            }
        }
    }

    print_line(gen, "void %s_step(%s* self) {", actor->value, actor->value);
    indent(gen);

    // Record this actor as the one currently running on this thread so
    // runtime helpers like ae_io_await() (std/net/aether_actor_bridge.c)
    // can identify the caller without an explicit parameter. Declared
    // in runtime/scheduler/multicore_scheduler.h.
    print_line(gen, "aether_step_actor_set(self);");

    // Timeout check — fires if idle longer than timeout_ns
    if (timeout_arm) {
        print_line(gen, "// Timeout check");
        print_line(gen, "if (self->timeout_ns > 0 && self->last_activity_ns > 0) {");
        indent(gen);
        print_line(gen, "uint64_t _now = (uint64_t)_aether_clock_ns();");
        print_line(gen, "if ((_now - self->last_activity_ns) >= self->timeout_ns) {");
        indent(gen);
        print_line(gen, "self->timeout_ns = 0;  // one-shot");
        print_line(gen, "self->last_activity_ns = 0;");
        // Generate the timeout body
        if (timeout_arm->child_count >= 2 && timeout_arm->children[1]) {
            ASTNode* tbody = timeout_arm->children[1];
            /* A defer scope and the hoist scope of its own, as a receive
             * arm is (#2494). */
            ASTNode* prev_hoist_scope = gen->hoist_scope_body;
            enter_scope(gen);
            if (tbody->type == AST_BLOCK) {
                gen->hoist_scope_body = tbody;
                for (int j = 0; j < tbody->child_count; j++) {
                    generate_statement(gen, tbody->children[j]);
                }
            } else {
                generate_statement(gen, tbody);
            }
            exit_scope(gen);
            gen->hoist_scope_body = prev_hoist_scope;
        }
        print_line(gen, "return;");
        unindent(gen);
        print_line(gen, "}");
        unindent(gen);
        print_line(gen, "}");
        print_line(gen, "");
    }

    print_line(gen, "Message msg;");
    print_line(gen, "");
    print_line(gen, "if (unlikely(!mailbox_receive(&self->mailbox, &msg))) {");
    indent(gen);
    if (timeout_arm) {
        // Start timeout countdown when mailbox is empty
        print_line(gen, "if (self->timeout_ns > 0 && self->last_activity_ns == 0) {");
        indent(gen);
        print_line(gen, "self->last_activity_ns = (uint64_t)_aether_clock_ns();");
        unindent(gen);
        print_line(gen, "}");
    }
    print_line(gen, "atomic_store_explicit(&self->active, 0, memory_order_relaxed);");
    print_line(gen, "return;");
    unindent(gen);
    print_line(gen, "}");
    if (timeout_arm) {
        // Message received — cancel timeout (one-shot: fire only if no messages ever arrive)
        print_line(gen, "self->timeout_ns = 0;");
        print_line(gen, "self->last_activity_ns = 0;");
    }
    print_line(gen, "aether_reply_slot_set(msg._reply_slot);");
    print_line(gen, "");
    
    if (pattern_count > 0) {
        print_line(gen, "void* _msg_data = msg.payload_ptr;");
        print_line(gen, "int _msg_id = msg.type;");
        print_line(gen, "");
        // Emscripten/wasm32 doesn't support label-address tables (GCC's
        // "labels as values") because they require relocations in code
        // sections, which wasm disallows ("relocations for function or
        // section offsets are only supported in metadata sections").
        // Route wasm through the MSVC switch-case fallback the same way.
        // __wasi__ / __wasm__ join __EMSCRIPTEN__ here. The comment above has
        // always described the intent as "route wasm through the switch-case
        // fallback", but the guard named only Emscripten, so a wasm32-wasi
        // build still emitted the label-address table and died with exactly
        // the error quoted above (#1655). optimizer.c's equivalent guards
        // already list all three; this one was the outlier.
        print_line(gen, "#if AETHER_GCC_COMPAT && !defined(__EMSCRIPTEN__) && \\");
        print_line(gen, "    !defined(__wasi__) && !defined(__wasm__)");
        print_line(gen, "// COMPUTED GOTO DISPATCH - 15-30%% faster than switch");
        print_line(gen, "static void* dispatch_table[256] = {");
        indent(gen);

        // Generate dispatch table with labels (GCC/Clang path)
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* child = actor->children[i];
            if (child->type == AST_RECEIVE_STATEMENT && child->child_count > 0) {
                for (int j = 0; j < child->child_count; j++) {
                    ASTNode* arm = child->children[j];
                    ASTNode* pattern = NULL;

                    if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 1) {
                        pattern = arm->children[0];
                    }
                    else if (arm->type == AST_BLOCK) {
                        for (int k = 0; k < arm->child_count; k++) {
                            if (arm->children[k]->type == AST_MESSAGE_PATTERN) {
                                pattern = arm->children[k];
                                break;
                            }
                        }
                    }

                    if (pattern && pattern->type == AST_MESSAGE_PATTERN) {
                        MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
                        if (msg_def) {
                            print_line(gen, "[%d] = &&handle_%s,", msg_def->message_id, pattern->value);
                        }
                    }
                }
            }
        }

        unindent(gen);
        print_line(gen, "};");
        print_line(gen, "if (likely(_msg_id >= 0 && _msg_id < 256 && dispatch_table[_msg_id])) {");
        indent(gen);
        print_line(gen, "goto *dispatch_table[_msg_id];");
        unindent(gen);
        print_line(gen, "}");
        print_line(gen, "#else");
        print_line(gen, "// MSVC: switch-case dispatch fallback");
        print_line(gen, "switch (_msg_id) {");

        // Generate switch cases (MSVC path)
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* child = actor->children[i];
            if (child->type == AST_RECEIVE_STATEMENT && child->child_count > 0) {
                for (int j = 0; j < child->child_count; j++) {
                    ASTNode* arm = child->children[j];
                    ASTNode* pattern = NULL;

                    if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 1) {
                        pattern = arm->children[0];
                    }
                    else if (arm->type == AST_BLOCK) {
                        for (int k = 0; k < arm->child_count; k++) {
                            if (arm->children[k]->type == AST_MESSAGE_PATTERN) {
                                pattern = arm->children[k];
                                break;
                            }
                        }
                    }

                    if (pattern && pattern->type == AST_MESSAGE_PATTERN) {
                        MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
                        if (msg_def) {
                            print_line(gen, "case %d: goto handle_%s;", msg_def->message_id, pattern->value);
                        }
                    }
                }
            }
        }

        print_line(gen, "default: break;");
        print_line(gen, "}");
        print_line(gen, "#endif");
        print_line(gen, "return;  // Unknown message type");
        print_line(gen, "");
        
        // Generate labels for each handler
        for (int i = 0; i < actor->child_count; i++) {
            ASTNode* child = actor->children[i];
            if (child->type == AST_RECEIVE_STATEMENT && child->child_count > 0) {
                for (int j = 0; j < child->child_count; j++) {
                    ASTNode* arm = child->children[j];
                    ASTNode* pattern = NULL;

                    // V2: AST_RECEIVE_ARM contains pattern
                    if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 1) {
                        pattern = arm->children[0];
                    }
                    // V1: AST_BLOCK contains MESSAGE_PATTERN
                    else if (arm->type == AST_BLOCK) {
                        for (int k = 0; k < arm->child_count; k++) {
                            if (arm->children[k]->type == AST_MESSAGE_PATTERN) {
                                pattern = arm->children[k];
                                break;
                            }
                        }
                    }

                    if (pattern && pattern->type == AST_MESSAGE_PATTERN) {
                        MessageDef* msg_def = lookup_message(gen->message_registry, pattern->value);
                        const char* single_int = msg_def ? get_single_int_field(msg_def) : NULL;

                        print_line(gen, "handle_%s:", pattern->value);
                        indent(gen);
                        if (single_int) {
                            // Single-field inline: reconstruct on stack from payload_int
                            const char* cast = "";
                            if (msg_def->fields) {
                                int fk = msg_def->fields->type_kind;
                                if (fk == TYPE_INT64 || fk == TYPE_DURATION) cast = "(int64_t)";
                                else if (fk == TYPE_PTR || fk == TYPE_ACTOR_REF) cast = "(void*)";
                            }
                            print_line(gen, "if (_msg_data) {");
                            indent(gen);
                            print_line(gen, "%s_handle_%s(self, _msg_data);", actor->value, pattern->value);
                            /* Release heap-string fields the sender
                             * deep-copied at send time (#466). The
                             * <Msg>_release_fields function is
                             * emitted alongside the message struct
                             * iff the message has at least one
                             * string field; we always call it here
                             * — for messages without string fields
                             * the function isn't emitted and the C
                             * compiler will reject this call,
                             * but for the single-int path we land
                             * here only when the message struct was
                             * heap-allocated (non-inline path),
                             * which by construction means the
                             * message has at least one non-int
                             * field. For inline (single-int)
                             * messages this branch fires only when
                             * _msg_data is non-NULL — same shape,
                             * so the release_fields function is
                             * always available. */
                            if (msg_def) {
                                int msg_has_string = 0;
                                for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                                    /* #2525: a closure field is released with the strings. */
                                    if (f->type_kind == TYPE_STRING ||
                                        (f->type_kind == TYPE_FUNCTION && f->c_type &&
                                         strcmp(f->c_type, "_AeClosure") == 0)) { msg_has_string = 1; break; }
                                }
                                if (msg_has_string) {
                                    print_line(gen, "%s_release_fields((%s*)_msg_data);",
                                               pattern->value, pattern->value);
                                }
                            }
                            print_line(gen, "aether_free_message(_msg_data);");
                            unindent(gen);
                            print_line(gen, "} else {");
                            indent(gen);
                            print_line(gen, "%s _msg_val = { ._message_id = msg.type, .%s = %smsg.payload_int };",
                                      pattern->value, single_int, cast);
                            print_line(gen, "%s_handle_%s(self, &_msg_val);", actor->value, pattern->value);
                            unindent(gen);
                            print_line(gen, "}");
                        } else {
                            // Two-field inline: reconstruct from payload_int + payload_ptr
                            print_line(gen, "%s_handle_%s(self, _msg_data);", actor->value, pattern->value);
                            /* Same release-fields call as the single-
                             * int branch above (#466). */
                            if (msg_def) {
                                int msg_has_string = 0;
                                for (MessageFieldDef* f = msg_def->fields; f; f = f->next) {
                                    /* #2525: a closure field is released with the strings. */
                                    if (f->type_kind == TYPE_STRING ||
                                        (f->type_kind == TYPE_FUNCTION && f->c_type &&
                                         strcmp(f->c_type, "_AeClosure") == 0)) { msg_has_string = 1; break; }
                                }
                                if (msg_has_string) {
                                    print_line(gen, "%s_release_fields((%s*)_msg_data);",
                                               pattern->value, pattern->value);
                                }
                            }
                            print_line(gen, "aether_free_message(_msg_data);");
                        }
                        print_line(gen, "return;");
                        unindent(gen);
                        print_line(gen, "");
                    }
                }
            }
        }
    }
    
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
    
    print_line(gen, "%s* spawn_%s(int preferred_core) {", actor->value, actor->value);
    indent(gen);
    print_line(gen, "// AETHER_SINGLE_CORE=1 forces all actors to core 0 (eliminates cross-core overhead)");
    print_line(gen, "static int _single_core_cached = -1;");
    print_line(gen, "if (_single_core_cached < 0) _single_core_cached = (getenv(\"AETHER_SINGLE_CORE\") != NULL);");
    print_line(gen, "int core = (preferred_core >= 0) ? preferred_core : (_single_core_cached ? 0 : -1);");
    print_line(gen, "%s* actor = (%s*)scheduler_spawn_actor(core, (void (*)(void*))%s_step, sizeof(%s));",
               actor->value, actor->value, actor->value, actor->value);
    /* NULL means the allocation failed: nothing pools actors any more. The
     * second allocation that used to follow was no likelier to succeed, and
     * its block came from an allocator that scheduler_release_actor's free
     * does not match (#2485). */
    print_line(gen, "if (!actor) return NULL;");
    print_line(gen, "atomic_init(&actor->active, 0);  // inactive until first message send");
    print_line(gen, "atomic_init(&actor->migrate_to, -1);");
    print_line(gen, "atomic_init(&actor->dead, 0);");
    print_line(gen, "actor->auto_process = 0;");
    print_line(gen, "");
    
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_STATE_DECLARATION) {
            if (type_is_sized_array(child->node_type) && child->node_type->array_size > 0 &&
                !state_field_is_ptr(gen, actor, child->value)) {
                /* #2464: an array field does not assign. Zero it, then set
                 * the elements an array-literal initializer gives. */
                print_line(gen, "memset(actor->%s, 0, sizeof(actor->%s));",
                           child->value, child->value);
                ASTNode* init = child->child_count > 0 ? child->children[0] : NULL;
                if (init && init->type == AST_ARRAY_LITERAL) {
                    /* #2528: an owned array's elements are copied or taken. */
                    int oak = state_array_owned(child, NULL);
                    for (int e = 0; e < init->child_count &&
                                    e < child->node_type->array_size; e++) {
                        print_indent(gen);
                        fprintf(gen->output, "actor->%s[%d] = ", child->value, e);
                        if (oak == 1) {
                            emit_owned_string_element(gen, init->children[e]);
                        } else if (oak == 2) {
                            emit_closure_take(gen, init->children[e]);
                        } else {
                            generate_expression(gen, init->children[e]);
                        }
                        fprintf(gen->output, ";\n");
                    }
                } else if (init) {
                    print_indent(gen);
                    fprintf(gen->output, "memcpy(actor->%s, ", child->value);
                    generate_expression(gen, init);
                    fprintf(gen->output, ", sizeof(actor->%s));\n", child->value);
                }
            } else if (child->child_count > 0) {
                print_indent(gen);
                fprintf(gen->output, "actor->%s = ", child->value);
                generate_expression(gen, child->children[0]);
                fprintf(gen->output, ";\n");
            } else {
                print_line(gen, "actor->%s = 0;", child->value);
            }
            /* #2528: the block is not zeroed (a kept one is reused), so the
             * tracker is set here: the initializer's value is owned when it
             * is a fresh heap string. */
            if (state_field_owns_string(child)) {
                int owned = child->child_count > 0 && child->children[0] &&
                            is_heap_string_expr(gen, child->children[0]);
                print_line(gen, "actor->_heap_%s = %d;", child->value, owned ? 1 : 0);
            }
        }
    }
    print_line(gen, "actor->destroy_state = %s_destroy_state;   /* #2528 */", actor->value);

    // Auto-initialize "my_ref" to the actor's own pointer so it is valid
    // immediately after spawn — no Setup message needed.  This eliminates
    // the race window where my_ref is 0 if Spawn arrives before Setup on
    // a different core. Only a `my_ref` the actor uses as a reference: one
    // used as a number is an ordinary field (#2466).
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        if (child->type == AST_STATE_DECLARATION &&
            strcmp(child->value, "my_ref") == 0 &&
            state_field_is_ptr(gen, actor, child->value)) {
            print_line(gen, "actor->my_ref = (void*)actor;  // self-ref available immediately (no Setup needed)");
            break;
        }
    }

    print_line(gen, "");
    print_line(gen, "#if AETHER_HAS_THREADS");
    print_line(gen, "if (actor->auto_process) {");
    indent(gen);
    // Without its thread the actor is an ordinary one, stepped by its core
    // and ended by its release (#2517).
    print_line(gen, "if (pthread_create(&actor->thread, NULL, (void*(*)(void*))aether_actor_thread, actor) != 0) {");
    indent(gen);
    print_line(gen, "actor->thread = 0;");
    print_line(gen, "actor->auto_process = 0;");
    unindent(gen);
    print_line(gen, "}");
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "#endif");

    // Set timeout if actor has a receive ... after N clause.
    //
    // The timeout expression runs HERE, in spawn_<Actor>, with `actor`
    // in scope as the freshly-allocated handle — `self` doesn't exist
    // yet, so any state-field reference (`after m_interval_ms`) must
    // resolve to `actor->m_interval_ms`. We swap the codegen's
    // state-self alias to "actor" for the duration of the expression
    // and restore it after. Without this swap the user is forced to
    // hardcode a literal (`after 1000`) and roll their own elapsed-
    // time math against `clock_ns()` — the workaround a real-world
    // site-poller had to carry. State has already been initialised
    // a few lines up, so the value the expression reads is the same
    // value the actor will see once it starts running.
    if (timeout_arm && timeout_arm->child_count >= 1) {
        print_line(gen, "// Receive timeout (milliseconds -> nanoseconds)");
        print_indent(gen);
        fprintf(gen->output, "actor->timeout_ns = (uint64_t)(");
        const char* prev_alias = gen->state_self_alias;
        gen->state_self_alias = "actor";
        generate_expression(gen, timeout_arm->children[0]);
        gen->state_self_alias = prev_alias;
        fprintf(gen->output, ") * 1000000ULL;\n");
        print_line(gen, "actor->last_activity_ns = (uint64_t)_aether_clock_ns();  // Start timeout countdown at spawn");
        print_line(gen, "atomic_store_explicit(&actor->active, 1, memory_order_release);  // Activate for timeout polling");
    }

    print_line(gen, "");
    print_line(gen, "return actor;");
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");

    print_line(gen, "void send_%s(%s* actor, int type, int payload) {", actor->value, actor->value);
    indent(gen);
    print_line(gen, "Message msg = {type, 0, payload, NULL};");
    print_line(gen, "if (atomic_load_explicit(&actor->assigned_core, memory_order_relaxed) == aether_core_id_get()) {");
    indent(gen);
    print_line(gen, "scheduler_send_local((ActorBase*)actor, msg);");
    unindent(gen);
    print_line(gen, "} else {");
    indent(gen);
    print_line(gen, "scheduler_send_remote((ActorBase*)actor, msg, aether_core_id_get());");
    unindent(gen);
    print_line(gen, "}");
    unindent(gen);
    print_line(gen, "}");
    print_line(gen, "");
    
    if (gen->current_actor) free(gen->current_actor);
    gen->current_actor = NULL;
    if (gen->actor_state_vars) {
        for (int i = 0; i < gen->state_var_count; i++) {
            free(gen->actor_state_vars[i]);
        }
        free(gen->actor_state_vars);
        gen->actor_state_vars = NULL;
    }
    gen->state_var_count = 0;
    gen->actor_count++;
}
