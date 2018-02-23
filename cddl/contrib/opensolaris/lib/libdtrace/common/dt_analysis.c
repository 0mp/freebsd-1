/*-
 * Copyright (c) 2018 Jonathan Anderson
 * Copyright (c) 2018 Brian Kidney
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include <dt_analysis.h>
#include <dt_program.h>


/* TODO: refactor */
#define	CALL_OPCODE	0x2F


/* Print GraphViz Dot-formatted output for a DTrace action */
static void print_action(dtrace_actdesc_t *, const char *probename, FILE *out);

/* Print GraphViz Dot-formatted output for a DTrace Instruction Format Object */
static void print_difo(dtrace_difo_t *, const char *probename, FILE *out);


/*ARGSUSED*/
bool
dtrace_analyze_program_modref(dtrace_prog_t *pgp, FILE *output)
{
	dt_stmt_t *stp;
	dtrace_actdesc_t *ap;
	dtrace_ecbdesc_t *last = NULL;
	dtrace_probedesc_t *descp;
	int modref, cumulative_modref = 0;
	bool ok = true;

	for (stp = dt_list_next(&pgp->dp_stmts); stp; stp = dt_list_next(stp)) {
		dtrace_ecbdesc_t *edp = stp->ds_desc->dtsd_ecbdesc;
		if (edp	== last)
			continue;
		last = edp;
		descp = &edp->dted_probe;

		for (ap = edp->dted_action; ap != NULL; ap = ap->dtad_next) {
			int modref = dtrace_modref_action(ap);

			ok &= dtrace_modref_check(modref, cumulative_modref,
				descp, stderr);

			cumulative_modref |= modref;
		}
	}

	return (ok);
}

/*ARGSUSED*/
void
dtrace_graph_program(dtrace_hdl_t *dtp, dtrace_prog_t *pgp, FILE *dot_output)
{
	char probename[DTRACE_FULLNAMELEN];
	dt_stmt_t *stp;
	dtrace_actdesc_t *ap;
	dtrace_ecbdesc_t *edp, *last = NULL;
	dtrace_probedesc_t *descp;
	char *cp;

	fprintf(dot_output, "digraph {\n");

	for (stp = dt_list_next(&pgp->dp_stmts); stp; stp = dt_list_next(stp)) {
		edp = stp->ds_desc->dtsd_ecbdesc;
		if (edp	== last)
			continue;
		last = edp;
		descp = &edp->dted_probe;

		assert(sizeof(probename) == (sizeof(descp->dtpd_provider) +
			sizeof(descp->dtpd_mod) + sizeof(descp->dtpd_func) +
			sizeof(descp->dtpd_name) + 4));

		cp = probename;
		cp = stpncpy(cp, descp->dtpd_provider,
			sizeof(probename) - (cp - probename) - 1);
		*cp++ = ':';
		cp = stpncpy(cp, descp->dtpd_mod,
			sizeof(probename) - (cp - probename) - 1);
		*cp++ = ':';
		cp = stpncpy(cp, descp->dtpd_func,
			sizeof(probename) - (cp - probename) - 1);
		*cp++ = ':';
		cp = stpncpy(cp, descp->dtpd_name,
			sizeof(probename) - (cp - probename) - 1);
		assert(*cp == 0);

		for (ap = edp->dted_action; ap != NULL; ap = ap->dtad_next) {
			print_action(ap, probename, dot_output);
		}
	}

	fprintf(dot_output, "}\n");
}

int
dtrace_modref_action(const dtrace_actdesc_t *ap)
{
	dtrace_actkind_t kind = ap->dtad_kind;
	int modref = 0;

	switch (kind) {
	case DTRACEACT_NONE:
	case DTRACEACT_STOP:
	case DTRACEACT_RAISE:
	case DTRACEACT_PRINTF:
	case DTRACEACT_PRINTA:
	case DTRACEACT_PRINTM:
		break;

	case DTRACEACT_EXIT:
	case DTRACEACT_TRACEMEM:
	case DTRACEACT_TRACEMEM_DYNSIZE:
		modref |= DTRACE_MODREF_MEMORY_MOD;
		break;

	case DTRACEACT_DIFEXPR:
		modref |= dtrace_modref_difo(ap->dtad_difo);
		break;

	case DTRACEACT_USTACK:
	case DTRACEACT_JSTACK:
		modref |= DTRACE_MODREF_MEMORY_REF | DTRACE_MODREF_MEMORY_MOD;
		break;

	case DTRACEACT_SPECULATIVE:
	case DTRACEACT_SPECULATE:
	case DTRACEACT_DISCARD:
		modref |= DTRACE_MODREF_STATE_REF | DTRACE_MODREF_STATE_MOD;
		break;

	case DTRACEACT_COMMIT:
		/* TODO */
		modref |= DTRACE_MODREF_STATE_REF | DTRACE_MODREF_STATE_MOD
			| DTRACE_MODREF_MEMORY_MOD;
		break;

	case DTRACEACT_PROC:
	case DTRACEACT_USYM:
	case DTRACEACT_UMOD:
	case DTRACEACT_UADDR:
	case DTRACEACT_PROC_DESTRUCTIVE:
	case DTRACEACT_SYSTEM:
	case DTRACEACT_FREOPEN:
	case DTRACEACT_PROC_CONTROL:
	case DTRACEACT_KERNEL:
	case DTRACEACT_STACK	:
	case DTRACEACT_SYM:
	case DTRACEACT_MOD:
	case DTRACEACT_KERNEL_DESTRUCTIVE:
	case DTRACEACT_BREAKPOINT:
	case DTRACEACT_PANIC:
	case DTRACEACT_CHILL:
	default:
		/* TODO: classify more action kinds */
		modref |= DTRACE_MODREF_ALL;
		break;
	}

	return (modref);
}

int
dtrace_modref_call(const dif_instr_t *ip)
{

	assert(DIF_INSTR_OP(*ip) == CALL_OPCODE);

	switch (DIF_INSTR_SUBR(*ip)) {
	default:
		// If we haven't explicitly described the behaviour of the
		// called subroutine, assume the worst:
		return (DTRACE_MODREF_ALL);
	}
}

bool
dtrace_modref_check(int action_modref, int cumulative_modref,
	     const dtrace_probedesc_t *dp, FILE *output)
{

	if ((action_modref & cumulative_modref) == action_modref) {
		// No new modifications or references have been made
		return (0);
	}

	// TODO: check various scenario policies
	fprintf(output, "new mod/ref behaviour in %s:%s:%s:%s: 0x%x vs 0x%x\n",
		dp->dtpd_provider, dp->dtpd_mod, dp->dtpd_func, dp->dtpd_name,
		action_modref, cumulative_modref);

	return (DTRACE_MODREF_ALL);
}

int
dtrace_modref_difo(const dtrace_difo_t *dp)
{
	dtrace_difv_t *vp;
	dif_instr_t *ip;
	int i;
	int modref = 0;

	/* Check explicit mod/ref behaviour described in symbol table */
	for (i = 0; i < dp->dtdo_varlen; i++) {
		vp = &dp->dtdo_vartab[i];

		if (vp->dtdv_flags & DIFV_F_MOD) {
			switch (vp->dtdv_scope) {
			case DIFV_SCOPE_GLOBAL:
				modref |= DTRACE_MODREF_GLOBAL_MOD;
				break;
			case DIFV_SCOPE_THREAD:
				modref |= DTRACE_MODREF_THREAD_LOCAL_MOD;
				break;
			case DIFV_SCOPE_LOCAL:
				modref |= DTRACE_MODREF_CLAUSE_LOCAL_MOD;
				break;
			}
		}

		if (vp->dtdv_flags & DIFV_F_REF) {
			switch (vp->dtdv_scope) {
			case DIFV_SCOPE_GLOBAL:
				modref |= DTRACE_MODREF_GLOBAL_REF;
				break;
			case DIFV_SCOPE_THREAD:
				modref |= DTRACE_MODREF_THREAD_LOCAL_REF;
				break;
			case DIFV_SCOPE_LOCAL:
				modref |= DTRACE_MODREF_CLAUSE_LOCAL_REF;
				break;
			}
		}
	}

	/* Check implicit mod/ref behaviour of subroutine calls within DIF */
	for (i = 0; i < dp->dtdo_len; i++) {
		ip = dp->dtdo_buf + i;

		if (DIF_INSTR_OP(*ip) == CALL_OPCODE) {
			modref |= dtrace_modref_call(ip);
		}
	}

	return (modref);
}

static void
print_action(dtrace_actdesc_t *ap, const char *probename, FILE *dot_output)
{
	dtrace_actkind_t kind = ap->dtad_kind;

	if (kind == DTRACEACT_DIFEXPR) {
		print_difo(ap->dtad_difo, probename, dot_output);

	} else if (DTRACEACT_CLASS(kind) == DTRACEACT_SPECULATIVE) {
		/* TODO */

	} else if (DTRACEACT_ISAGG(kind)) {
		/* TODO */

	}
}

static void
print_difo(dtrace_difo_t *dp, const char *probename, FILE *dot_output)
{
	char label[DTRACE_FULLNAMELEN];
	char name[DTRACE_FULLNAMELEN];
	dtrace_difv_t *vp;
	size_t remaining;
	char *cp;
	int i;

	/*
	 * Iterate over the difo, outputing any calls to builtin functioms.
	 */

	for (i = 1; i < dp->dtdo_len; i++) {
		dif_instr_t instr = dp->dtdo_buf[i];
		dif_instr_t opcode = DIF_INSTR_OP(instr);

		if (opcode == CALL_OPCODE) {
			uint_t subr = DIF_INSTR_SUBR(instr);
			stpncpy(name, dtrace_subrstr(NULL, subr), sizeof(name));
			cp = stpncpy(label, name, sizeof(label));
			stpncpy(cp, "()", 2);

			fprintf(dot_output, "\"%s\" [ label = \"%s\" ];\n",
				name, label);

			fprintf(dot_output, "\"%s\" -> \"%s\"\n",
				name, probename);

			fprintf(dot_output, "\"%s\" -> \"%s\"\n",
				probename, name);
		}
	}

	/*
	 * Iterate over symbol table, outputting mods and refs to .dot output
	 * and taking note of any mods.
	 */
	for (i = 0; i < dp->dtdo_varlen; i++) {
		vp = &dp->dtdo_vartab[i];

		/* Prefix clause-local variables with the name of the clause */
		cp = name;
		if (vp->dtdv_scope == DIFV_SCOPE_LOCAL) {
			cp = stpncpy(cp, probename, sizeof(name));
			*cp++ = ':';
		}
		cp = stpncpy(cp, dp->dtdo_strtab + vp->dtdv_name,
			sizeof(name) - (cp - name) - 1);
		assert(*cp == 0);

		/* Generate user-visible label, include the variable's scope */
		cp = label;
		cp = stpncpy(cp, name, sizeof(label) - 1);
		remaining = sizeof(label) - (cp - label) - 1;

		switch (vp->dtdv_scope) {
		case DIFV_SCOPE_GLOBAL:
			cp = stpncpy(cp, " (global)", remaining);
			break;
		case DIFV_SCOPE_THREAD:
			cp = stpncpy(cp, " (thread-local)", remaining);
			break;
		case DIFV_SCOPE_LOCAL:
			cp = stpncpy(cp, " (clause-local)", remaining);
			stpncpy(name, probename, sizeof(name));
			break;
		default:
			cp = stpncpy(cp, " (unknown scope)", remaining);
		}

		/*
		 * Output the basic information about the variable as well as
		 * references (var->probe edges) and modifications (probe->var).
		 */
		fprintf(dot_output, "\"%s\" [ label = \"%s\" ];\n",
			name, label);

		if (vp->dtdv_flags & DIFV_F_REF) {
			fprintf(dot_output, "\"%s\" -> \"%s\"\n",
				name, probename);
		}

		if (vp->dtdv_flags & DIFV_F_MOD)
		{
			fprintf(dot_output, "\"%s\" -> \"%s\"\n",
				probename, name);
		}
	}
}
