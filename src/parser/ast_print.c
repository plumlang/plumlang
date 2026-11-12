/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Printing the syntax tree as S-expressions, for `plum ast` and tests. */
#include "parser/ast.h"

#include "util/fmt.h"

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

static void print_list(FILE *out, PlumExpr *const *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fputc(' ', out);
        plum_print_expr(out, items[i]);
    }
}

void plum_print_expr(FILE *out, const PlumExpr *e) {
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
                plum_print_expr(out, part->expr);
                fprintf(out, " \"%s\")", part->format);
            } else {
                plum_print_expr(out, part->expr);
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
        print_list(out, e->as.list.items, e->as.list.count);
        fputc(')', out);
        break;
    case PLUM_EXPR_MAP:
        fputs("(map", out);
        for (size_t i = 0; i < e->as.map.count; i++) {
            fputs(" (", out);
            plum_print_expr(out, e->as.map.entries[i].key);
            fputc(' ', out);
            plum_print_expr(out, e->as.map.entries[i].value);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_UNARY:
        fprintf(out, "(%s ", plum_token_kind_text(e->as.unary.op));
        plum_print_expr(out, e->as.unary.operand);
        fputc(')', out);
        break;
    case PLUM_EXPR_BINARY:
        fprintf(out, "(%s ", plum_token_kind_text(e->as.binary.op));
        plum_print_expr(out, e->as.binary.left);
        fputc(' ', out);
        plum_print_expr(out, e->as.binary.right);
        fputc(')', out);
        break;
    case PLUM_EXPR_RANGE:
        fprintf(out, "(%s ", e->as.range.inclusive ? "..=" : "..");
        plum_print_expr(out, e->as.range.low);
        fputc(' ', out);
        plum_print_expr(out, e->as.range.high);
        fputc(')', out);
        break;
    case PLUM_EXPR_IS:
    case PLUM_EXPR_AS:
        fprintf(out, "(%s ", e->kind == PLUM_EXPR_IS ? "is" : "as");
        plum_print_expr(out, e->as.cast.value);
        fputc(' ', out);
        plum_print_type(out, e->as.cast.type);
        fputc(')', out);
        break;
    case PLUM_EXPR_CALL:
        fputs("(call ", out);
        plum_print_expr(out, e->as.call.callee);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            const PlumArg *arg = &e->as.call.args[i];
            fputc(' ', out);
            if (arg->name != NULL) {
                fprintf(out, "(%s: ", arg->name);
                plum_print_expr(out, arg->value);
                fputc(')', out);
            } else {
                plum_print_expr(out, arg->value);
            }
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_INDEX:
        fputs("(index ", out);
        plum_print_expr(out, e->as.index.object);
        fputc(' ', out);
        plum_print_expr(out, e->as.index.index);
        fputc(')', out);
        break;
    case PLUM_EXPR_FIELD:
        fprintf(out, "(%s ", e->as.field.optional ? "?." : ".");
        plum_print_expr(out, e->as.field.object);
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
        plum_print_expr(out, e->as.lambda.body);
        fputc(')', out);
        break;
    case PLUM_EXPR_IF:
        fputs("(if ", out);
        plum_print_expr(out, e->as.if_.cond);
        fputc(' ', out);
        plum_print_expr(out, e->as.if_.then);
        if (e->as.if_.otherwise != NULL) {
            fputc(' ', out);
            plum_print_expr(out, e->as.if_.otherwise);
        }
        fputc(')', out);
        break;
    case PLUM_EXPR_TRY:
    case PLUM_EXPR_SPAWN:
        fprintf(out, "(%s ", e->kind == PLUM_EXPR_TRY ? "try" : "spawn");
        plum_print_expr(out, e->as.inner);
        fputc(')', out);
        break;
    case PLUM_EXPR_ERROR:
        fputs("<error>", out);
        break;
    }
}
