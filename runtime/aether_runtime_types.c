// Runtime Type Checking Implementation
#include "aether_runtime_types.h"
#include "aether_locale_num.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Convert RuntimeTypeCode to string
const char* type_code_to_string(RuntimeTypeCode code) {
    switch (code) {
        case RUNTIME_TYPE_INT: return "int";
        case RUNTIME_TYPE_FLOAT: return "float";
        case RUNTIME_TYPE_BOOL: return "bool";
        case RUNTIME_TYPE_STRING: return "string";
        case RUNTIME_TYPE_ARRAY: return "array";
        case RUNTIME_TYPE_STRUCT: return "struct";
        case RUNTIME_TYPE_ACTOR_REF: return "actor_ref";
        case RUNTIME_TYPE_VOID: return "void";
        case RUNTIME_TYPE_UNKNOWN: return "unknown";
        default: return "unknown";
    }
}

// Convert string to RuntimeTypeCode
RuntimeTypeCode string_to_type_code(const char* type_name) {
    if (strcmp(type_name, "int") == 0) return RUNTIME_TYPE_INT;
    if (strcmp(type_name, "float") == 0) return RUNTIME_TYPE_FLOAT;
    if (strcmp(type_name, "bool") == 0) return RUNTIME_TYPE_BOOL;
    if (strcmp(type_name, "string") == 0) return RUNTIME_TYPE_STRING;
    if (strcmp(type_name, "array") == 0) return RUNTIME_TYPE_ARRAY;
    if (strcmp(type_name, "struct") == 0) return RUNTIME_TYPE_STRUCT;
    if (strcmp(type_name, "actor_ref") == 0) return RUNTIME_TYPE_ACTOR_REF;
    if (strcmp(type_name, "void") == 0) return RUNTIME_TYPE_VOID;
    return RUNTIME_TYPE_UNKNOWN;
}

// typeof(value) - Returns the type of a runtime value as a string
const char* aether_typeof(RuntimeValue* val) {
    if (!val) return "void";
    return type_code_to_string(val->type);
}

// is_type(value, type_name) - Check if value matches type
int aether_is_type(RuntimeValue* val, const char* type_name) {
    if (!val) return strcmp(type_name, "void") == 0;
    RuntimeTypeCode expected = string_to_type_code(type_name);
    return val->type == expected;
}

// convert_type(value, target_type) - Attempt type conversion
RuntimeValue* aether_convert_type(RuntimeValue* val, const char* target_type) {
    if (!val) return NULL;
    
    RuntimeValue* result = (RuntimeValue*)malloc(sizeof(RuntimeValue));
    if (!result) return NULL;   // caller already handles a NULL result (see `!val` above)
    RuntimeTypeCode target_code = string_to_type_code(target_type);
    result->type = target_code;

    // Convert from int
    if (val->type == RUNTIME_TYPE_INT) {
        if (target_code == RUNTIME_TYPE_FLOAT) {
            result->value.float_val = (double)val->value.int_val;
            return result;
        } else if (target_code == RUNTIME_TYPE_STRING) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", (long long)val->value.int_val);
            result->value.string_val = strdup(buffer);
            if (!result->value.string_val) { free(result); return NULL; }
            return result;
        } else if (target_code == RUNTIME_TYPE_BOOL) {
            result->value.bool_val = val->value.int_val != 0;
            return result;
        }
    }
    
    // Convert from float
    if (val->type == RUNTIME_TYPE_FLOAT) {
        if (target_code == RUNTIME_TYPE_INT) {
            result->value.int_val = (int64_t)val->value.float_val;
            return result;
        } else if (target_code == RUNTIME_TYPE_STRING) {
            char buffer[32];
            // Locale-pinned — this is machine text (see aether_locale_num.h).
            aether_c_snprintf_double(buffer, sizeof(buffer), "%f",
                                     val->value.float_val);
            result->value.string_val = strdup(buffer);
            if (!result->value.string_val) { free(result); return NULL; }
            return result;
        } else if (target_code == RUNTIME_TYPE_BOOL) {
            result->value.bool_val = val->value.float_val != 0.0;
            return result;
        }
    }
    
    // Convert from string
    if (val->type == RUNTIME_TYPE_STRING) {
        if (target_code == RUNTIME_TYPE_INT) {
            result->value.int_val = atoll(val->value.string_val);
            return result;
        } else if (target_code == RUNTIME_TYPE_FLOAT) {
            // aether_c_strtod, not atof: locale-pinned (atof follows
            // LC_NUMERIC, so "3.14" reads as 3.0 under a comma-decimal host).
            result->value.float_val =
                aether_c_strtod(val->value.string_val, NULL);
            return result;
        } else if (target_code == RUNTIME_TYPE_BOOL) {
            result->value.bool_val = val->value.string_val &&
                                    strlen(val->value.string_val) > 0;
            return result;
        }
    }
    
    // Convert from bool
    if (val->type == RUNTIME_TYPE_BOOL) {
        if (target_code == RUNTIME_TYPE_INT) {
            result->value.int_val = val->value.bool_val ? 1 : 0;
            return result;
        } else if (target_code == RUNTIME_TYPE_FLOAT) {
            result->value.float_val = val->value.bool_val ? 1.0 : 0.0;
            return result;
        } else if (target_code == RUNTIME_TYPE_STRING) {
            result->value.string_val = strdup(val->value.bool_val ? "true" : "false");
            if (!result->value.string_val) { free(result); return NULL; }
            return result;
        }
    }
    
    // No conversion possible - return a copy of the input unchanged. Reuse the
    // already-allocated (and NULL-checked) result rather than free+malloc again,
    // which would reintroduce an unchecked allocation dereferenced right below.
    *result = *val;
    return result;
}
