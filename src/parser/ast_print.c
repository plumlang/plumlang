/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Printing the syntax tree as S-expressions, for `plum ast` and tests. */
#include "parser/ast.h"

#include "util/fmt.h"

static void print_expr(FILE *out, const PlumExpr *e, int indent);
static void print_stmt(FILE *out, const PlumStmt *s, int indent);

void plum_print_type(FILE *out, const PlumType *type) {
    switch (type->kind) {
    case PLUM_TYPE_NAME:
        fputs(type->as.name.name, out);
        if (type->as.name.arg_count > 0) {
            fputc('[', out);
            for (size_t i = 0; i < type->as.name.arg_count; i++) {
                if (i > 0) {
                    fputs(", ", out);
                }
                plum_print_type(out, type->as.name.args[i]);
            }
            fputc(']', out);
        }
        break;
    case PLUM_TYPE_OPTIONAL:
        plum_print_type(out, type->as.optional);
        fputc('?', out);
        break;
    case PLUM_TYPE_FUNC:
        fputc('(', out);
        for (size_t i = 0; i < type->as.func.param_count; i++) {
            if (i > 0) {
                fputs(", ", out);
            }
            plum_print_type(out, type->as.func.params[i]);
        }
        fputs(") -> ", out);
        plum_print_type(out, type->as.func.result);
        break;
    case PLUM_TYPE_ERROR:
        fputs("<error>", out);
        break;
    }
}

static void print_list(FILE *out, PlumExpr *const *items, size_t count, int indent) {
    for (size_t i = 0; i < count; i++) {
        fputc(' ', out);
        print_expr(out, items[i], indent);
    }
}

static void newline(FILE *out, int indent) {
    fputc('\n', out);
    for (int i = 0; i < indent; i++) {
        fputc(' ', out);
    }
}

void plum_print_expr(FILE *out, const PlumExpr *e) {
    print_expr(out, e, 0);
}

static void print_expr(FILE *out, const PlumExpr *e, int indent) {
    switch (e->kind) {
    case PLUM_EXPR_INT:
        fprintf(out, "%lld", (long long)e->as.i);
        break;
    case PLUM_EXPR_FLOAT: {
        char buf[PLUM_FLOAT_BUF];
        fputs(plum_format_float(e->as.f, buf), out);
        break;
    }
    case PLUM_EXPR_STRING:
        plum_write_quoted(out, e->as.str.chars, e->as.str.len);
        break;
    case PLUM_EXPR_INTERP:
        fputs("(interp", out);
        for (size_t i = 0; i < e->as.interp.count; i++) {
            const PlumInterpPart *part = &e->as.interp.parts[i];
            fputc(' ', out);
            if (part->expr == NULL) {
                plum_write_quoted(out, part->text, part->len);
            } else if (part->format != NULL) {
                fputs("(format ", out);
                print_expr(out, part->expr, indent);
                fprintf(out, " \"%s\")", part->format);
            } else {
                print_expr(out, part->expr, indent);
            }
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_BOOL:
        fputs(e->as.b ? "good" : "bad", out);
        break;
    case PLUM_EXPR_NONE:
        fputs("none", out);
        break;
    case PLUM_EXPR_NAME:
        fputs(e->as.name, out);
        break;
    case PLUM_EXPR_SELF:
        fputs("self", out);
        break;
    case PLUM_EXPR_SUPER:
        fputs("super", out);
        break;
    case PLUM_EXPR_LIST:
        fputs("(list", out);
        print_list(out, e->as.list.items, e->as.list.count, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_MAP:
        fputs("(map", out);
        for (size_t i = 0; i < e->as.map.count; i++) {
            fputs(" (", out);
            print_expr(out, e->as.map.entries[i].key, indent);
            fputc(' ', out);
            print_expr(out, e->as.map.entries[i].value, indent);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_UNARY:
        fprintf(out, "(%s ", plum_token_kind_text(e->as.unary.op));
        print_expr(out, e->as.unary.operand, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_BINARY:
        fprintf(out, "(%s ", plum_token_kind_text(e->as.binary.op));
        print_expr(out, e->as.binary.left, indent);
        fputc(' ', out);
        print_expr(out, e->as.binary.right, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_RANGE:
        fprintf(out, "(%s ", e->as.range.inclusive ? "..=" : "..");
        print_expr(out, e->as.range.low, indent);
        fputc(' ', out);
        print_expr(out, e->as.range.high, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        fprintf(out, "(%s ", e->kind == PLUM_EXPR_IS ? "is" : "as");
        print_expr(out, e->as.cast.value, indent);
        fputc(' ', out);
        plum_print_type(out, e->as.cast.type);
        fputc(')', out);
        break;
    case PLUM_EXPR_CALL:
        fputs("(call ", out);
        print_expr(out, e->as.call.callee, indent);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            const PlumArg *arg = &e->as.call.args[i];
            fputc(' ', out);
            if (arg->name != NULL) {
                fprintf(out, "(%s: ", arg->name);
                print_expr(out, arg->value, indent);
                fputc(')', out);
            } else {
                print_expr(out, arg->value, indent);
            }
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_INDEX:
        fputs("(index ", out);
        print_expr(out, e->as.index.object, indent);
        fputc(' ', out);
        print_expr(out, e->as.index.index, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_FIELD:
        fprintf(out, "(%s ", e->as.field.optional ? "?." : ".");
        print_expr(out, e->as.field.object, indent);
        fprintf(out, " %s)", e->as.field.name);
        break;
    case PLUM_EXPR_LAMBDA:
        fputs("(lambda (", out);
        for (size_t i = 0; i < e->as.lambda.param_count; i++) {
            const PlumParam *param = &e->as.lambda.params[i];
            if (i > 0) {
                fputc(' ', out);
            }
            if (param->type != NULL) {
                fprintf(out, "(%s ", param->name);
                plum_print_type(out, param->type);
                fputc(')', out);
            } else {
                fputs(param->name, out);
            }
        }
        fputs(") ", out);
        if (e->as.lambda.block != NULL) {
            print_stmt(out, e->as.lambda.block, indent);
        } else {
            print_expr(out, e->as.lambda.body, indent);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_MATCH:
        fputs("(match ", out);
        print_expr(out, e->as.match.subject, indent);
        for (size_t i = 0; i < e->as.match.arm_count; i++) {
            const PlumMatchArm *arm = &e->as.match.arms[i];
            newline(out, indent + 2);
            fputs("(arm", out);
            if (arm->pattern_count == 0) {
                fputs(" else", out);
            }
            for (size_t k = 0; k < arm->pattern_count; k++) {
                fputc(' ', out);
                print_expr(out, arm->patterns[k], indent);
            }
            fputs(" => ", out);
            print_stmt(out, arm->body, indent + 4);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_WILDCARD:
        fputc('_', out);
        break;
    case PLUM_EXPR_IF:
        fputs("(if ", out);
        print_expr(out, e->as.if_.cond, indent);
        fputc(' ', out);
        print_expr(out, e->as.if_.then, indent);
        if (e->as.if_.otherwise != NULL) {
            fputc(' ', out);
            print_expr(out, e->as.if_.otherwise, indent);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        fprintf(out, "(%s ", e->kind == PLUM_EXPR_TRY ? "try" : "spawn");
        print_expr(out, e->as.inner, indent);
        fputc(')', out);
        break;
    case PLUM_EXPR_ERROR:
        fputs("<error>", out);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static void print_params(FILE *out, const PlumParam *params, size_t count, int indent) {
    fputc('(', out);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            fputc(' ', out);
        }
        fprintf(out, "(%s", params[i].name);
        if (params[i].type != NULL) {
            fputc(' ', out);
            plum_print_type(out, params[i].type);
        }
        if (params[i].fallback != NULL) {
            fputs(" = ", out);
            print_expr(out, params[i].fallback, indent);
        }
        fputc(')', out);
    }
    fputc(')', out);
}

static void print_branch(FILE *out, const PlumBranch *b, int indent) {
    fprintf(out, "(branch %s ", b->name);
    print_params(out, b->params, b->param_count, indent);
    if (b->result != NULL) {
        fputs(" -> ", out);
        plum_print_type(out, b->result);
    }
    if (b->arrow != NULL) {
        fputs(" => ", out);
        print_expr(out, b->arrow, indent);
    } else if (b->body != NULL) {
        newline(out, indent + 2);
        print_stmt(out, b->body, indent + 2);
    }
    fputc(')', out);
}

static void print_stmt(FILE *out, const PlumStmt *s, int indent) {
    switch (s->kind) {
    case PLUM_STMT_EXPR:
        print_expr(out, s->as.expr, indent);
        break;
    case PLUM_STMT_DECL:
        fprintf(out, "(%s %s", s->as.decl.constant ? "root" : "plant", s->as.decl.name);
        if (s->as.decl.type != NULL) {
            fputs(": ", out);
            plum_print_type(out, s->as.decl.type);
        }
        if (s->as.decl.value != NULL) {
            fputc(' ', out);
            print_expr(out, s->as.decl.value, indent);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_ASSIGN:
        fprintf(out, "(%s ", plum_token_kind_text(s->as.assign.op));
        print_expr(out, s->as.assign.target, indent);
        fputc(' ', out);
        print_expr(out, s->as.assign.value, indent);
        fputc(')', out);
        break;
    case PLUM_STMT_BLOCK:
        fputs("(block", out);
        for (size_t i = 0; i < s->as.block.count; i++) {
            newline(out, indent + 2);
            print_stmt(out, s->as.block.stmts[i], indent + 2);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_IF:
    case PLUM_STMT_IF_PLANT:
        if (s->kind == PLUM_STMT_IF) {
            fputs("(if ", out);
            print_expr(out, s->as.if_.cond, indent);
        } else {
            fprintf(out, "(if-plant %s ", s->as.if_.name);
            print_expr(out, s->as.if_.value, indent);
        }
        newline(out, indent + 2);
        print_stmt(out, s->as.if_.then, indent + 2);
        if (s->as.if_.otherwise != NULL) {
            newline(out, indent + 2);
            print_stmt(out, s->as.if_.otherwise, indent + 2);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_WHILE:
        fputs("(while ", out);
        print_expr(out, s->as.while_.cond, indent);
        newline(out, indent + 2);
        print_stmt(out, s->as.while_.body, indent + 2);
        fputc(')', out);
        break;
    case PLUM_STMT_FOR:
        fprintf(out, "(for %s", s->as.for_.first);
        if (s->as.for_.second != NULL) {
            fprintf(out, ", %s", s->as.for_.second);
        }
        fputc(' ', out);
        print_expr(out, s->as.for_.iterable, indent);
        newline(out, indent + 2);
        print_stmt(out, s->as.for_.body, indent + 2);
        fputc(')', out);
        break;
    case PLUM_STMT_BREAK:
        fputs("(break)", out);
        break;
    case PLUM_STMT_CONTINUE:
        fputs("(continue)", out);
        break;
    case PLUM_STMT_RETURN:
    case PLUM_STMT_THROW:
        fputs(s->kind == PLUM_STMT_RETURN ? "(return" : "(throw", out);
        if (s->as.expr != NULL) {
            fputc(' ', out);
            print_expr(out, s->as.expr, indent);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_TRY:
        fputs("(try", out);
        newline(out, indent + 2);
        print_stmt(out, s->as.try_.body, indent + 2);
        for (size_t i = 0; i < s->as.try_.catch_count; i++) {
            const PlumCatch *c = &s->as.try_.catches[i];
            newline(out, indent + 2);
            fprintf(out, "(catch %s", c->name);
            if (c->type != NULL) {
                fputs(": ", out);
                plum_print_type(out, c->type);
            }
            newline(out, indent + 4);
            print_stmt(out, c->body, indent + 4);
            fputc(')', out);
        }
        if (s->as.try_.finally != NULL) {
            newline(out, indent + 2);
            fputs("(finally", out);
            newline(out, indent + 4);
            print_stmt(out, s->as.try_.finally, indent + 4);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_BRANCH:
        print_branch(out, &s->as.branch, indent);
        break;
    case PLUM_STMT_TREE:
    case PLUM_STMT_TRAIT:
        fprintf(out, "(%s %s", s->kind == PLUM_STMT_TREE ? "tree" : "trait", s->as.tree.name);
        if (s->as.tree.super_count > 0) {
            fputs(" :", out);
            for (size_t i = 0; i < s->as.tree.super_count; i++) {
                fputc(' ', out);
                plum_print_type(out, s->as.tree.supers[i]);
            }
        }
        for (size_t i = 0; i < s->as.tree.field_count; i++) {
            const PlumField *f = &s->as.tree.fields[i];
            newline(out, indent + 2);
            fprintf(out, "(field %s%s ", f->name, f->weak ? " weak" : "");
            plum_print_type(out, f->type);
            if (f->fallback != NULL) {
                fputs(" = ", out);
                print_expr(out, f->fallback, indent);
            }
            fputc(')', out);
        }
        for (size_t i = 0; i < s->as.tree.method_count; i++) {
            newline(out, indent + 2);
            print_branch(out, &s->as.tree.methods[i], indent + 2);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_KIND:
        fprintf(out, "(kind %s", s->as.kind.name);
        for (size_t i = 0; i < s->as.kind.case_count; i++) {
            const PlumCase *c = &s->as.kind.cases[i];
            fprintf(out, " (%s", c->name);
            if (c->param_count > 0) {
                fputc(' ', out);
                print_params(out, c->params, c->param_count, indent);
            }
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_SEED:
        fprintf(out, "(seed %s", s->as.seed.path);
        if (s->as.seed.alias != NULL) {
            fprintf(out, " as %s", s->as.seed.alias);
        }
        if (s->as.seed.name_count > 0) {
            fputs(" {", out);
            for (size_t i = 0; i < s->as.seed.name_count; i++) {
                fprintf(out, "%s%s", i > 0 ? " " : "", s->as.seed.names[i]);
            }
            fputc('}', out);
        }
        fputc(')', out);
        break;
    case PLUM_STMT_ERROR:
        fputs("<error>", out);
        break;
    }
}

void plum_print_program(FILE *out, const PlumProgram *program) {
    for (size_t i = 0; i < program->count; i++) {
        print_stmt(out, program->stmts[i], 0);
        fputc('\n', out);
    }
}
