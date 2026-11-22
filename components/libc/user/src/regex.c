/*
 * Micro OS — the regular expressions of `grep` and `sed`.
 *
 * A POSIX regular expression, basic or extended: the literals, `.`, `*`, `+`,
 * `?`, `{m,n}`, `^`, `$`, a group, an alternation and a back reference, and a
 * bracket expression with ranges and the classes `[:alpha:]` and its family.
 * The engine is small on purpose and has no locale: a byte is a character, a
 * class is the ASCII one, and a multibyte character of a file is a run of
 * bytes (the caller of `sed` handles the runes).
 *
 * The expression is parsed into a tree and compiled to a little program of
 * instructions: a character, a class, a split with a preferred branch (the
 * greedy order), a jump, and the two saves of a group. A match is the program
 * walked from the left of the text, backtracking on a split; a visited bit of
 * a (instruction, position) pair stops the walk from running in a circle on an
 * expression that can match nothing inside a repetition (`\(a*\)*`).
 *
 * The match is the leftmost one; the greedy order makes it the longest in the
 * ordinary cases, which is what `sed` and `grep` need. The engine is not a
 * theorem: `POSIX` asks for the longest of every group as well, and a pattern
 * where the two orders disagree takes the first that the walk finds.
 */
#include <regex.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define REGEX_MAX_PROGRAM 4096
#define REGEX_MAX_GROUPS 10
#define REGEX_MAX_DEPTH 32
#define REGEX_CLASS_BYTES 32

enum node_type {
    NODE_EMPTY,
    NODE_CHAR,
    NODE_ANY,
    NODE_CLASS,
    NODE_BOL,
    NODE_EOL,
    NODE_GROUP,
    NODE_BACKREF,
    NODE_CAT,
    NODE_ALT,
    NODE_STAR,
    NODE_PLUS,
    NODE_QUEST,
    NODE_REPEAT,
};

struct node {
    enum node_type type;
    int ch;
    int index;
    int minimum;
    int maximum; /* -1 is unbounded */
    unsigned char *bits;
    struct node *left;
    struct node *right;
    struct node *allocated_next;
};

enum opcode {
    I_CHAR,
    I_ANY,
    I_CLASS,
    I_BOL,
    I_EOL,
    I_SAVE,
    I_BACKREF,
    I_SPLIT,
    I_JMP,
    I_MATCH,
};

struct instruction {
    enum opcode op;
    int a;
    int b;
    unsigned char ch;
    const unsigned char *bits;
};

struct program {
    struct instruction *code;
    int count;
    int capacity;
    int groups;
    unsigned char newline;
    unsigned char icase;
};

struct parser {
    const char *text;
    const char *place;
    int extended;
    int flags;
    int groups;
    int depth;
    int error;
    struct node *pool;
    struct program code;
};

/* --- the tree --- */

static struct node *node_new(struct parser *parser, enum node_type type)
{
    struct node *node = (struct node *)calloc(1, sizeof(*node));

    if (node == NULL) {
        parser->error = REG_ESPACE;
        return NULL;
    }

    node->type = type;
    node->allocated_next = parser->pool;
    parser->pool = node;

    return node;
}

static void tree_free(struct node *node)
{
    while (node != NULL) {
        struct node *next = node->allocated_next;

        if (node->bits != NULL) {
            free(node->bits);
        }
        free(node);
        node = next;
    }
}

static int class_set(unsigned char *bits, int byte)
{
    bits[(unsigned char)byte / 8] |= (unsigned char)(1u << ((unsigned char)byte % 8));

    return 0;
}

static int class_has(const unsigned char *bits, int byte)
{
    return (bits[(unsigned char)byte / 8] >> ((unsigned char)byte % 8)) & 1u;
}

static int is_word(int byte)
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

/* The class of a bracket expression, the names of POSIX. */
static int named_class(unsigned char *bits, const char *name, size_t length)
{
    int byte;

    for (byte = 0; byte < 256; byte++) {
        int match = 0;

        if (length == 5 && strncmp(name, "alpha", 5) == 0) {
            match = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        } else if (length == 5 && strncmp(name, "digit", 5) == 0) {
            match = byte >= '0' && byte <= '9';
        } else if (length == 5 && strncmp(name, "alnum", 5) == 0) {
            match = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                    (byte >= '0' && byte <= '9');
        } else if (length == 5 && strncmp(name, "lower", 5) == 0) {
            match = byte >= 'a' && byte <= 'z';
        } else if (length == 5 && strncmp(name, "upper", 5) == 0) {
            match = byte >= 'A' && byte <= 'Z';
        } else if (length == 5 && strncmp(name, "space", 5) == 0) {
            match = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' ||
                    byte == '\f';
        } else if (length == 5 && strncmp(name, "blank", 5) == 0) {
            match = byte == ' ' || byte == '\t';
        } else if (length == 5 && strncmp(name, "punct", 5) == 0) {
            match = byte > 32 && byte < 127 && !is_word(byte);
        } else if (length == 5 && strncmp(name, "cntrl", 5) == 0) {
            match = byte < 32 || byte == 127;
        } else if (length == 5 && strncmp(name, "print", 5) == 0) {
            match = byte >= 32 && byte < 127;
        } else if (length == 5 && strncmp(name, "graph", 5) == 0) {
            match = byte > 32 && byte < 127;
        } else if (length == 6 && strncmp(name, "xdigit", 6) == 0) {
            match = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                    (byte >= 'A' && byte <= 'F');
        } else {
            return REG_ECTYPE;
        }

        if (match) {
            class_set(bits, byte);
        }
    }

    return 0;
}

/* --- the parser --- */

static int at_end(const struct parser *parser)
{
    return *parser->place == '\0';
}

static int peek_mark(const struct parser *parser, char basic, char open)
{
    if (parser->extended) {
        return *parser->place == open;
    }
    if (parser->place[0] == '\\' && parser->place[1] == basic) {
        return 1;
    }

    return 0;
}

static int eat(struct parser *parser, char basic, char open)
{
    if (!peek_mark(parser, basic, open)) {
        return 0;
    }
    parser->place += parser->extended ? 1 : 2;

    return 1;
}

static int at_close(const struct parser *parser)
{
    return peek_mark(parser, ')', ')');
}

static int at_alternation(const struct parser *parser)
{
    return peek_mark(parser, '|', '|');
}

/* Is this place the end of the branch: the end of the text, a close of a
 * group or an alternation? A `$` is the end of a line only there. */
static int at_branch_end(const struct parser *parser, const char *place)
{
    if (*place == '\0') {
        return 1;
    }
    if (parser->extended) {
        return *place == ')' || *place == '|';
    }

    return place[0] == '\\' && (place[1] == ')' || place[1] == '|');
}

static int literal_escape(const struct parser *parser)
{
    return parser->place[0] == '\\' && parser->place[1] != '\0';
}

/* The class of a bracket expression, after its `[`. */
static struct node *parse_class(struct parser *parser)
{
    struct node *node = node_new(parser, NODE_CLASS);
    int invert = 0;
    int first = 1;

    if (node == NULL) {
        return NULL;
    }
    node->bits = (unsigned char *)calloc(1, REGEX_CLASS_BYTES);
    if (node->bits == NULL) {
        parser->error = REG_ESPACE;
        return NULL;
    }

    if (*parser->place == '^' || *parser->place == '!') {
        invert = 1;
        parser->place++;
    }

    for (;;) {
        int low;

        if (*parser->place == '\0') {
            parser->error = REG_EBRACK;
            return node;
        }
        if (*parser->place == ']' && !first) {
            parser->place++;
            break;
        }
        first = 0;

        if (parser->place[0] == '[' && parser->place[1] == ':') {
            const char *close = strstr(parser->place + 2, ":]");
            int rc;

            if (close != NULL) {
                rc = named_class(node->bits, parser->place + 2,
                                 (size_t)(close - (parser->place + 2)));
                if (rc != 0) {
                    parser->error = rc;
                    return node;
                }
                parser->place = close + 2;
                continue;
            }
        }

        if (literal_escape(parser)) {
            parser->place++;
        }
        low = (unsigned char)*parser->place++;

        if (*parser->place == '-' && parser->place[1] != ']' && parser->place[1] != '\0') {
            int high;

            parser->place++;
            if (literal_escape(parser)) {
                parser->place++;
            }
            high = (unsigned char)*parser->place++;
            if (high < low) {
                parser->error = REG_ERANGE;
                return node;
            }
            for (int byte = low; byte <= high; byte++) {
                class_set(node->bits, byte);
            }
        } else {
            class_set(node->bits, low);
        }
    }

    if ((parser->flags & REG_ICASE) != 0) {
        for (int byte = 'A'; byte <= 'Z'; byte++) {
            if (class_has(node->bits, byte)) {
                class_set(node->bits, byte + ('a' - 'A'));
            }
            if (class_has(node->bits, byte + ('a' - 'A'))) {
                class_set(node->bits, byte);
            }
        }
    }

    if (invert) {
        for (int byte = 0; byte < 256; byte++) {
            node->bits[byte / 8] ^= (unsigned char)(1u << (byte % 8));
        }
        if ((parser->flags & REG_NEWLINE) != 0) {
            /* `.` and a negated set stop at a newline in this mode. */
            node->bits['\n' / 8] &= (unsigned char)~(1u << ('\n' % 8));
        }
    }

    return node;
}

static struct node *node_cat(struct parser *parser, struct node *left, struct node *right)
{
    struct node *node;

    if (left == NULL || right == NULL) {
        parser->error = REG_ESPACE;
        return left;
    }
    if (left->type == NODE_EMPTY) {
        return right;
    }
    if (right->type == NODE_EMPTY) {
        return left;
    }

    node = node_new(parser, NODE_CAT);
    if (node == NULL) {
        return NULL;
    }
    node->left = left;
    node->right = right;

    return node;
}

static struct node *parse_alternation(struct parser *parser);

/* An atom, and the postfix operators that follow it. */
static struct node *parse_atom(struct parser *parser, int at_start)
{
    struct node *node = NULL;
    int byte;

    if (*parser->place == '[') {
        parser->place++;
        return parse_class(parser);
    }
    if (*parser->place == '.') {
        parser->place++;
        return node_new(parser, NODE_ANY);
    }
    if (*parser->place == '^' && at_start) {
        parser->place++;
        return node_new(parser, NODE_BOL);
    }
    if (*parser->place == '$' && at_branch_end(parser, parser->place + 1)) {
        parser->place++;
        return node_new(parser, NODE_EOL);
    }
    if (parser->extended && *parser->place == '(') {
        parser->place++;
        if (parser->groups + 1 >= REGEX_MAX_GROUPS) {
            parser->error = REG_ESPACE;
            return NULL;
        }
        parser->groups++;
        node = node_new(parser, NODE_GROUP);
        if (node == NULL) {
            return NULL;
        }
        node->index = parser->groups;
        node->left = parse_alternation(parser);
        if (parser->error != 0) {
            return NULL;
        }
        if (!eat(parser, ')', ')')) {
            parser->error = REG_EPAREN;
            return NULL;
        }
        return node;
    }
    if (!parser->extended && parser->place[0] == '\\' && parser->place[1] == '(') {
        parser->place += 2;
        if (parser->groups + 1 >= REGEX_MAX_GROUPS) {
            parser->error = REG_ESPACE;
            return NULL;
        }
        parser->groups++;
        node = node_new(parser, NODE_GROUP);
        if (node == NULL) {
            return NULL;
        }
        node->index = parser->groups;
        node->left = parse_alternation(parser);
        if (parser->error != 0) {
            return NULL;
        }
        if (!eat(parser, ')', ')')) {
            parser->error = REG_EPAREN;
            return NULL;
        }
        return node;
    }

    if (literal_escape(parser)) {
        parser->place++;
        if (!parser->extended && *parser->place >= '1' && *parser->place <= '9') {
            node = node_new(parser, NODE_BACKREF);
            if (node == NULL) {
                return NULL;
            }
            node->index = *parser->place - '0';
            parser->place++;
            return node;
        }
    }

    byte = (unsigned char)*parser->place;
    if (byte == '\0') {
        return node_new(parser, NODE_EMPTY);
    }
    parser->place++;
    node = node_new(parser, NODE_CHAR);
    if (node != NULL) {
        if ((parser->flags & REG_ICASE) != 0 && byte >= 'A' && byte <= 'Z') {
            byte += 'a' - 'A';
        }
        node->ch = byte;
    }

    return node;
}

/* Is the repeat that starts at the place a `{m,n}`? */
static int parse_repeat_bounds(struct parser *parser, int *minimum, int *maximum)
{
    const char *place = parser->place;
    int low = 0;
    int high = 0;
    int any = 0;

    if (*place != '{') {
        return -1;
    }
    place++;

    while (*place >= '0' && *place <= '9') {
        low = low * 10 + (*place - '0');
        any = 1;
        place++;
    }
    if (!any) {
        return -1;
    }
    if (*place == '}') {
        /* `{m}` is `{m,m}`. */
        *minimum = low;
        *maximum = low;
        parser->place = place + 1;
        return 0;
    }
    if (*place != ',') {
        return -1;
    }
    place++;
    any = 0;
    while (*place >= '0' && *place <= '9') {
        high = high * 10 + (*place - '0');
        any = 1;
        place++;
    }
    if (*place != '}') {
        return -1;
    }

    *minimum = low;
    *maximum = any ? high : -1;
    parser->place = place + 1;

    return 0;
}

static struct node *parse_repeat(struct parser *parser, int at_start)
{
    struct node *node = parse_atom(parser, at_start);

    if (node == NULL || parser->error != 0) {
        return node;
    }

    for (;;) {
        int kind = -1;
        struct node *wrapper;

        if (*parser->place == '*') {
            kind = NODE_STAR;
            parser->place++;
        } else if (parser->extended && *parser->place == '+') {
            kind = NODE_PLUS;
            parser->place++;
        } else if (parser->extended && *parser->place == '?') {
            kind = NODE_QUEST;
            parser->place++;
        } else if (!parser->extended && parser->place[0] == '\\' && parser->place[1] == '+') {
            kind = NODE_PLUS;
            parser->place += 2;
        } else if (!parser->extended && parser->place[0] == '\\' && parser->place[1] == '?') {
            kind = NODE_QUEST;
            parser->place += 2;
        } else if (parser->extended && *parser->place == '{') {
            int minimum;
            int maximum;

            if (parse_repeat_bounds(parser, &minimum, &maximum) == 0) {
                wrapper = node_new(parser, NODE_REPEAT);
                if (wrapper == NULL) {
                    return NULL;
                }
                wrapper->left = node;
                wrapper->minimum = minimum;
                wrapper->maximum = maximum;
                node = wrapper;
                continue;
            }
            return node;
        } else if (!parser->extended && parser->place[0] == '\\' && parser->place[1] == '{') {
            int minimum;
            int maximum;
            const char *saved = parser->place;

            parser->place += 1;
            if (parse_repeat_bounds(parser, &minimum, &maximum) == 0) {
                wrapper = node_new(parser, NODE_REPEAT);
                if (wrapper == NULL) {
                    return NULL;
                }
                wrapper->left = node;
                wrapper->minimum = minimum;
                wrapper->maximum = maximum;
                node = wrapper;
                continue;
            }
            parser->place = saved;
            return node;
        } else {
            break;
        }

        if (node->type == NODE_EMPTY) {
            parser->error = REG_BADRPT;
            return NULL;
        }
        wrapper = node_new(parser, (enum node_type)kind);
        if (wrapper == NULL) {
            return NULL;
        }
        wrapper->left = node;
        node = wrapper;
    }

    return node;
}

static struct node *parse_concat(struct parser *parser)
{
    struct node *node = node_new(parser, NODE_EMPTY);
    int at_start = 1;

    if (node == NULL) {
        return NULL;
    }

    while (!at_end(parser) && !at_alternation(parser) && !at_close(parser)) {
        struct node *piece;

        if (parser->depth++ > REGEX_MAX_DEPTH) {
            parser->error = REG_EPAREN;
            return NULL;
        }
        piece = parse_repeat(parser, at_start);
        parser->depth--;
        if (piece == NULL || parser->error != 0) {
            return NULL;
        }
        node = node_cat(parser, node, piece);
        at_start = 0;
    }

    return node;
}

static struct node *parse_alternation(struct parser *parser)
{
    struct node *node = parse_concat(parser);

    if (node == NULL || parser->error != 0) {
        return node;
    }

    while (at_alternation(parser)) {
        struct node *right;
        struct node *wrapper;

        parser->place += parser->extended ? 1 : 2;
        right = parse_concat(parser);
        if (right == NULL || parser->error != 0) {
            return NULL;
        }
        wrapper = node_new(parser, NODE_ALT);
        if (wrapper == NULL) {
            return NULL;
        }
        wrapper->left = node;
        wrapper->right = right;
        node = wrapper;
    }

    return node;
}

/* --- the compiler --- */

static int compile_node(struct parser *parser, const struct node *node);

static int emit(struct parser *parser, enum opcode op, int a, int b, unsigned char ch,
                const unsigned char *bits)
{
    struct program *code = &parser->code;
    struct instruction *grown;

    if (code->count >= REGEX_MAX_PROGRAM) {
        parser->error = REG_ESPACE;
        return -1;
    }

    if (code->count == code->capacity) {
        int capacity = (code->capacity == 0) ? 64 : code->capacity * 2;

        grown = (struct instruction *)realloc(code->code, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            parser->error = REG_ESPACE;
            return -1;
        }
        code->code = grown;
        code->capacity = capacity;
    }

    code->code[code->count].op = op;
    code->code[code->count].a = a;
    code->code[code->count].b = b;
    code->code[code->count].ch = ch;
    code->code[code->count].bits = bits;

    return code->count++;
}

static int compile_star(struct parser *parser, const struct node *node, int plus)
{
    int split;
    int start = parser->code.count;

    if (plus) {
        if (compile_node(parser, node->left) != 0) {
            return -1;
        }
        split = emit(parser, I_SPLIT, start, -1, 0, NULL);
        if (split < 0) {
            return -1;
        }
        parser->code.code[split].b = parser->code.count;

        return 0;
    }

    split = emit(parser, I_SPLIT, -1, -1, 0, NULL);
    if (split < 0) {
        return -1;
    }
    parser->code.code[split].a = parser->code.count;
    if (compile_node(parser, node->left) != 0) {
        return -1;
    }
    if (emit(parser, I_JMP, split, 0, 0, NULL) < 0) {
        return -1;
    }
    parser->code.code[split].b = parser->code.count;

    return 0;
}

static int compile_node(struct parser *parser, const struct node *node)
{
    if (node == NULL) {
        return 0;
    }
    if (parser->error != 0) {
        return -1;
    }

    switch (node->type) {
    case NODE_EMPTY:
        return 0;
    case NODE_CHAR:
        return (emit(parser, I_CHAR, 0, 0, (unsigned char)node->ch, NULL) < 0) ? -1 : 0;
    case NODE_ANY:
        return (emit(parser, I_ANY, 0, 0, 0, NULL) < 0) ? -1 : 0;
    case NODE_CLASS:
        return (emit(parser, I_CLASS, 0, 0, 0, node->bits) < 0) ? -1 : 0;
    case NODE_BOL:
        return (emit(parser, I_BOL, 0, 0, 0, NULL) < 0) ? -1 : 0;
    case NODE_EOL:
        return (emit(parser, I_EOL, 0, 0, 0, NULL) < 0) ? -1 : 0;
    case NODE_BACKREF:
        return (emit(parser, I_BACKREF, node->index, 0, 0, NULL) < 0) ? -1 : 0;
    case NODE_GROUP:
        if (emit(parser, I_SAVE, 2 * node->index, 0, 0, NULL) < 0) {
            return -1;
        }
        if (compile_node(parser, node->left) != 0) {
            return -1;
        }
        return (emit(parser, I_SAVE, 2 * node->index + 1, 0, 0, NULL) < 0) ? -1 : 0;
    case NODE_CAT:
        if (compile_node(parser, node->left) != 0) {
            return -1;
        }
        return compile_node(parser, node->right);
    case NODE_ALT: {
        int split = emit(parser, I_SPLIT, -1, -1, 0, NULL);
        int jump;

        if (split < 0) {
            return -1;
        }
        parser->code.code[split].a = parser->code.count;
        if (compile_node(parser, node->left) != 0) {
            return -1;
        }
        jump = emit(parser, I_JMP, -1, 0, 0, NULL);
        if (jump < 0) {
            return -1;
        }
        parser->code.code[split].b = parser->code.count;
        if (compile_node(parser, node->right) != 0) {
            return -1;
        }
        parser->code.code[jump].a = parser->code.count;
        return 0;
    }
    case NODE_STAR:
        return compile_star(parser, node, 0);
    case NODE_PLUS:
        return compile_star(parser, node, 1);
    case NODE_QUEST: {
        int split = emit(parser, I_SPLIT, -1, -1, 0, NULL);

        if (split < 0) {
            return -1;
        }
        parser->code.code[split].a = parser->code.count;
        if (compile_node(parser, node->left) != 0) {
            return -1;
        }
        parser->code.code[split].b = parser->code.count;
        return 0;
    }
    case NODE_REPEAT: {
        int copy;

        if (node->minimum < 0 || (node->maximum >= 0 && node->maximum < node->minimum)) {
            parser->error = REG_BADBR;
            return -1;
        }
        for (copy = 0; copy < node->minimum; copy++) {
            if (compile_node(parser, node->left) != 0) {
                return -1;
            }
        }
        if (node->maximum < 0) {
            struct node loop;

            memset(&loop, 0, sizeof(loop));
            loop.type = NODE_STAR;
            loop.left = node->left;
            return compile_star(parser, &loop, 0);
        }
        for (copy = node->minimum; copy < node->maximum; copy++) {
            int split = emit(parser, I_SPLIT, -1, -1, 0, NULL);

            if (split < 0) {
                return -1;
            }
            parser->code.code[split].a = parser->code.count;
            if (compile_node(parser, node->left) != 0) {
                return -1;
            }
            parser->code.code[split].b = parser->code.count;
        }
        return 0;
    }
    default:
        break;
    }

    parser->error = REG_BADPAT;

    return -1;
}

/* --- the match --- */

struct matcher {
    const struct program *program;
    const char *text;
    size_t length;
    int *saves;
    unsigned char *visited;
    size_t width;
    int flags;
    size_t end;
};

static int run(struct matcher *matcher, int pc, size_t position)
{
    const struct program *program = matcher->program;

    for (;;) {
        const struct instruction *instruction;

        if (pc < 0 || pc >= program->count) {
            return 0;
        }
        instruction = &program->code[pc];

        switch (instruction->op) {
        case I_MATCH:
            matcher->end = position;
            return 1;
        case I_CHAR: {
            unsigned char byte = (unsigned char)matcher->text[position];
            unsigned char want = instruction->ch;

            if ((matcher->flags & REG_ICASE) != 0) {
                if (byte >= 'A' && byte <= 'Z') {
                    byte += 'a' - 'A';
                }
                if (want >= 'A' && want <= 'Z') {
                    want += 'a' - 'A';
                }
            }
            if (byte != want || position >= matcher->length) {
                return 0;
            }
            position++;
            pc++;
            break;
        }
        case I_ANY:
            if (position >= matcher->length) {
                return 0;
            }
            if ((program->newline != 0) && matcher->text[position] == '\n') {
                return 0;
            }
            position++;
            pc++;
            break;
        case I_CLASS: {
            unsigned char byte;

            if (position >= matcher->length) {
                return 0;
            }
            byte = (unsigned char)matcher->text[position];
            if ((matcher->flags & REG_ICASE) != 0 && byte >= 'A' && byte <= 'Z') {
                byte += 'a' - 'A';
            }
            if (!class_has(instruction->bits, byte)) {
                return 0;
            }
            position++;
            pc++;
            break;
        }
        case I_BOL:
            if (position == 0) {
                if ((matcher->flags & REG_NOTBOL) != 0) {
                    return 0;
                }
            } else if ((program->newline == 0) || matcher->text[position - 1] != '\n') {
                return 0;
            }
            pc++;
            break;
        case I_EOL:
            if (position == matcher->length) {
                if ((matcher->flags & REG_NOTEOL) != 0) {
                    return 0;
                }
            } else if ((program->newline == 0) || matcher->text[position] != '\n') {
                return 0;
            }
            pc++;
            break;
        case I_SAVE: {
            int slot = instruction->a;
            int previous = matcher->saves[slot];

            matcher->saves[slot] = (int)position;
            if (run(matcher, pc + 1, position)) {
                return 1;
            }
            matcher->saves[slot] = previous;
            return 0;
        }
        case I_BACKREF: {
            int group = instruction->a;
            int start = matcher->saves[2 * group];
            int end = matcher->saves[2 * group + 1];
            size_t length;

            if (start < 0 || end < start) {
                return 0;
            }
            length = (size_t)(end - start);
            if (position + length > matcher->length) {
                return 0;
            }
            if ((matcher->flags & REG_ICASE) != 0) {
                for (size_t i = 0; i < length; i++) {
                    int a = (unsigned char)matcher->text[start + (int)i];
                    int b = (unsigned char)matcher->text[position + i];

                    if (a >= 'A' && a <= 'Z') {
                        a += 'a' - 'A';
                    }
                    if (b >= 'A' && b <= 'Z') {
                        b += 'a' - 'A';
                    }
                    if (a != b) {
                        return 0;
                    }
                }
            } else if (memcmp(matcher->text + start, matcher->text + position, length) != 0) {
                return 0;
            }
            position += length;
            pc++;
            break;
        }
        case I_SPLIT: {
            size_t key;

            if (matcher->visited != NULL) {
                key = (size_t)pc * matcher->width + position;
                if ((matcher->visited[key / 8] >> (key % 8)) & 1u) {
                    return 0;
                }
                matcher->visited[key / 8] |= (unsigned char)(1u << (key % 8));
            }
            if (run(matcher, instruction->a, position)) {
                return 1;
            }
            if (run(matcher, instruction->b, position)) {
                return 1;
            }
            return 0;
        }
        case I_JMP:
            pc = instruction->a;
            break;
        default:
            return 0;
        }
    }
}

/* --- the public calls --- */

static const char *const reg_errors[] = {
    "no error",
    "no match",
    "invalid pattern",
    "invalid collating element",
    "invalid character class",
    "trailing backslash",
    "invalid back reference",
    "unbalanced brackets",
    "unbalanced parentheses",
    "unbalanced braces",
    "invalid repeat count",
    "invalid character range",
    "out of memory",
    "nothing to repeat",
};

int regcomp(regex_t *expression, const char *pattern, int flags)
{
    struct parser parser;
    struct node *tree;
    int rc;

    if (expression == NULL || pattern == NULL) {
        return REG_BADPAT;
    }

    memset(expression, 0, sizeof(*expression));
    memset(&parser, 0, sizeof(parser));
    parser.text = pattern;
    parser.place = pattern;
    parser.extended = (flags & REG_EXTENDED) != 0;
    parser.flags = flags;
    parser.code.icase = (unsigned char)((flags & REG_ICASE) != 0);

    tree = parse_alternation(&parser);
    if (parser.error != 0) {
        tree_free(parser.pool);
        free(parser.code.code);
        return parser.error;
    }
    if (!at_end(&parser)) {
        tree_free(parser.pool);
        free(parser.code.code);
        return REG_EPAREN;
    }

    parser.code.newline = (unsigned char)((flags & REG_NEWLINE) != 0);
    if (compile_node(&parser, tree) != 0 || parser.error != 0) {
        rc = (parser.error != 0) ? parser.error : REG_ESPACE;
        tree_free(parser.pool);
        free(parser.code.code);
        return rc;
    }
    if (emit(&parser, I_MATCH, 0, 0, 0, NULL) < 0) {
        tree_free(parser.pool);
        free(parser.code.code);
        return REG_ESPACE;
    }

    expression->re_nsub = (size_t)parser.groups;
    expression->re_private = malloc(sizeof(parser));
    if (expression->re_private == NULL) {
        tree_free(parser.pool);
        free(parser.code.code);
        return REG_ESPACE;
    }
    memcpy(expression->re_private, &parser, sizeof(parser));

    /* The tree is owned by the copy now; do not free it here. */
    return 0;
}

int regexec(const regex_t *expression, const char *text, size_t count, regmatch_t matches[],
            int flags)
{
    struct parser *parser;
    struct matcher matcher;
    size_t length;
    size_t width;
    size_t start;
    int found = 0;

    if (expression == NULL || text == NULL || expression->re_private == NULL) {
        return REG_NOMATCH;
    }

    parser = (struct parser *)expression->re_private;
    length = strlen(text);
    width = length + 1;

    memset(&matcher, 0, sizeof(matcher));
    matcher.program = &parser->code;
    matcher.text = text;
    matcher.length = length;
    matcher.flags = flags;

    matcher.saves = (int *)malloc(sizeof(int) * 2 * REGEX_MAX_GROUPS);
    if (matcher.saves == NULL) {
        return REG_ESPACE;
    }

    /* The guard against a loop that matches nothing is a bit per pair of
     * (instruction, position); a text long enough to make it large is walked
     * without it, and the recursion of an empty loop is then the history of
     * the expression itself. */
    if ((size_t)parser->code.count * width <= (size_t)1 << 22) {
        size_t bits = (size_t)parser->code.count * width;
        size_t bytes = (bits + 7) / 8;

        matcher.visited = (unsigned char *)malloc(bytes);
        if (matcher.visited != NULL) {
            memset(matcher.visited, 0, bytes);
        }
    }
    matcher.width = width;

    for (start = 0; start <= length; start++) {
        for (int i = 0; i < 2 * REGEX_MAX_GROUPS; i++) {
            matcher.saves[i] = -1;
        }
        if (matcher.visited != NULL) {
            size_t bits = (size_t)parser->code.count * width;

            memset(matcher.visited, 0, (bits + 7) / 8);
        }
        if (run(&matcher, 0, start)) {
            found = 1;
            if (matches != NULL && count > 0) {
                matches[0].rm_so = (regoff_t)start;
                matches[0].rm_eo = (regoff_t)matcher.end;
                for (size_t group = 1; group < count; group++) {
                    int begin = (group < REGEX_MAX_GROUPS) ? matcher.saves[2 * group] : -1;
                    int finish = (group < REGEX_MAX_GROUPS) ? matcher.saves[2 * group + 1] : -1;

                    matches[group].rm_so = (regoff_t)begin;
                    matches[group].rm_eo = (regoff_t)finish;
                }
            }
            break;
        }
    }

    /*
     * The whole match is saved by a pair around the program: the group 0 of
     * POSIX. The matcher has no instruction for it, so the end of the match
     * is the position of the I_MATCH that ended the walk, and the walk above
     * gives the start. A shorter answer than the text is the ordinary case of
     * a pattern that does not end in `.*`.
     */
    free(matcher.visited);
    free(matcher.saves);

    return found ? 0 : REG_NOMATCH;
}

void regfree(regex_t *expression)
{
    struct parser *parser;

    if (expression == NULL || expression->re_private == NULL) {
        return;
    }

    parser = (struct parser *)expression->re_private;
    tree_free(parser->pool);
    free(parser->code.code);
    free(parser);
    expression->re_private = NULL;
    expression->re_nsub = 0;
}

size_t regerror(int code, const regex_t *expression, char *buffer, size_t size)
{
    const char *text;
    size_t length;

    (void)expression;

    if (code < 0 || (size_t)code >= sizeof(reg_errors) / sizeof(reg_errors[0])) {
        text = "unknown error";
    } else {
        text = reg_errors[code];
    }

    length = strlen(text) + 1;
    if (buffer != NULL && size > 0) {
        size_t copy = (length < size) ? length : size;

        memcpy(buffer, text, copy);
        buffer[copy - 1] = '\0';
    }

    return length;
}
