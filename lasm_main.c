
#include "list.h"
#include "parse_tree.h"
#include "lasm.tab.h"
#include "lasm.yy.h"
#include "lasm_param.h"
#include "warn.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <search.h>

#define REG_READ  false
#define REG_WRITE true

typedef struct reg_access_t {
	struct list_head all; /* all */
	struct list_head act; /* all readers OR all writers */
	struct reg_access_t *prev_other;
	bool writer;
	stmt_t *stmt;
	arg_t  *arg;
} reg_access_t;

typedef struct reg_t {
	char *regname;
	struct list_head ra_all; /* reg_access_t */
	struct list_head ra_act[2]; /* index by writter, reg_access_t */
} reg_t;

typedef struct reg_set {
	void *root;
	reg_t mem;
} reg_set_t;

static inline void reg_init(reg_t *r, char *name)
{
	r->regname = name;
	list_init(&r->ra_all);
	list_init(r->ra_act + 0);
	list_init(r->ra_act + 1);
}

static inline void reg_access_init(reg_access_t *sp, reg_t *r, stmt_t *stmt, arg_t *arg, bool writer)
{
	list_init(&sp->all);
	list_init(&sp->act);
	sp->writer     = writer;
	sp->stmt       = stmt;
	sp->arg        = arg;

	struct list_head *po = &r->ra_act[!writer];
	if (!list_is_empty(po)) {
		sp->prev_other = list_entry(po->prev, reg_access_t, act);
	} else {
		sp->prev_other = NULL;
	}
}

static inline void reg_set_init(reg_set_t *rs)
{
	rs->root = NULL;
	reg_init(&rs->mem, "memory");
}

int reg_cmpar_by_name(reg_t *to_insert, reg_t *exsisting)
{
	return strcmp(to_insert->regname, exsisting->regname);
}

reg_access_t *reg_add_access(reg_t *r, stmt_t *stmt, arg_t *arg, bool writer)
{
	reg_access_t *sp = malloc(sizeof(*sp));
	if (!sp)
		return NULL;

	reg_access_init(sp, r, stmt, arg, writer);

	list_add_prev(&r->ra_all, &sp->all);
	list_add_prev(&r->ra_act[writer], &sp->act);

	return sp;
}

reg_t *reg_find(struct reg_set *rs, char *regname)
{
	reg_t *r = malloc(sizeof(*r));
	if (!r)
		return NULL;

	reg_init(r, regname);

	reg_t **found_reg = tsearch(r, &rs->root, (__compar_fn_t)reg_cmpar_by_name);

	if (*found_reg != r)
		free(r);
	return *found_reg;
}

reg_access_t *reg_prev_access_with_type(reg_t *r, reg_access_t *cur, bool written)
{
	return cur->prev_other;
}

bool stmt_has_fwd_dep(stmt_t *e)
{
	arg_t *a;
	if (e->mem_dep.dep)
		return true;


	arg_list_for_each(a, &e->arg_in_list) {
		if (a->dep.dep)
			return true;
	}

	arg_list_for_each(a, &e->arg_out_list) {
		if (a->dep.dep)
			return true;
	}

	return false;
}

bool stmt_has_rev_dep(stmt_t *e)
{
	return !list_is_empty(&e->rev_dep_list);
}

int stmt_add_rev_dep(stmt_t *dep_on, stmt_t *rev)
{
	rev_dep_t *rd = malloc(sizeof(*rd));
	if (!rd)
		return -1;

	rd->stmt = rev;

	list_add_prev(&dep_on->rev_dep_list, &rd->l);

	return 0;
}

int stmt_add_dep(stmt_t *stmt, dep_t *dep, bool written, reg_access_t *curr_ra, reg_t *reg)
{
	/* the last access of the type we are not */
	reg_access_t *prev_ra = reg_prev_access_with_type(reg, curr_ra, !written);
	if (!prev_ra) {
		/* we are the first access, no dependency */
		return 0;
	}

	if (!written && prev_ra->writer) {
		/* RAW */
		stmt_add_rev_dep(prev_ra->stmt, stmt);
		dep->dep      = prev_ra->stmt;
		dep->dep_type = DEP_RAW;
	} else if (written && !prev_ra->writer) {
		/* WAR */
		stmt_add_rev_dep(prev_ra->stmt, stmt);
		dep->dep      = prev_ra->stmt;
		dep->dep_type = DEP_WAR;
	} else {
		/* some dep I don't care about */
		DEBUG_PR("go a unhandled dep type: written: %d ; prev written: %d", written, prev_ra->writer);
	}

	return 0;
}

int _reg_accessed(struct reg_set *t, stmt_t *stmt, dep_t *dep, arg_t *arg, bool written, reg_t *found_reg)
{
	reg_access_t *ra = reg_add_access(found_reg, stmt, arg, written);
	if (!ra)
		return -1;

	return stmt_add_dep(stmt, dep, written, ra, found_reg);
}

int reg_accessed(struct reg_set *t, stmt_t *stmt, arg_t *arg, bool written)
{
	reg_t *found_reg = reg_find(t, arg->arg);
	if (!found_reg)
		return -1;

	return _reg_accessed(t, stmt, &arg->dep, arg, written, found_reg);
}

int mem_accessed(reg_set_t *rs, stmt_t *e, bool written)
{
	reg_t *r = &rs->mem;
	return _reg_accessed(rs, e, &e->mem_dep, NULL, written, r);
}

int stmt_populate_deps(stmt_t *e, struct reg_set *rs)
{
	arg_t *a;
	arg_list_for_each(a, &e->arg_in_list) {
		if (a->type == ARG_REG)
			reg_accessed(rs, e, a, REG_READ);
	}

	if (e->instr->mem_access == MEM_IN_RD) {
		mem_accessed(rs, e, REG_READ);
	}

	bool written = e->instr->mem_access != MEM_OUT_WR;
	arg_list_for_each(a, &e->arg_out_list) {
		if (a->type == ARG_REG)
			reg_accessed(rs, e, a, written);
	}

	if (e->instr->mem_access == MEM_OUT_WR) {
		mem_accessed(rs, e, REG_WRITE);
	}

	return 0;
}

int stmt_list_populate_deps(struct list_head *stmt_list, struct reg_set *rs)
{
	stmt_t *e;
	stmt_list_for_each(e, stmt_list) {
		//DEBUG_PR("processing stmt: %s", e->opcode);
		int r = stmt_populate_deps(e, rs);
		if (r < 0) {
			WARN_STMT(e, "could not generate deps");
			return -1;
		}
	}

	return 0;
}

void arg_list_emit(struct list_head *arg_list, FILE *o)
{
	arg_t *a;
	arg_list_for_each(a, arg_list) {
		if (a->l.next != arg_list)
			fprintf(o, "%s, ", a->arg);
		else
			fputs(a->arg, o);
	}
}

void stmt_emit_iloc(stmt_t *e, FILE *o)
{
	fputs(e->instr->name, o);
	fputc(' ', o);
	arg_list_emit(&e->arg_in_list, o);
	if (&e->arg_out_list != e->arg_out_list.next)
		fputs(" => ", o);
	arg_list_emit(&e->arg_out_list, o);
}

void dep_print(dep_t *dep, int stmt_parent, FILE *o)
{
	if (dep->dep) {
		char *style;
		if (dep->dep_type == DEP_WAR) {
			style = "dashed";
		} else {
			style = "solid";
		}
		fprintf(o, "stmt_%d -> stmt_%d [style=\"%s\"]\n", stmt_parent, dep->dep->inum, style);
	}
}

void arg_list_deps_print(struct list_head *arg_list, int stmt_parent, FILE *o)
{
	arg_t *a;
	int i = 0;
	arg_list_for_each(a, arg_list) {
		//fprintf(o, "arg_%d_%d [label=\"%s\"]\n", stmt_parent, i, a->arg);
		//fprintf(o, "arg_%d_%d -> stmt_%d [dir=\"none\"]\n", stmt_parent, i, stmt_parent);
		dep_print(&a->dep, stmt_parent, o);
		i++;
	}
}

void stmt_deps_print(stmt_t *e, FILE *o)
{
	fprintf(o, "stmt_%d [label=\"(%u) ", e->inum, e->cum_latency);
	stmt_emit_iloc(e, o);
	fprintf(o, "\"]\n");

	arg_list_deps_print(&e->arg_in_list,  e->inum, o);
	arg_list_deps_print(&e->arg_out_list, e->inum, o);

	dep_print(&e->mem_dep, e->inum, o);
}

void stmt_list_deps_print(struct list_head *stmt_list, FILE *o)
{
	stmt_t *e;
	fprintf(o, "digraph g {\n");
	stmt_list_for_each(e, stmt_list) {
		stmt_deps_print(e, o);
	}
	fprintf(o, "}\n");
}

void stmt_calc_cum_latency(stmt_t *e, unsigned prev_latency)
{
	if (!e)
		return;

	unsigned ilate = e->instr->latency;
	unsigned curlate = ilate + prev_latency;
	if (e->cum_latency < curlate) {
		e->cum_latency = curlate;

		arg_t *a;
		arg_list_for_each(a, &e->arg_in_list) {
			stmt_calc_cum_latency(a->dep.dep, curlate);
		}

		arg_list_for_each(a, &e->arg_out_list) {
			stmt_calc_cum_latency(a->dep.dep, curlate);
		}

		stmt_calc_cum_latency(e->mem_dep.dep, curlate);
	}
}

void stmt_list_calc_cum_latency(struct list_head *stmt_list)
{
	stmt_t *e;
	stmt_list_for_each(e, stmt_list) {
		if (stmt_has_rev_dep(e))
			continue;

		stmt_calc_cum_latency(e, 0);
	}
}

void stmt_list_schedule(struct list_head *stmt_list)
{
	stmt_t *e;
	LIST_HEAD(ready);
	stmt_list_for_each(e, stmt_list) {
		if (stmt_has_fwd_dep(e))
			continue;

		/* operate only on the leaves */
		list_add_prev(&ready, &e->ready_list);
	}
}


int main(int argc, char *argv[])
{
	LIST_HEAD(lh);
	yyscan_t s = NULL;
	int r = lasm_lex_init(&s);
	if (r != 0)
		fprintf(stderr, "bleh.");

	lasm_parse(&lh, s);
	lasm_lex_destroy(s);

	r = stmt_list_match_instrs(&lh);
	if (r < 0)
		return -1;

	reg_set_t rs;
	reg_set_init(&rs);

	r = stmt_list_populate_deps(&lh, &rs);
	if (r < 0) {
		WARN("error in populating dependencies.");
		return -1;
	}

	stmt_list_calc_cum_latency(&lh);

	stmt_list_deps_print(&lh, stdout);

	stmt_list_free(&lh);

	return 0;
}
