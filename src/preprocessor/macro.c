#include "input.h"
#include "macro.h"
#include "strtab.h"
#include "tokenize.h"
#include <lacc/cli.h>
#include <lacc/hash.h>
#include <lacc/list.h>

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#define HASH_TABLE_BUCKETS 1024

static struct hash_table macro_hash_table;
static int new_macro_added;

static int macrocmp(const struct macro *a, const struct macro *b)
{
    int i;

    if ((a->type != b->type) || (a->params != b->params))
        return 1;

    if (tok_cmp(a->name, b->name))
        return 1;

    if (array_len(&a->replacement) != array_len(&b->replacement))
        return 1;

    for (i = 0; i < array_len(&a->replacement); ++i) {
        if (tok_cmp(
                array_get(&a->replacement, i),
                array_get(&b->replacement, i)))
            return 1;
    }

    return 0;
}

static struct string macro_hash_key(void *ref)
{
    return ((struct macro *) ref)->name.d.string;
}

static void macro_hash_del(void *ref)
{
    struct macro *macro = (struct macro *) ref;
    array_clear(&macro->replacement);
    free(macro);
}

static void *macro_hash_add(void *ref)
{
    struct macro *macro, *arg;

    arg = (struct macro *) ref;
    macro = calloc(1, sizeof(*macro));
    *macro = *arg;

    /* Signal that the hash table has ownership now, and it will not be
     * freed in define(). */
    new_macro_added = 1;
    return macro;
}

static void cleanup(void)
{
    hash_destroy(&macro_hash_table);
}

static void ensure_initialized(void)
{
    static int done;

    if (!done) {
        hash_init(
            &macro_hash_table,
            HASH_TABLE_BUCKETS,
            macro_hash_key,
            macro_hash_add,
            macro_hash_del);
        atexit(cleanup);
        done = 1;
    }
}

const struct macro *definition(struct token name)
{
    struct macro *ref = NULL;
    struct token *tok;

    ensure_initialized();
    if (name.token == IDENTIFIER) {
        ref = hash_lookup(&macro_hash_table, name.d.string);
        if (ref) {
            /* Replace __LINE__ with current line number, by mutating
             * the replacement list on the fly. */
            if (!str_cmp(ref->name.d.string, str_init("__LINE__"))) {
                tok = &array_get(&ref->replacement, 0);
                tok->d.number.val.i = current_file_line();
            }
        }
    }

    return ref;
}

void define(struct macro macro)
{
    struct macro *ref;

    ensure_initialized();
    new_macro_added = 0;
    ref = hash_insert(&macro_hash_table, &macro);
    if (macrocmp(ref, &macro)) {
        error("Redefinition of macro '%s' with different substitution.",
            macro.name.d.string.str);
        exit(1);
    }

    /* Need to clean up memory for replacement list since ownership was
     * not given to hash table. */
    if (!new_macro_added) {
        array_clear(&macro.replacement);
    }
}

void undef(struct token name)
{
    ensure_initialized();
    if (name.token == IDENTIFIER) {
        hash_remove(&macro_hash_table, name.d.string);
    }
}

/* Keep track of which macros have been expanded, avoiding recursion by
 * looking up in this list for each new expansion.
 */
static struct list expand_stack;

static int is_macro_expanded(const struct macro *macro)
{
    int i;
    const struct macro *other;

    for (i = 0; i < list_len(&expand_stack); ++i) {
        other = (const struct macro *) list_get(&expand_stack, i);
        if (!tok_cmp(other->name, macro->name))
            return 1;
    }

    return 0;
}

static void push_expand_stack(const struct macro *macro)
{
    assert(!is_macro_expanded(macro));
    list_push(&expand_stack, (void *) macro);
}

static void pop_expand_stack(void)
{
    assert(list_len(&expand_stack));
    list_pop(&expand_stack);
    if (!list_len(&expand_stack)) {
        list_clear(&expand_stack, NULL);
    }
}

/* Calculate length of list, excluding trailing END marker.
 */
static size_t len(const struct token *list)
{
    size_t i = 0;
    assert(list);
    while (list[i].token != END)
        i++;
    return i;
}

void print_list(const struct token *list)
{
    int first = 1;
    size_t l = len(list);
    printf("[");
    while (list->token != END) {
        if (!first)
            printf(", ");
        printf("'");
        if (list->leading_whitespace > 0) {
            printf("%*s", list->leading_whitespace, " ");
        }
        if (list->token == NEWLINE)
            printf("\\n");
        else
            printf("%s", list->d.string.str);
        printf("'");
        first = 0;
        list++;
    }
    printf("] (%lu)\n", l);
}

/* Extend input list with concatinating another list to it. Takes
 * ownership of both arguments.
 */
static struct token *concat(struct token *list, struct token *other)
{
    size_t i = len(list);
    size_t j = len(other);

    list = realloc(list, (i + j + 1) * sizeof(*list));
    memmove(list + i, other, (j + 1) * sizeof(*list));
    assert(list[i + j].token == END);
    free(other);
    return list;
}

/* Extend input list by a single token. Take ownership of input.
 */
static struct token *append(struct token *list, struct token other)
{
    size_t i = len(list);

    assert(list[i].token == END);
    list = realloc(list, (i + 2) * sizeof(*list));
    list[i + 1] = list[i];
    list[i] = other;
    return list;
}

static struct token *copy(const struct token *list)
{
    size_t i = len(list) + 1;
    struct token *c = calloc(i, sizeof(*c));

    return memcpy(c, list, i * sizeof(*c));
}

/* Paste together two tokens.
 */
static struct token paste(struct token left, struct token right)
{
    struct token result;
    size_t length;
    char *data, *endptr;

    length = left.d.string.len + right.d.string.len;
    data   = calloc(length + 1, sizeof(*data));
    data   = strcpy(data, left.d.string.str);
    data   = strcat(data, right.d.string.str);
    result = tokenize(data, &endptr);
    if (endptr != data + length) {
        error("Invalid token resulting from pasting '%s' and '%s'.",
            left.d.string.str, right.d.string.str);
        exit(1);
    }

    result.leading_whitespace = left.leading_whitespace;
    free(data);
    return result;
}

/* In-place expansion of token paste operators.
 * ['foo', '##', '_f', '##', 'u', '##', 'nc'] becomes ['foo_func']
 */
static struct token *expand_paste_operators(struct token *list)
{
    struct token
        *ptr = list,
        *end = list + 1;

    if (list->token == END)
        return list;

    if (list->token == TOKEN_PASTE) {
        error("Unexpected token paste operator at beginning of line.");
        exit(1);
    }

    while (end->token != END) {
        if (end->token == TOKEN_PASTE) {
            end++;
            if (end->token == END) {
                error("Unexpected token paste operator at end of line.");
                exit(1);
            }
            *ptr = paste(*ptr, *end);
            end++;
        } else {
            *(++ptr) = *end++;
            assert(end->token != NEWLINE);
        }
    }

    *(ptr + 1) = *end;
    return list;
}

static struct token *expand_macro(
    const struct macro *macro,
    struct token *args[])
{
    int i, param;
    struct token *res = calloc(1, sizeof(*res));
    struct token *tok;

    res[0] = basic_token[END];
    push_expand_stack(macro);
    for (i = 0; i < array_len(&macro->replacement); ++i) {
        tok = &array_get(&macro->replacement, i);
        if (tok->token == PARAM) {
            /* Create a copy of args before expanding to avoid it being
             * free'd. */
            param = tok->d.number.val.i;
            res = concat(res, expand(copy(args[param])));
        } else if (
            i < array_len(&macro->replacement) - 1 &&
            tok->token == '#' &&
            (tok + 1)->token == PARAM)
        {
            i++;
            param = (tok + 1)->d.number.val.i;
            res = append(res, stringify(args[param]));
        } else {
            res = append(res, *tok);
        }
    }
    res = expand_paste_operators(res);
    res = expand(res);
    pop_expand_stack();

    for (i = 0; i < macro->params; ++i) {
        free(args[i]);
    }
    free(args);
    return res;
}

static const struct token *skip(const struct token *list, enum token_type token)
{
    if (list->token != token) {
        assert(basic_token[token].d.string.str);
        error("Expected '%s', but got '%s'.",
            basic_token[token].d.string.str, list->d.string.str);
    }

    list++;
    assert(list->token != NEWLINE || (list + 1)->token == END);
    return list;
}

/* Read argument in macro expansion, starting from one offset from the
 * initial open parenthesis. Stop readin when reaching a comma, and
 * nesting depth is zero. Track nesting depth to allow things like
 * MAX( foo(a), b ).
 */
static struct token *read_arg(
    const struct token *list,
    const struct token **endptr)
{
    size_t n = 0;
    struct token *arg = calloc(1, sizeof(*arg));
    int nesting = 0;

    do {
        if (list->token == END) {
            error("Unexpected end of input in expansion.");
            exit(1);
        }
        if (list->token == '(') {
            nesting++;
        } else if (list->token == ')') {
            nesting--;
            if (nesting < 0) {
                error("Negative nesting depth in expansion.");
                exit(1);
            }
        }
        arg = realloc(arg, (++n + 1) * sizeof(*arg));
        arg[n - 1] = *list++;
    } while (nesting || (list->token != ',' && list->token != ')'));

    arg[n] = basic_token[END];
    *endptr = list;
    return arg;
}

static struct token **read_args(
    const struct token *list,
    const struct token **endptr,
    const struct macro *macro)
{
    struct token **args = calloc(macro->params, sizeof(*args));
    int n = 0;

    if (macro->type == FUNCTION_LIKE) {
        list = skip(list, '(');
        for (; n < macro->params; ++n) {
            args[n] = read_arg(list, &list);
            if (n < macro->params - 1) {
                list = skip(list, ',');
            }
        }
        list = skip(list, ')');
    }

    *endptr = list;
    return args;
}

static int needs_expansion(const struct token *list)
{
    const struct macro *def;

    while (list->token != END) {
        def = definition(*list);
        if (def && !is_macro_expanded(def))
            return 1;
        list++;
    }

    return 0;
}

struct token *expand(struct token *original)
{
    const struct token *list;
    struct token *res;

    /* Do nothing if there is nothing to expand. */
    if (!needs_expansion(original))
        return original;

    /*printf("Expanding ");
    print_list(original);*/

    list = original;
    res = calloc(1, sizeof(*res));
    res[0] = basic_token[END];
    while (list->token != END) {
        const struct macro *def = definition(*list);
        int leading_whitespace = list->leading_whitespace;
        struct token *expn;
        struct token **args;

        /* Only expand function-like macros if they appear as function
         * invocations, beginning with an open paranthesis. */
        if (def && !is_macro_expanded(def) &&
            (def->type != FUNCTION_LIKE || (list + 1)->token == '('))
        {
            args = read_args(list + 1, &list, def);
            expn = expand_macro(def, args);

            /* Dirty fix for adding whitespace after expansion. Fill in
             * correct number of spaces from the expanded token. */
            expn->leading_whitespace = leading_whitespace;
            res = concat(res, expn);
        } else {
            res = append(res, *list++);
        }
    }

    /*printf("Result: ");
    print_list(res);*/

    free(original);
    return res;
}

int tok_cmp(struct token a, struct token b)
{
    if (a.token != b.token)
        return 1;

    if (a.token == PARAM) {
        return a.d.number.val.i != b.d.number.val.i;
    } else if (a.token == NUMBER) {
        if (!type_equal(a.d.number.type, b.d.number.type))
            return 1;
        return
            (a.d.number.type->type == T_UNSIGNED) ?
                a.d.number.val.u != b.d.number.val.u :
                a.d.number.val.i != b.d.number.val.i;
    } else {
        return str_cmp(a.d.string, b.d.string);
    }
}

/* From GCC documentation: All leading and trailing whitespace in text
 * being stringified is ignored. Any sequence of whitespace in the
 * middle of the text is converted to a single space in the stringified
 * result.
 */
struct token stringify(const struct token list[])
{
    int n = 0;
    size_t len = 0;
    struct token t = {STRING};
    struct string strval;
    char *buf = calloc(1, sizeof(*buf));

    while (list->token != END) {
        assert(list->token != NEWLINE);

        /* Reduce to a single space, and only insert between other
         * tokens in the list. */
        strval = tokstr(*list);
        len += strval.len + (list->leading_whitespace && n);
        buf = realloc(buf, (len + 1) * sizeof(*buf));
        if (n && list->leading_whitespace) {
            buf[len - strval.len - 1] = ' ';
            buf[len - strval.len] = '\0';
        }

        buf = strncat(buf, strval.str, len);
        list++;
        n++;
    }

    t.d.string = str_register(buf, len);
    free(buf);
    return t;
}

static TokenArray parse(char *str)
{
    char *endptr;
    struct token param = {PARAM};
    TokenArray arr = {0};

    while (*str) {
        if (*str == '@') {
            array_push_back(&arr, param);
            str++;
        } else {
            array_push_back(&arr, tokenize(str, &endptr));
            assert(str != endptr);
            str = endptr;
        }
    }

    return arr;
}

static void register__builtin_va_end(void)
{
    struct macro macro = {
        {IDENTIFIER},
        FUNCTION_LIKE,
        1, /* parameters */
    };

    macro.name.d.string = str_init("__builtin_va_end");
    macro.replacement = parse(
        "@[0].gp_offset=0;"
        "@[0].fp_offset=0;"
        "@[0].overflow_arg_area=(void*)0;"
        "@[0].reg_save_area=(void*)0;");

    assert(array_len(&macro.replacement) == 44);
    define(macro);
}

static void register__builtin__FILE__(void)
{
    struct token file = {STRING};
    struct macro macro = {
        {IDENTIFIER},
        OBJECT_LIKE,
        0, /* parameters */
    };

    file.d.string = str_init(current_file_path());
    array_push_back(&macro.replacement, file);

    macro.name.d.string = str_init("__FILE__");
    define(macro);
}

void register_builtin_definitions(void)
{
    struct macro macro = {
        {IDENTIFIER},
        OBJECT_LIKE,
        0, /* parameters */
    };

    macro.name.d.string = str_init("__STDC_VERSION__");
    macro.replacement = parse("199409L");
    define(macro);

    macro.name.d.string = str_init("__STDC__");
    macro.replacement = parse("1");
    define(macro);

    macro.name.d.string = str_init("__STDC_HOSTED__");
    macro.replacement = parse("1");
    define(macro);

    macro.name.d.string = str_init("__LINE__");
    macro.replacement = parse("0");
    define(macro);

    macro.name.d.string = str_init("__x86_64__");
    macro.replacement = parse("1");
    define(macro);

    /* For some reason this is not properly handled by musl. */
    macro.name.d.string = str_init("__inline");
    macro.replacement = parse(" ");
    define(macro);

    register__builtin__FILE__();
    register__builtin_va_end();
}
