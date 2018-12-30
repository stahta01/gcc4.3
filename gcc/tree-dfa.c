/* Data flow functions for trees.
   Copyright (C) 2001, 2002, 2003, 2004, 2005, 2007 Free Software Foundation, Inc.
   Contributed by Diego Novillo <dnovillo@redhat.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "hashtab.h"
#include "pointer-set.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "output.h"
#include "timevar.h"
#include "expr.h"
#include "ggc.h"
#include "langhooks.h"
#include "flags.h"
#include "function.h"
#include "diagnostic.h"
#include "tree-dump.h"
#include "tree-gimple.h"
#include "tree-flow.h"
#include "tree-inline.h"
#include "tree-pass.h"
#include "convert.h"
#include "params.h"
#include "cgraph.h"

/* Build and maintain data flow information for trees.  */

/* Counters used to display DFA and SSA statistics.  */
struct dfa_stats_d
{
  long num_stmt_anns;
  long num_var_anns;
  long num_defs;
  long num_uses;
  long num_phis;
  long num_phi_args;
  int max_num_phi_args;
  long num_vdefs;
  long num_vuses;
};


/* Local functions.  */
static void collect_dfa_stats (struct dfa_stats_d *);
static tree collect_dfa_stats_r (tree *, int *, void *);
static tree find_vars_r (tree *, int *, void *);


/*---------------------------------------------------------------------------
			Dataflow analysis (DFA) routines
---------------------------------------------------------------------------*/
/* Find all the variables referenced in the function.  This function
   builds the global arrays REFERENCED_VARS and CALL_CLOBBERED_VARS.

   Note that this function does not look for statement operands, it simply
   determines what variables are referenced in the program and detects
   various attributes for each variable used by alias analysis and the
   optimizer.  */

static unsigned int
find_referenced_vars (void)
{
  basic_block bb;
  block_stmt_iterator si;

  FOR_EACH_BB (bb)
    for (si = bsi_start (bb); !bsi_end_p (si); bsi_next (&si))
      {
	tree *stmt_p = bsi_stmt_ptr (si);
	walk_tree (stmt_p, find_vars_r, NULL, NULL);
      }

  return 0;
}

struct tree_opt_pass pass_referenced_vars =
{
  NULL,					/* name */
  NULL,					/* gate */
  find_referenced_vars,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_FIND_REFERENCED_VARS,		/* tv_id */
  PROP_gimple_leh | PROP_cfg,		/* properties_required */
  PROP_referenced_vars,			/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0,                                    /* todo_flags_finish */
  0				        /* letter */
};


/*---------------------------------------------------------------------------
			    Manage annotations
---------------------------------------------------------------------------*/
/* Create a new annotation for a _DECL node T.  */

var_ann_t
create_var_ann (tree t)
{
  var_ann_t ann;
  struct static_var_ann_d *sann = NULL;

  gcc_assert (t);
  gcc_assert (DECL_P (t));
  gcc_assert (!t->base.ann || t->base.ann->common.type == VAR_ANN);

  if (!MTAG_P (t) && (TREE_STATIC (t) || DECL_EXTERNAL (t)))
    {
      sann = GGC_CNEW (struct static_var_ann_d);
      ann = &sann->ann;
    }
  else
    ann = GGC_CNEW (struct var_ann_d);

  ann->common.type = VAR_ANN;

  if (!MTAG_P (t) && (TREE_STATIC (t) || DECL_EXTERNAL (t)))
    {
       void **slot;
       sann->uid = DECL_UID (t);
       slot = htab_find_slot_with_hash (gimple_var_anns (cfun),
				        t, DECL_UID (t), INSERT);
       gcc_assert (!*slot);
       *slot = sann;
    }
  else
    t->base.ann = (tree_ann_t) ann;

  return ann;
}

/* Create a new annotation for a FUNCTION_DECL node T.  */

function_ann_t
create_function_ann (tree t)
{
  function_ann_t ann;

  gcc_assert (t);
  gcc_assert (TREE_CODE (t) == FUNCTION_DECL);
  gcc_assert (!t->base.ann || t->base.ann->common.type == FUNCTION_ANN);

  ann = ggc_alloc (sizeof (*ann));
  memset ((void *) ann, 0, sizeof (*ann));

  ann->common.type = FUNCTION_ANN;

  t->base.ann = (tree_ann_t) ann;

  return ann;
}

/* Create a new annotation for a statement node T.  */

stmt_ann_t
create_stmt_ann (tree t)
{
  stmt_ann_t ann;

  gcc_assert (is_gimple_stmt (t));
  gcc_assert (!t->base.ann || t->base.ann->common.type == STMT_ANN);

  ann = GGC_CNEW (struct stmt_ann_d);

  ann->common.type = STMT_ANN;

  /* Since we just created the annotation, mark the statement modified.  */
  ann->modified = true;

  t->base.ann = (tree_ann_t) ann;

  return ann;
}

/* Create a new annotation for a tree T.  */

tree_ann_common_t
create_tree_common_ann (tree t)
{
  tree_ann_common_t ann;

  gcc_assert (t);
  gcc_assert (!t->base.ann || t->base.ann->common.type == TREE_ANN_COMMON);

  ann = GGC_CNEW (struct tree_ann_common_d);

  ann->type = TREE_ANN_COMMON;
  t->base.ann = (tree_ann_t) ann;

  return ann;
}

/* Build a temporary.  Make sure and register it to be renamed.  */

tree
make_rename_temp (tree type, const char *prefix)
{
  tree t = create_tmp_var (type, prefix);

  if (TREE_CODE (TREE_TYPE (t)) == COMPLEX_TYPE
      || TREE_CODE (TREE_TYPE (t)) == VECTOR_TYPE)
    DECL_GIMPLE_REG_P (t) = 1;

  if (gimple_referenced_vars (cfun))
    {
      add_referenced_var (t);
      mark_sym_for_renaming (t);
    }

  return t;
}



/*---------------------------------------------------------------------------
			      Debugging functions
---------------------------------------------------------------------------*/
/* Dump the list of all the referenced variables in the current function to
   FILE.  */

void
dump_referenced_vars (FILE *file)
{
  tree var;
  referenced_var_iterator rvi;
  
  fprintf (file, "\nReferenced variables in %s: %u\n\n",
	   get_name (current_function_decl), (unsigned) num_referenced_vars);
  
  FOR_EACH_REFERENCED_VAR (var, rvi)
    {
      fprintf (file, "Variable: ");
      dump_variable (file, var);
      fprintf (file, "\n");
    }
}


/* Dump the list of all the referenced variables to stderr.  */

void
debug_referenced_vars (void)
{
  dump_referenced_vars (stderr);
}


/* Dump sub-variables for VAR to FILE.  */

void
dump_subvars_for (FILE *file, tree var)
{
  subvar_t sv = get_subvars_for_var (var);
  tree subvar;
  unsigned int i;

  if (!sv)
    return;

  fprintf (file, "{ ");

  for (i = 0; VEC_iterate (tree, sv, i, subvar); ++i)
    {
      print_generic_expr (file, subvar, dump_flags);
      fprintf (file, "@" HOST_WIDE_INT_PRINT_UNSIGNED, SFT_OFFSET (subvar));
      if (SFT_BASE_FOR_COMPONENTS_P (subvar))
        fprintf (file, "[B]");
      fprintf (file, " ");
    }

  fprintf (file, "}");
}


/* Dumb sub-variables for VAR to stderr.  */

void
debug_subvars_for (tree var)
{
  dump_subvars_for (stderr, var);
}


/* Dump variable VAR and its may-aliases to FILE.  */

void
dump_variable (FILE *file, tree var)
{
  var_ann_t ann;

  if (TREE_CODE (var) == SSA_NAME)
    {
      if (POINTER_TYPE_P (TREE_TYPE (var)))
	dump_points_to_info_for (file, var);
      var = SSA_NAME_VAR (var);
    }

  if (var == NULL_TREE)
    {
      fprintf (file, "<nil>");
      return;
    }

  print_generic_expr (file, var, dump_flags);

  ann = var_ann (var);

  fprintf (file, ", UID D.%u", (unsigned) DECL_UID (var));

  fprintf (file, ", ");
  print_generic_expr (file, TREE_TYPE (var), dump_flags);

  if (ann && ann->symbol_mem_tag)
    {
      fprintf (file, ", symbol memory tag: ");
      print_generic_expr (file, ann->symbol_mem_tag, dump_flags);
    }

  if (TREE_ADDRESSABLE (var))
    fprintf (file, ", is addressable");
  
  if (is_global_var (var))
    fprintf (file, ", is global");

  if (TREE_THIS_VOLATILE (var))
    fprintf (file, ", is volatile");

  dump_mem_sym_stats_for_var (file, var);

  if (is_call_clobbered (var))
    {
      const char *s = "";
      var_ann_t va = var_ann (var);
      unsigned int escape_mask = va->escape_mask;

      fprintf (file, ", call clobbered");
      fprintf (file, " (");
      if (escape_mask & ESCAPE_STORED_IN_GLOBAL)
	{ fprintf (file, "%sstored in global", s); s = ", "; }
      if (escape_mask & ESCAPE_TO_ASM)
	{ fprintf (file, "%sgoes through ASM", s); s = ", "; }
      if (escape_mask & ESCAPE_TO_CALL)
	{ fprintf (file, "%spassed to call", s); s = ", "; }
      if (escape_mask & ESCAPE_BAD_CAST)
	{ fprintf (file, "%sbad cast", s); s = ", "; }
      if (escape_mask & ESCAPE_TO_RETURN)
	{ fprintf (file, "%sreturned from func", s); s = ", "; }
      if (escape_mask & ESCAPE_TO_PURE_CONST)
	{ fprintf (file, "%spassed to pure/const", s); s = ", "; }
      if (escape_mask & ESCAPE_IS_GLOBAL)
	{ fprintf (file, "%sis global var", s); s = ", "; }
      if (escape_mask & ESCAPE_IS_PARM)
	{ fprintf (file, "%sis incoming pointer", s); s = ", "; }
      if (escape_mask & ESCAPE_UNKNOWN)
	{ fprintf (file, "%sunknown escape", s); s = ", "; }
      fprintf (file, ")");
    }

  if (ann->noalias_state == NO_ALIAS)
    fprintf (file, ", NO_ALIAS (does not alias other NO_ALIAS symbols)");
  else if (ann->noalias_state == NO_ALIAS_GLOBAL)
    fprintf (file, ", NO_ALIAS_GLOBAL (does not alias other NO_ALIAS symbols"
	           " and global vars)");
  else if (ann->noalias_state == NO_ALIAS_ANYTHING)
    fprintf (file, ", NO_ALIAS_ANYTHING (does not alias any other symbols)");

  if (gimple_default_def (cfun, var))
    {
      fprintf (file, ", default def: ");
      print_generic_expr (file, gimple_default_def (cfun, var), dump_flags);
    }

  if (MTAG_P (var) && may_aliases (var))
    {
      fprintf (file, ", may aliases: ");
      dump_may_aliases_for (file, var);
    }

  if (get_subvars_for_var (var))
    {
      fprintf (file, ", sub-vars: ");
      dump_subvars_for (file, var);
    }

  if (!is_gimple_reg (var))
    {
      if (memory_partition (var))
	{
	  fprintf (file, ", belongs to partition: ");
	  print_generic_expr (file, memory_partition (var), dump_flags);
	}

      if (TREE_CODE (var) == MEMORY_PARTITION_TAG)
	{
	  fprintf (file, ", partition symbols: ");
	  dump_decl_set (file, MPT_SYMBOLS (var));
	}

      if (TREE_CODE (var) == STRUCT_FIELD_TAG)
	{
	  fprintf (file, ", offset: " HOST_WIDE_INT_PRINT_UNSIGNED,
		   SFT_OFFSET (var));
	  fprintf (file, ", base for components: %s",
		   SFT_BASE_FOR_COMPONENTS_P (var) ? "NO" : "YES");
	  fprintf (file, ", partitionable: %s",
		   SFT_UNPARTITIONABLE_P (var) ? "NO" : "YES");
	}
    }

  fprintf (file, "\n");
}


/* Dump variable VAR and its may-aliases to stderr.  */

void
debug_variable (tree var)
{
  dump_variable (stderr, var);
}


/* Dump various DFA statistics to FILE.  */

void
dump_dfa_stats (FILE *file)
{
  struct dfa_stats_d dfa_stats;

  unsigned long size, total = 0;
  const char * const fmt_str   = "%-30s%-13s%12s\n";
  const char * const fmt_str_1 = "%-30s%13lu%11lu%c\n";
  const char * const fmt_str_3 = "%-43s%11lu%c\n";
  const char *funcname
    = lang_hooks.decl_printable_name (current_function_decl, 2);

  collect_dfa_stats (&dfa_stats);

  fprintf (file, "\nDFA Statistics for %s\n\n", funcname);

  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, fmt_str, "", "  Number of  ", "Memory");
  fprintf (file, fmt_str, "", "  instances  ", "used ");
  fprintf (file, "---------------------------------------------------------\n");

  size = num_referenced_vars * sizeof (tree);
  total += size;
  fprintf (file, fmt_str_1, "Referenced variables", (unsigned long)num_referenced_vars,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_stmt_anns * sizeof (struct stmt_ann_d);
  total += size;
  fprintf (file, fmt_str_1, "Statements annotated", dfa_stats.num_stmt_anns,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_var_anns * sizeof (struct var_ann_d);
  total += size;
  fprintf (file, fmt_str_1, "Variables annotated", dfa_stats.num_var_anns,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_uses * sizeof (tree *);
  total += size;
  fprintf (file, fmt_str_1, "USE operands", dfa_stats.num_uses,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_defs * sizeof (tree *);
  total += size;
  fprintf (file, fmt_str_1, "DEF operands", dfa_stats.num_defs,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_vuses * sizeof (tree *);
  total += size;
  fprintf (file, fmt_str_1, "VUSE operands", dfa_stats.num_vuses,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_vdefs * sizeof (tree *);
  total += size;
  fprintf (file, fmt_str_1, "VDEF operands", dfa_stats.num_vdefs,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_phis * sizeof (struct tree_phi_node);
  total += size;
  fprintf (file, fmt_str_1, "PHI nodes", dfa_stats.num_phis,
	   SCALE (size), LABEL (size));

  size = dfa_stats.num_phi_args * sizeof (struct phi_arg_d);
  total += size;
  fprintf (file, fmt_str_1, "PHI arguments", dfa_stats.num_phi_args,
 	   SCALE (size), LABEL (size));

  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, fmt_str_3, "Total memory used by DFA/SSA data", SCALE (total),
	   LABEL (total));
  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, "\n");

  if (dfa_stats.num_phis)
    fprintf (file, "Average number of arguments per PHI node: %.1f (max: %d)\n",
	     (float) dfa_stats.num_phi_args / (float) dfa_stats.num_phis,
	     dfa_stats.max_num_phi_args);

  fprintf (file, "\n");
}


/* Dump DFA statistics on stderr.  */

void
debug_dfa_stats (void)
{
  dump_dfa_stats (stderr);
}


/* Collect DFA statistics and store them in the structure pointed to by
   DFA_STATS_P.  */

static void
collect_dfa_stats (struct dfa_stats_d *dfa_stats_p)
{
  struct pointer_set_t *pset;
  basic_block bb;
  block_stmt_iterator i;

  gcc_assert (dfa_stats_p);

  memset ((void *)dfa_stats_p, 0, sizeof (struct dfa_stats_d));

  /* Walk all the trees in the function counting references.  Start at
     basic block NUM_FIXED_BLOCKS, but don't stop at block boundaries.  */
  pset = pointer_set_create ();

  for (i = bsi_start (BASIC_BLOCK (NUM_FIXED_BLOCKS));
       !bsi_end_p (i); bsi_next (&i))
    walk_tree (bsi_stmt_ptr (i), collect_dfa_stats_r, (void *) dfa_stats_p,
	       pset);

  pointer_set_destroy (pset);

  FOR_EACH_BB (bb)
    {
      tree phi;
      for (phi = phi_nodes (bb); phi; phi = PHI_CHAIN (phi))
	{
	  dfa_stats_p->num_phis++;
	  dfa_stats_p->num_phi_args += PHI_NUM_ARGS (phi);
	  if (PHI_NUM_ARGS (phi) > dfa_stats_p->max_num_phi_args)
	    dfa_stats_p->max_num_phi_args = PHI_NUM_ARGS (phi);
	}
    }
}


/* Callback for walk_tree to collect DFA statistics for a tree and its
   children.  */

static tree
collect_dfa_stats_r (tree *tp, int *walk_subtrees ATTRIBUTE_UNUSED,
		     void *data)
{
  tree t = *tp;
  struct dfa_stats_d *dfa_stats_p = (struct dfa_stats_d *)data;

  if (t->base.ann)
    {
      switch (ann_type (t->base.ann))
	{
	case STMT_ANN:
	  {
	    dfa_stats_p->num_stmt_anns++;
	    dfa_stats_p->num_defs += NUM_SSA_OPERANDS (t, SSA_OP_DEF);
	    dfa_stats_p->num_uses += NUM_SSA_OPERANDS (t, SSA_OP_USE);
	    dfa_stats_p->num_vdefs += NUM_SSA_OPERANDS (t, SSA_OP_VDEF);
	    dfa_stats_p->num_vuses += NUM_SSA_OPERANDS (t, SSA_OP_VUSE);
	    break;
	  }

	case VAR_ANN:
	  dfa_stats_p->num_var_anns++;
	  break;

	default:
	  break;
	}
    }

  return NULL;
}


/*---------------------------------------------------------------------------
			     Miscellaneous helpers
---------------------------------------------------------------------------*/
/* Callback for walk_tree.  Used to collect variables referenced in
   the function.  */

static tree
find_vars_r (tree *tp, int *walk_subtrees, void *data ATTRIBUTE_UNUSED)
{
  /* If T is a regular variable that the optimizers are interested
     in, add it to the list of variables.  */
  if (SSA_VAR_P (*tp))
    add_referenced_var (*tp);

  /* Type, _DECL and constant nodes have no interesting children.
     Ignore them.  */
  else if (IS_TYPE_OR_DECL_P (*tp) || CONSTANT_CLASS_P (*tp))
    *walk_subtrees = 0;

  return NULL_TREE;
}

/* Lookup UID in the referenced_vars hashtable and return the associated
   variable.  */

tree 
referenced_var_lookup (unsigned int uid)
{
  tree h;
  struct tree_decl_minimal in;
  in.uid = uid;
  h = (tree) htab_find_with_hash (gimple_referenced_vars (cfun), &in, uid);
  gcc_assert (h || uid == 0);
  return h;
}

/* Check if TO is in the referenced_vars hash table and insert it if not.  
   Return true if it required insertion.  */

bool
referenced_var_check_and_insert (tree to)
{ 
  tree h, *loc;
  struct tree_decl_minimal in;
  unsigned int uid = DECL_UID (to);

  in.uid = uid;
  h = (tree) htab_find_with_hash (gimple_referenced_vars (cfun), &in, uid);
  if (h)
    {
      /* DECL_UID has already been entered in the table.  Verify that it is
	 the same entry as TO.  See PR 27793.  */
      gcc_assert (h == to);
      return false;
    }

  loc = (tree *) htab_find_slot_with_hash (gimple_referenced_vars (cfun),
					   &in, uid, INSERT);
  *loc = to;
  return true;
}

/* Lookup VAR UID in the default_defs hashtable and return the associated
   variable.  */

tree 
gimple_default_def (struct function *fn, tree var)
{
  struct tree_decl_minimal ind;
  struct tree_ssa_name in;
  gcc_assert (SSA_VAR_P (var));
  in.var = (tree)&ind;
  ind.uid = DECL_UID (var);
  return (tree) htab_find_with_hash (DEFAULT_DEFS (fn), &in, DECL_UID (var));
}

/* Insert the pair VAR's UID, DEF into the default_defs hashtable.  */

void
set_default_def (tree var, tree def)
{ 
  struct tree_decl_minimal ind;
  struct tree_ssa_name in;
  void **loc;

  gcc_assert (SSA_VAR_P (var));
  in.var = (tree)&ind;
  ind.uid = DECL_UID (var);
  if (!def)
    {
      loc = htab_find_slot_with_hash (DEFAULT_DEFS (cfun), &in,
            DECL_UID (var), INSERT);
      gcc_assert (*loc);
      htab_remove_elt (DEFAULT_DEFS (cfun), *loc);
      return;
    }
  gcc_assert (TREE_CODE (def) == SSA_NAME && SSA_NAME_VAR (def) == var);
  loc = htab_find_slot_with_hash (DEFAULT_DEFS (cfun), &in,
                                  DECL_UID (var), INSERT);

  /* Default definition might be changed by tail call optimization.  */
  if (*loc)
    SSA_NAME_IS_DEFAULT_DEF (*(tree *) loc) = false;
  *(tree *) loc = def;

   /* Mark DEF as the default definition for VAR.  */
   SSA_NAME_IS_DEFAULT_DEF (def) = true;
}

/* Add VAR to the list of referenced variables if it isn't already there.  */

void
add_referenced_var (tree var)
{
  var_ann_t v_ann;

  v_ann = get_var_ann (var);
  gcc_assert (DECL_P (var));
  
  /* Insert VAR into the referenced_vars has table if it isn't present.  */
  if (referenced_var_check_and_insert (var))
    {
      /* This is the first time we found this variable, annotate it with
	 attributes that are intrinsic to the variable.  */
      
      /* Tag's don't have DECL_INITIAL.  */
      if (MTAG_P (var))
	return;

      /* Scan DECL_INITIAL for pointer variables as they may contain
	 address arithmetic referencing the address of other
	 variables.  
	 Even non-constant intializers need to be walked, because
	 IPA passes might prove that their are invariant later on.  */
      if (DECL_INITIAL (var)
	  /* Initializers of external variables are not useful to the
	     optimizers.  */
          && !DECL_EXTERNAL (var))
      	walk_tree (&DECL_INITIAL (var), find_vars_r, NULL, 0);
    }
}

/* Remove VAR from the list.  */

void
remove_referenced_var (tree var)
{
  var_ann_t v_ann;
  struct tree_decl_minimal in;
  void **loc;
  unsigned int uid = DECL_UID (var);
  subvar_t sv;

  /* If we remove a var, we should also remove its subvars, as we kill
     their parent var and its annotation.  */
  if (var_can_have_subvars (var)
      && (sv = get_subvars_for_var (var)))
    {
      unsigned int i;
      tree subvar;
      for (i = 0; VEC_iterate (tree, sv, i, subvar); ++i)
        remove_referenced_var (subvar);
    }

  clear_call_clobbered (var);
  if ((v_ann = var_ann (var)))
    ggc_free (v_ann);
  var->base.ann = NULL;
  gcc_assert (DECL_P (var));
  in.uid = uid;
  loc = htab_find_slot_with_hash (gimple_referenced_vars (cfun), &in, uid,
				  NO_INSERT);
  htab_clear_slot (gimple_referenced_vars (cfun), loc);
}


/* Return the virtual variable associated to the non-scalar variable VAR.  */

tree
get_virtual_var (tree var)
{
  STRIP_NOPS (var);

  if (TREE_CODE (var) == SSA_NAME)
    var = SSA_NAME_VAR (var);

  while (TREE_CODE (var) == REALPART_EXPR || TREE_CODE (var) == IMAGPART_EXPR
	 || handled_component_p (var))
    var = TREE_OPERAND (var, 0);

  /* Treating GIMPLE registers as virtual variables makes no sense.
     Also complain if we couldn't extract a _DECL out of the original
     expression.  */
  gcc_assert (SSA_VAR_P (var));
  gcc_assert (!is_gimple_reg (var));

  return var;
}

/* Mark all the naked symbols in STMT for SSA renaming.
   
   NOTE: This function should only be used for brand new statements.
   If the caller is modifying an existing statement, it should use the
   combination push_stmt_changes/pop_stmt_changes.  */

void
mark_symbols_for_renaming (tree stmt)
{
  tree op;
  ssa_op_iter iter;

  update_stmt (stmt);

  /* Mark all the operands for renaming.  */
  FOR_EACH_SSA_TREE_OPERAND (op, stmt, iter, SSA_OP_ALL_OPERANDS)
    if (DECL_P (op))
      mark_sym_for_renaming (op);
}


/* Find all variables within the gimplified statement that were not previously
   visible to the function and add them to the referenced variables list.  */

static tree
find_new_referenced_vars_1 (tree *tp, int *walk_subtrees,
			    void *data ATTRIBUTE_UNUSED)
{
  tree t = *tp;

  if (TREE_CODE (t) == VAR_DECL && !var_ann (t))
    {
      add_referenced_var (t);
      mark_sym_for_renaming (t);
    }

  if (IS_TYPE_OR_DECL_P (t))
    *walk_subtrees = 0;

  return NULL;
}

void
find_new_referenced_vars (tree *stmt_p)
{
  walk_tree (stmt_p, find_new_referenced_vars_1, NULL, NULL);
}


/* If EXP is a handled component reference for a structure, return the
   base variable.  The access range is delimited by bit positions *POFFSET and
   *POFFSET + *PMAX_SIZE.  The access size is *PSIZE bits.  If either
   *PSIZE or *PMAX_SIZE is -1, they could not be determined.  If *PSIZE
   and *PMAX_SIZE are equal, the access is non-variable.  */

tree
get_ref_base_and_extent (tree exp, HOST_WIDE_INT *poffset,
			 HOST_WIDE_INT *psize,
			 HOST_WIDE_INT *pmax_size)
{
  HOST_WIDE_INT bitsize = -1;
  HOST_WIDE_INT maxsize = -1;
  tree size_tree = NULL_TREE;
  HOST_WIDE_INT bit_offset = 0;
  bool seen_variable_array_ref = false;

  gcc_assert (!SSA_VAR_P (exp));

  /* First get the final access size from just the outermost expression.  */
  if (TREE_CODE (exp) == COMPONENT_REF)
    size_tree = DECL_SIZE (TREE_OPERAND (exp, 1));
  else if (TREE_CODE (exp) == BIT_FIELD_REF)
    size_tree = TREE_OPERAND (exp, 1);
  else
    {
      enum machine_mode mode = TYPE_MODE (TREE_TYPE (exp));
      if (mode == BLKmode)
	size_tree = TYPE_SIZE (TREE_TYPE (exp));
      else
	bitsize = GET_MODE_BITSIZE (mode);
    }
  if (size_tree != NULL_TREE)
    {
      if (! host_integerp (size_tree, 1))
	bitsize = -1;
      else
	bitsize = TREE_INT_CST_LOW (size_tree);
    }

  /* Initially, maxsize is the same as the accessed element size.
     In the following it will only grow (or become -1).  */
  maxsize = bitsize;

  /* Compute cumulative bit-offset for nested component-refs and array-refs,
     and find the ultimate containing object.  */
  while (1)
    {
      switch (TREE_CODE (exp))
	{
	case BIT_FIELD_REF:
	  bit_offset += tree_low_cst (TREE_OPERAND (exp, 2), 0);
	  break;

	case COMPONENT_REF:
	  {
	    tree field = TREE_OPERAND (exp, 1);
	    tree this_offset = component_ref_field_offset (exp);

	    if (this_offset && TREE_CODE (this_offset) == INTEGER_CST)
	      {
		HOST_WIDE_INT hthis_offset = tree_low_cst (this_offset, 0);

		hthis_offset *= BITS_PER_UNIT;
		bit_offset += hthis_offset;
		bit_offset += tree_low_cst (DECL_FIELD_BIT_OFFSET (field), 0);
	      }
	    else
	      {
		tree csize = TYPE_SIZE (TREE_TYPE (TREE_OPERAND (exp, 0)));
		/* We need to adjust maxsize to the whole structure bitsize.
		   But we can subtract any constant offset seen sofar,
		   because that would get us out of the structure otherwise.  */
		if (maxsize != -1 && csize && host_integerp (csize, 1))
		  maxsize = TREE_INT_CST_LOW (csize) - bit_offset;
		else
		  maxsize = -1;
	      }
	  }
	  break;

	case ARRAY_REF:
	case ARRAY_RANGE_REF:
	  {
	    tree index = TREE_OPERAND (exp, 1);
	    tree low_bound = array_ref_low_bound (exp);
	    tree unit_size = array_ref_element_size (exp);

	    /* If the resulting bit-offset is constant, track it.  */
	    if (host_integerp (index, 0)
		&& host_integerp (low_bound, 0)
		&& host_integerp (unit_size, 1))
	      {
		HOST_WIDE_INT hindex = tree_low_cst (index, 0);

		hindex -= tree_low_cst (low_bound, 0);
		hindex *= tree_low_cst (unit_size, 1);
		hindex *= BITS_PER_UNIT;
		bit_offset += hindex;

		/* An array ref with a constant index up in the structure
		   hierarchy will constrain the size of any variable array ref
		   lower in the access hierarchy.  */
		seen_variable_array_ref = false;
	      }
	    else
	      {
		tree asize = TYPE_SIZE (TREE_TYPE (TREE_OPERAND (exp, 0)));
		/* We need to adjust maxsize to the whole array bitsize.
		   But we can subtract any constant offset seen sofar,
		   because that would get us outside of the array otherwise.  */
		if (maxsize != -1 && asize && host_integerp (asize, 1))
		  maxsize = TREE_INT_CST_LOW (asize) - bit_offset;
		else
		  maxsize = -1;

		/* Remember that we have seen an array ref with a variable
		   index.  */
		seen_variable_array_ref = true;
	      }
	  }
	  break;

	case REALPART_EXPR:
	  break;

	case IMAGPART_EXPR:
	  bit_offset += bitsize;
	  break;

	case VIEW_CONVERT_EXPR:
	  /* ???  We probably should give up here and bail out.  */
	  break;

	default:
	  goto done;
	}

      exp = TREE_OPERAND (exp, 0);
    }
 done:

  /* We need to deal with variable arrays ending structures such as
       struct { int length; int a[1]; } x;           x.a[d]
       struct { struct { int a; int b; } a[1]; } x;  x.a[d].a
       struct { struct { int a[1]; } a[1]; } x;      x.a[0][d], x.a[d][0]
     where we do not know maxsize for variable index accesses to
     the array.  The simplest way to conservatively deal with this
     is to punt in the case that offset + maxsize reaches the
     base type boundary.  */
  if (seen_variable_array_ref
      && maxsize != -1
      && host_integerp (TYPE_SIZE (TREE_TYPE (exp)), 1)
      && bit_offset + maxsize
	   == (signed)TREE_INT_CST_LOW (TYPE_SIZE (TREE_TYPE (exp))))
    maxsize = -1;

  /* ???  Due to negative offsets in ARRAY_REF we can end up with
     negative bit_offset here.  We might want to store a zero offset
     in this case.  */
  *poffset = bit_offset;
  *psize = bitsize;
  *pmax_size = maxsize;

  return exp;
}

/* Returns true if STMT references an SSA_NAME that has
   SSA_NAME_OCCURS_IN_ABNORMAL_PHI set, otherwise false.  */

bool
stmt_references_abnormal_ssa_name (tree stmt)
{
  ssa_op_iter oi;
  use_operand_p use_p;

  FOR_EACH_SSA_USE_OPERAND (use_p, stmt, oi, SSA_OP_USE)
    {
      if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (USE_FROM_PTR (use_p)))
	return true;
    }

  return false;
}

