/* SSA-PRE for trees.
   Copyright (C) 2001, 2002, 2003, 2004, 2005, 2006, 2007
   Free Software Foundation, Inc.
   Contributed by Daniel Berlin <dan@dberlin.org> and Steven Bosscher
   <stevenb@suse.de>

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
#include "ggc.h"
#include "tree.h"
#include "basic-block.h"
#include "diagnostic.h"
#include "tree-inline.h"
#include "tree-flow.h"
#include "tree-gimple.h"
#include "tree-dump.h"
#include "timevar.h"
#include "fibheap.h"
#include "hashtab.h"
#include "tree-iterator.h"
#include "real.h"
#include "alloc-pool.h"
#include "obstack.h"
#include "tree-pass.h"
#include "flags.h"
#include "bitmap.h"
#include "langhooks.h"
#include "cfgloop.h"
#include "tree-ssa-sccvn.h"
#include "params.h"

/* TODO:

   1. Avail sets can be shared by making an avail_find_leader that
      walks up the dominator tree and looks in those avail sets.
      This might affect code optimality, it's unclear right now.
   2. Strength reduction can be performed by anticipating expressions
      we can repair later on.
   3. We can do back-substitution or smarter value numbering to catch
      commutative expressions split up over multiple statements.
*/

/* For ease of terminology, "expression node" in the below refers to
   every expression node but GIMPLE_MODIFY_STMT, because GIMPLE_MODIFY_STMT's
   represent the actual statement containing the expressions we care about,
   and we cache the value number by putting it in the expression.  */

/* Basic algorithm

   First we walk the statements to generate the AVAIL sets, the
   EXP_GEN sets, and the tmp_gen sets.  EXP_GEN sets represent the
   generation of values/expressions by a given block.  We use them
   when computing the ANTIC sets.  The AVAIL sets consist of
   SSA_NAME's that represent values, so we know what values are
   available in what blocks.  AVAIL is a forward dataflow problem.  In
   SSA, values are never killed, so we don't need a kill set, or a
   fixpoint iteration, in order to calculate the AVAIL sets.  In
   traditional parlance, AVAIL sets tell us the downsafety of the
   expressions/values.

   Next, we generate the ANTIC sets.  These sets represent the
   anticipatable expressions.  ANTIC is a backwards dataflow
   problem.  An expression is anticipatable in a given block if it could
   be generated in that block.  This means that if we had to perform
   an insertion in that block, of the value of that expression, we
   could.  Calculating the ANTIC sets requires phi translation of
   expressions, because the flow goes backwards through phis.  We must
   iterate to a fixpoint of the ANTIC sets, because we have a kill
   set.  Even in SSA form, values are not live over the entire
   function, only from their definition point onwards.  So we have to
   remove values from the ANTIC set once we go past the definition
   point of the leaders that make them up.
   compute_antic/compute_antic_aux performs this computation.

   Third, we perform insertions to make partially redundant
   expressions fully redundant.

   An expression is partially redundant (excluding partial
   anticipation) if:

   1. It is AVAIL in some, but not all, of the predecessors of a
      given block.
   2. It is ANTIC in all the predecessors.

   In order to make it fully redundant, we insert the expression into
   the predecessors where it is not available, but is ANTIC.

   For the partial anticipation case, we only perform insertion if it
   is partially anticipated in some block, and fully available in all
   of the predecessors.

   insert/insert_aux/do_regular_insertion/do_partial_partial_insertion
   performs these steps.

   Fourth, we eliminate fully redundant expressions.
   This is a simple statement walk that replaces redundant
   calculations with the now available values.  */

/* Representations of value numbers:

   Value numbers are represented using the "value handle" approach.
   This means that each SSA_NAME (and for other reasons to be
   disclosed in a moment, expression nodes) has a value handle that
   can be retrieved through get_value_handle.  This value handle *is*
   the value number of the SSA_NAME.  You can pointer compare the
   value handles for equivalence purposes.

   For debugging reasons, the value handle is internally more than
   just a number, it is a VALUE_HANDLE named "VH.x", where x is a
   unique number for each value number in use.  This allows
   expressions with SSA_NAMES replaced by value handles to still be
   pretty printed in a sane way.  They simply print as "VH.3 *
   VH.5", etc.

   Expression nodes have value handles associated with them as a
   cache.  Otherwise, we'd have to look them up again in the hash
   table This makes significant difference (factor of two or more) on
   some test cases.  They can be thrown away after the pass is
   finished.  */

/* Representation of expressions on value numbers:

   In some portions of this code, you will notice we allocate "fake"
   analogues to the expression we are value numbering, and replace the
   operands with the values of the expression.  Since we work on
   values, and not just names, we canonicalize expressions to value
   expressions for use in the ANTIC sets, the EXP_GEN set, etc.

   This is theoretically unnecessary, it just saves a bunch of
   repeated get_value_handle and find_leader calls in the remainder of
   the code, trading off temporary memory usage for speed.  The tree
   nodes aren't actually creating more garbage, since they are
   allocated in a special pools which are thrown away at the end of
   this pass.

   All of this also means that if you print the EXP_GEN or ANTIC sets,
   you will see "VH.5 + VH.7" in the set, instead of "a_55 +
   b_66" or something.  The only thing that actually cares about
   seeing the value leaders is phi translation, and it needs to be
   able to find the leader for a value in an arbitrary block, so this
   "value expression" form is perfect for it (otherwise you'd do
   get_value_handle->find_leader->translate->get_value_handle->find_leader).*/


/* Representation of sets:

   There are currently two types of sets used, hopefully to be unified soon.
   The AVAIL sets do not need to be sorted in any particular order,
   and thus, are simply represented as two bitmaps, one that keeps
   track of values present in the set, and one that keeps track of
   expressions present in the set.

   The other sets are represented as doubly linked lists kept in topological
   order, with an optional supporting bitmap of values present in the
   set.  The sets represent values, and the elements can be values or
   expressions.  The elements can appear in different sets, but each
   element can only appear once in each set.

   Since each node in the set represents a value, we also want to be
   able to map expression, set pairs to something that tells us
   whether the value is present is a set.  We use a per-set bitmap for
   that.  The value handles also point to a linked list of the
   expressions they represent via a tree annotation.  This is mainly
   useful only for debugging, since we don't do identity lookups.  */


/* Next global expression id number.  */
static unsigned int next_expression_id;

typedef VEC(tree, gc) *vuse_vec;
DEF_VEC_P (vuse_vec);
DEF_VEC_ALLOC_P (vuse_vec, heap);

static VEC(vuse_vec, heap) *expression_vuses;

/* Mapping from expression to id number we can use in bitmap sets.  */
static VEC(tree, heap) *expressions;

/* Allocate an expression id for EXPR.  */

static inline unsigned int
alloc_expression_id (tree expr)
{
  tree_ann_common_t ann;

  ann = get_tree_common_ann (expr);

  /* Make sure we won't overflow. */
  gcc_assert (next_expression_id + 1 > next_expression_id);

  ann->aux = XNEW (unsigned int);
  * ((unsigned int *)ann->aux) = next_expression_id++;
  VEC_safe_push (tree, heap, expressions, expr);
  VEC_safe_push (vuse_vec, heap, expression_vuses, NULL);
  return next_expression_id - 1;
}

/* Return the expression id for tree EXPR.  */

static inline unsigned int
get_expression_id (tree expr)
{
  tree_ann_common_t ann = tree_common_ann (expr);
  gcc_assert (ann);
  gcc_assert (ann->aux);

  return  *((unsigned int *)ann->aux);
}

/* Return the existing expression id for EXPR, or create one if one
   does not exist yet.  */

static inline unsigned int
get_or_alloc_expression_id (tree expr)
{
  tree_ann_common_t ann = tree_common_ann (expr);

  if (ann == NULL || !ann->aux)
    return alloc_expression_id (expr);

  return get_expression_id (expr);
}

/* Return the expression that has expression id ID */

static inline tree
expression_for_id (unsigned int id)
{
  return VEC_index (tree, expressions, id);
}

/* Return the expression vuses for EXPR, if there are any.  */

static inline vuse_vec
get_expression_vuses (tree expr)
{
  unsigned int expr_id = get_or_alloc_expression_id (expr);
  return VEC_index (vuse_vec, expression_vuses, expr_id);
}

/* Set the expression vuses for EXPR to VUSES.  */

static inline void
set_expression_vuses (tree expr, vuse_vec vuses)
{
  unsigned int expr_id = get_or_alloc_expression_id (expr);
  VEC_replace (vuse_vec, expression_vuses, expr_id, vuses);
}


/* Free the expression id field in all of our expressions,
   and then destroy the expressions array.  */

static void
clear_expression_ids (void)
{
  int i;
  tree expr;

  for (i = 0; VEC_iterate (tree, expressions, i, expr); i++)
    {
      free (tree_common_ann (expr)->aux);
      tree_common_ann (expr)->aux = NULL;
    }
  VEC_free (tree, heap, expressions);
  VEC_free (vuse_vec, heap, expression_vuses);
}

static bool in_fre = false;

/* An unordered bitmap set.  One bitmap tracks values, the other,
   expressions.  */
typedef struct bitmap_set
{
  bitmap expressions;
  bitmap values;
} *bitmap_set_t;

#define FOR_EACH_EXPR_ID_IN_SET(set, id, bi)		\
  EXECUTE_IF_SET_IN_BITMAP(set->expressions, 0, id, bi)

/* Sets that we need to keep track of.  */
typedef struct bb_bitmap_sets
{
  /* The EXP_GEN set, which represents expressions/values generated in
     a basic block.  */
  bitmap_set_t exp_gen;

  /* The PHI_GEN set, which represents PHI results generated in a
     basic block.  */
  bitmap_set_t phi_gen;

  /* The TMP_GEN set, which represents results/temporaries generated
     in a basic block. IE the LHS of an expression.  */
  bitmap_set_t tmp_gen;

  /* The AVAIL_OUT set, which represents which values are available in
     a given basic block.  */
  bitmap_set_t avail_out;

  /* The ANTIC_IN set, which represents which values are anticipatable
     in a given basic block.  */
  bitmap_set_t antic_in;

  /* The PA_IN set, which represents which values are
     partially anticipatable in a given basic block.  */
  bitmap_set_t pa_in;

  /* The NEW_SETS set, which is used during insertion to augment the
     AVAIL_OUT set of blocks with the new insertions performed during
     the current iteration.  */
  bitmap_set_t new_sets;

  /* True if we have visited this block during ANTIC calculation.  */
  unsigned int visited:1;

  /* True we have deferred processing this block during ANTIC
     calculation until its successor is processed.  */
  unsigned int deferred : 1;
} *bb_value_sets_t;

#define EXP_GEN(BB)	((bb_value_sets_t) ((BB)->aux))->exp_gen
#define PHI_GEN(BB)	((bb_value_sets_t) ((BB)->aux))->phi_gen
#define TMP_GEN(BB)	((bb_value_sets_t) ((BB)->aux))->tmp_gen
#define AVAIL_OUT(BB)	((bb_value_sets_t) ((BB)->aux))->avail_out
#define ANTIC_IN(BB)	((bb_value_sets_t) ((BB)->aux))->antic_in
#define PA_IN(BB)	((bb_value_sets_t) ((BB)->aux))->pa_in
#define NEW_SETS(BB)	((bb_value_sets_t) ((BB)->aux))->new_sets
#define BB_VISITED(BB) ((bb_value_sets_t) ((BB)->aux))->visited
#define BB_DEFERRED(BB) ((bb_value_sets_t) ((BB)->aux))->deferred

/* Maximal set of values, used to initialize the ANTIC problem, which
   is an intersection problem.  */
static bitmap_set_t maximal_set;

/* Basic block list in postorder.  */
static int *postorder;

/* This structure is used to keep track of statistics on what
   optimization PRE was able to perform.  */
static struct
{
  /* The number of RHS computations eliminated by PRE.  */
  int eliminations;

  /* The number of new expressions/temporaries generated by PRE.  */
  int insertions;

  /* The number of inserts found due to partial anticipation  */
  int pa_insert;

  /* The number of new PHI nodes added by PRE.  */
  int phis;

  /* The number of values found constant.  */
  int constified;

} pre_stats;

static bool do_partial_partial;
static tree bitmap_find_leader (bitmap_set_t, tree);
static void bitmap_value_insert_into_set (bitmap_set_t, tree);
static void bitmap_value_replace_in_set (bitmap_set_t, tree);
static void bitmap_set_copy (bitmap_set_t, bitmap_set_t);
static bool bitmap_set_contains_value (bitmap_set_t, tree);
static void bitmap_insert_into_set (bitmap_set_t, tree);
static bitmap_set_t bitmap_set_new (void);
static tree create_expression_by_pieces (basic_block, tree, tree);
static tree find_or_generate_expression (basic_block, tree, tree);

/* We can add and remove elements and entries to and from sets
   and hash tables, so we use alloc pools for them.  */

static alloc_pool bitmap_set_pool;
static alloc_pool binary_node_pool;
static alloc_pool unary_node_pool;
static alloc_pool reference_node_pool;
static alloc_pool comparison_node_pool;
static alloc_pool modify_expr_node_pool;
static bitmap_obstack grand_bitmap_obstack;

/* We can't use allocation pools to hold temporary CALL_EXPR objects, since
   they are not of fixed size.  Instead, use an obstack.  */

static struct obstack temp_call_expr_obstack;


/* To avoid adding 300 temporary variables when we only need one, we
   only create one temporary variable, on demand, and build ssa names
   off that.  We do have to change the variable if the types don't
   match the current variable's type.  */
static tree pretemp;
static tree storetemp;
static tree prephitemp;

/* Set of blocks with statements that have had its EH information
   cleaned up.  */
static bitmap need_eh_cleanup;

/* Which expressions have been seen during a given phi translation.  */
static bitmap seen_during_translate;

/* The phi_translate_table caches phi translations for a given
   expression and predecessor.  */

static htab_t phi_translate_table;

/* A three tuple {e, pred, v} used to cache phi translations in the
   phi_translate_table.  */

typedef struct expr_pred_trans_d
{
  /* The expression.  */
  tree e;

  /* The predecessor block along which we translated the expression.  */
  basic_block pred;

  /* vuses associated with the expression.  */
  VEC (tree, gc) *vuses;

  /* The value that resulted from the translation.  */
  tree v;

  /* The hashcode for the expression, pred pair. This is cached for
     speed reasons.  */
  hashval_t hashcode;
} *expr_pred_trans_t;
typedef const struct expr_pred_trans_d *const_expr_pred_trans_t;

/* Return the hash value for a phi translation table entry.  */

static hashval_t
expr_pred_trans_hash (const void *p)
{
  const_expr_pred_trans_t const ve = (const_expr_pred_trans_t) p;
  return ve->hashcode;
}

/* Return true if two phi translation table entries are the same.
   P1 and P2 should point to the expr_pred_trans_t's to be compared.*/

static int
expr_pred_trans_eq (const void *p1, const void *p2)
{
  const_expr_pred_trans_t const ve1 = (const_expr_pred_trans_t) p1;
  const_expr_pred_trans_t const ve2 = (const_expr_pred_trans_t) p2;
  basic_block b1 = ve1->pred;
  basic_block b2 = ve2->pred;
  int i;
  tree vuse1;

  /* If they are not translations for the same basic block, they can't
     be equal.  */
  if (b1 != b2)
    return false;


  /* If they are for the same basic block, determine if the
     expressions are equal.  */
  if (!expressions_equal_p (ve1->e, ve2->e))
    return false;

  /* Make sure the vuses are equivalent.  */
  if (ve1->vuses == ve2->vuses)
    return true;

  if (VEC_length (tree, ve1->vuses) != VEC_length (tree, ve2->vuses))
    return false;

  for (i = 0; VEC_iterate (tree, ve1->vuses, i, vuse1); i++)
    {
      if (VEC_index (tree, ve2->vuses, i) != vuse1)
	return false;
    }

  return true;
}

/* Search in the phi translation table for the translation of
   expression E in basic block PRED with vuses VUSES.
   Return the translated value, if found, NULL otherwise.  */

static inline tree
phi_trans_lookup (tree e, basic_block pred, VEC (tree, gc) *vuses)
{
  void **slot;
  struct expr_pred_trans_d ept;

  ept.e = e;
  ept.pred = pred;
  ept.vuses = vuses;
  ept.hashcode = iterative_hash_expr (e, (unsigned long) pred);
  slot = htab_find_slot_with_hash (phi_translate_table, &ept, ept.hashcode,
				   NO_INSERT);
  if (!slot)
    return NULL;
  else
    return ((expr_pred_trans_t) *slot)->v;
}


/* Add the tuple mapping from {expression E, basic block PRED, vuses VUSES} to
   value V, to the phi translation table.  */

static inline void
phi_trans_add (tree e, tree v, basic_block pred, VEC (tree, gc) *vuses)
{
  void **slot;
  expr_pred_trans_t new_pair = XNEW (struct expr_pred_trans_d);
  new_pair->e = e;
  new_pair->pred = pred;
  new_pair->vuses = vuses;
  new_pair->v = v;
  new_pair->hashcode = iterative_hash_expr (e, (unsigned long) pred);
  slot = htab_find_slot_with_hash (phi_translate_table, new_pair,
				   new_pair->hashcode, INSERT);
  if (*slot)
    free (*slot);
  *slot = (void *) new_pair;
}


/* Return true if V is a value expression that represents itself.
   In our world, this is *only* non-value handles.  */

static inline bool
constant_expr_p (tree v)
{
  return TREE_CODE (v) != VALUE_HANDLE &&
    (TREE_CODE (v) == FIELD_DECL || is_gimple_min_invariant (v));
}

/* Add expression E to the expression set of value V.  */

void
add_to_value (tree v, tree e)
{
  /* Constants have no expression sets.  */
  if (constant_expr_p (v))
    return;

  if (VALUE_HANDLE_EXPR_SET (v) == NULL)
    VALUE_HANDLE_EXPR_SET (v) = bitmap_set_new ();

  bitmap_insert_into_set (VALUE_HANDLE_EXPR_SET (v), e);
}

/* Create a new bitmap set and return it.  */

static bitmap_set_t
bitmap_set_new (void)
{
  bitmap_set_t ret = (bitmap_set_t) pool_alloc (bitmap_set_pool);
  ret->expressions = BITMAP_ALLOC (&grand_bitmap_obstack);
  ret->values = BITMAP_ALLOC (&grand_bitmap_obstack);
  return ret;
}

/* Remove an expression EXPR from a bitmapped set.  */

static void
bitmap_remove_from_set (bitmap_set_t set, tree expr)
{
  tree val = get_value_handle (expr);

  gcc_assert (val);
  if (!constant_expr_p (val))
    {
      bitmap_clear_bit (set->values, VALUE_HANDLE_ID (val));
      bitmap_clear_bit (set->expressions, get_expression_id (expr));
    }
}

/* Insert an expression EXPR into a bitmapped set.  */

static void
bitmap_insert_into_set (bitmap_set_t set, tree expr)
{
  tree val = get_value_handle (expr);

  gcc_assert (val);
  if (!constant_expr_p (val))
    {
      bitmap_set_bit (set->values, VALUE_HANDLE_ID (val));
      bitmap_set_bit (set->expressions, get_or_alloc_expression_id (expr));
    }
}

/* Copy a bitmapped set ORIG, into bitmapped set DEST.  */

static void
bitmap_set_copy (bitmap_set_t dest, bitmap_set_t orig)
{
  bitmap_copy (dest->expressions, orig->expressions);
  bitmap_copy (dest->values, orig->values);
}


/* Free memory used up by SET.  */
static void
bitmap_set_free (bitmap_set_t set)
{
  BITMAP_FREE (set->expressions);
  BITMAP_FREE (set->values);
}


/* A comparison function for use in qsort to top sort a bitmap set.  Simply
   subtracts value handle ids, since they are created in topo-order.  */

static int
vh_compare (const void *pa, const void *pb)
{
  const tree vha = get_value_handle (*((const tree *)pa));
  const tree vhb = get_value_handle (*((const tree *)pb));

  /* This can happen when we constify things.  */
  if (constant_expr_p (vha))
    {
      if (constant_expr_p (vhb))
	return -1;
      return -1;
    }
  else if (constant_expr_p (vhb))
    return 1;
  return VALUE_HANDLE_ID (vha) - VALUE_HANDLE_ID (vhb);
}

/* Generate an topological-ordered array of bitmap set SET.  */

static VEC(tree, heap) *
sorted_array_from_bitmap_set (bitmap_set_t set)
{
  unsigned int i;
  bitmap_iterator bi;
  VEC(tree, heap) *result = NULL;

  FOR_EACH_EXPR_ID_IN_SET (set, i, bi)
    VEC_safe_push (tree, heap, result, expression_for_id (i));

  qsort (VEC_address (tree, result), VEC_length (tree, result),
	 sizeof (tree), vh_compare);

  return result;
}

/* Perform bitmapped set operation DEST &= ORIG.  */

static void
bitmap_set_and (bitmap_set_t dest, bitmap_set_t orig)
{
  bitmap_iterator bi;
  unsigned int i;

  if (dest != orig)
    {
      bitmap temp = BITMAP_ALLOC (&grand_bitmap_obstack);

      bitmap_and_into (dest->values, orig->values);
      bitmap_copy (temp, dest->expressions);
      EXECUTE_IF_SET_IN_BITMAP (temp, 0, i, bi)
	{
	  tree expr = expression_for_id (i);
	  tree val = get_value_handle (expr);
	  if (!bitmap_bit_p (dest->values, VALUE_HANDLE_ID (val)))
	    bitmap_clear_bit (dest->expressions, i);
	}
      BITMAP_FREE (temp);
    }
}

/* Subtract all values and expressions contained in ORIG from DEST.  */

static bitmap_set_t
bitmap_set_subtract (bitmap_set_t dest, bitmap_set_t orig)
{
  bitmap_set_t result = bitmap_set_new ();
  bitmap_iterator bi;
  unsigned int i;

  bitmap_and_compl (result->expressions, dest->expressions,
		    orig->expressions);

  FOR_EACH_EXPR_ID_IN_SET (result, i, bi)
    {
      tree expr = expression_for_id (i);
      tree val = get_value_handle (expr);
      bitmap_set_bit (result->values, VALUE_HANDLE_ID (val));
    }

  return result;
}

/* Subtract all the values in bitmap set B from bitmap set A.  */

static void
bitmap_set_subtract_values (bitmap_set_t a, bitmap_set_t b)
{
  unsigned int i;
  bitmap_iterator bi;
  bitmap temp = BITMAP_ALLOC (&grand_bitmap_obstack);

  bitmap_copy (temp, a->expressions);
  EXECUTE_IF_SET_IN_BITMAP (temp, 0, i, bi)
    {
      tree expr = expression_for_id (i);
      if (bitmap_set_contains_value (b, get_value_handle (expr)))
	bitmap_remove_from_set (a, expr);
    }
  BITMAP_FREE (temp);
}


/* Return true if bitmapped set SET contains the value VAL.  */

static bool
bitmap_set_contains_value (bitmap_set_t set, tree val)
{
  if (constant_expr_p (val))
    return true;

  if (!set || bitmap_empty_p (set->expressions))
    return false;

  return bitmap_bit_p (set->values, VALUE_HANDLE_ID (val));
}

static inline bool
bitmap_set_contains_expr (bitmap_set_t set, tree expr)
{
  return bitmap_bit_p (set->expressions, get_expression_id (expr));
}

/* Replace an instance of value LOOKFOR with expression EXPR in SET.  */

static void
bitmap_set_replace_value (bitmap_set_t set, tree lookfor, tree expr)
{
  bitmap_set_t exprset;
  unsigned int i;
  bitmap_iterator bi;

  if (constant_expr_p (lookfor))
    return;

  if (!bitmap_set_contains_value (set, lookfor))
    return;

  /* The number of expressions having a given value is usually
     significantly less than the total number of expressions in SET.
     Thus, rather than check, for each expression in SET, whether it
     has the value LOOKFOR, we walk the reverse mapping that tells us
     what expressions have a given value, and see if any of those
     expressions are in our set.  For large testcases, this is about
     5-10x faster than walking the bitmap.  If this is somehow a
     significant lose for some cases, we can choose which set to walk
     based on the set size.  */
  exprset = VALUE_HANDLE_EXPR_SET (lookfor);
  FOR_EACH_EXPR_ID_IN_SET (exprset, i, bi)
    {
      if (bitmap_bit_p (set->expressions, i))
	{
	  bitmap_clear_bit (set->expressions, i);
	  bitmap_set_bit (set->expressions, get_expression_id (expr));
	  return;
	}
    }
}

/* Return true if two bitmap sets are equal.  */

static bool
bitmap_set_equal (bitmap_set_t a, bitmap_set_t b)
{
  return bitmap_equal_p (a->values, b->values);
}

/* Replace an instance of EXPR's VALUE with EXPR in SET if it exists,
   and add it otherwise.  */

static void
bitmap_value_replace_in_set (bitmap_set_t set, tree expr)
{
  tree val = get_value_handle (expr);

  if (bitmap_set_contains_value (set, val))
    bitmap_set_replace_value (set, val, expr);
  else
    bitmap_insert_into_set (set, expr);
}

/* Insert EXPR into SET if EXPR's value is not already present in
   SET.  */

static void
bitmap_value_insert_into_set (bitmap_set_t set, tree expr)
{
  tree val = get_value_handle (expr);

  if (constant_expr_p (val))
    return;

  if (!bitmap_set_contains_value (set, val))
    bitmap_insert_into_set (set, expr);
}

/* Print out SET to OUTFILE.  */

static void
print_bitmap_set (FILE *outfile, bitmap_set_t set,
		  const char *setname, int blockindex)
{
  fprintf (outfile, "%s[%d] := { ", setname, blockindex);
  if (set)
    {
      bool first = true;
      unsigned i;
      bitmap_iterator bi;

      FOR_EACH_EXPR_ID_IN_SET (set, i, bi)
	{
	  tree expr = expression_for_id (i);

	  if (!first)
	    fprintf (outfile, ", ");
	  first = false;
	  print_generic_expr (outfile, expr, 0);

	  fprintf (outfile, " (");
	  print_generic_expr (outfile, get_value_handle (expr), 0);
	  fprintf (outfile, ") ");
	}
    }
  fprintf (outfile, " }\n");
}

void debug_bitmap_set (bitmap_set_t);

void
debug_bitmap_set (bitmap_set_t set)
{
  print_bitmap_set (stderr, set, "debug", 0);
}

/* Print out the expressions that have VAL to OUTFILE.  */

void
print_value_expressions (FILE *outfile, tree val)
{
  if (VALUE_HANDLE_EXPR_SET (val))
    {
      char s[10];
      sprintf (s, "VH.%04d", VALUE_HANDLE_ID (val));
      print_bitmap_set (outfile, VALUE_HANDLE_EXPR_SET (val), s, 0);
    }
}


void
debug_value_expressions (tree val)
{
  print_value_expressions (stderr, val);
}

/* Return the folded version of T if T, when folded, is a gimple
   min_invariant.  Otherwise, return T.  */

static tree
fully_constant_expression (tree t)
{
  tree folded;
  folded = fold (t);
  if (folded && is_gimple_min_invariant (folded))
    return folded;
  return t;
}

/* Make a temporary copy of a CALL_EXPR object NODE.  */

static tree
temp_copy_call_expr (tree node)
{
  return (tree) obstack_copy (&temp_call_expr_obstack, node, tree_size (node));
}

/* Translate the vuses in the VUSES vector backwards through phi nodes
   in PHIBLOCK, so that they have the value they would have in
   BLOCK. */

static VEC(tree, gc) *
translate_vuses_through_block (VEC (tree, gc) *vuses,
			       basic_block phiblock,
			       basic_block block)
{
  tree oldvuse;
  VEC(tree, gc) *result = NULL;
  int i;

  for (i = 0; VEC_iterate (tree, vuses, i, oldvuse); i++)
    {
      tree phi = SSA_NAME_DEF_STMT (oldvuse);
      if (TREE_CODE (phi) == PHI_NODE
	  && bb_for_stmt (phi) == phiblock)
	{
	  edge e = find_edge (block, bb_for_stmt (phi));
	  if (e)
	    {
	      tree def = PHI_ARG_DEF (phi, e->dest_idx);
	      if (def != oldvuse)
		{
		  if (!result)
		    result = VEC_copy (tree, gc, vuses);
		  VEC_replace (tree, result, i, def);
		}
	    }
	}
    }

  /* We avoid creating a new copy of the vuses unless something
     actually changed, so result can be NULL.  */
  if (result)
    {
      sort_vuses (result);
      return result;
    }
  return vuses;

}

/* Like find_leader, but checks for the value existing in SET1 *or*
   SET2.  This is used to avoid making a set consisting of the union
   of PA_IN and ANTIC_IN during insert.  */

static inline tree
find_leader_in_sets (tree expr, bitmap_set_t set1, bitmap_set_t set2)
{
  tree result;

  result = bitmap_find_leader (set1, expr);
  if (!result && set2)
    result = bitmap_find_leader (set2, expr);
  return result;
}

/* Translate EXPR using phis in PHIBLOCK, so that it has the values of
   the phis in PRED.  SEEN is a bitmap saying which expression we have
   translated since we started translation of the toplevel expression.
   Return NULL if we can't find a leader for each part of the
   translated expression.  */

static tree
phi_translate_1 (tree expr, bitmap_set_t set1, bitmap_set_t set2,
		 basic_block pred, basic_block phiblock, bitmap seen)
{
  tree phitrans = NULL;
  tree oldexpr = expr;

  if (expr == NULL)
    return NULL;

  if (constant_expr_p (expr))
    return expr;

  /* Phi translations of a given expression don't change.  */
  if (EXPR_P (expr) || GIMPLE_STMT_P (expr))
    {
      phitrans = phi_trans_lookup (expr, pred, get_expression_vuses (expr));
    }
  else
    phitrans = phi_trans_lookup (expr, pred, NULL);

  if (phitrans)
    return phitrans;

  /* Prevent cycles when we have recursively dependent leaders.  This
     can only happen when phi translating the maximal set.  */
  if (seen)
    {
      unsigned int expr_id = get_expression_id (expr);
      if (bitmap_bit_p (seen, expr_id))
	return NULL;
      bitmap_set_bit (seen, expr_id);
    }

  switch (TREE_CODE_CLASS (TREE_CODE (expr)))
    {
    case tcc_expression:
      return NULL;

    case tcc_vl_exp:
      {
	if (TREE_CODE (expr) != CALL_EXPR)
	  return NULL;
	else
	  {
	    tree oldfn = CALL_EXPR_FN (expr);
	    tree oldsc = CALL_EXPR_STATIC_CHAIN (expr);
	    tree newfn, newsc = NULL;
	    tree newexpr = NULL_TREE;
	    bool invariantarg = false;
	    int i, nargs;
	    VEC (tree, gc) *vuses = get_expression_vuses (expr);
	    VEC (tree, gc) *tvuses;

	    newfn = phi_translate_1 (find_leader_in_sets (oldfn, set1, set2),
				     set1, set2, pred, phiblock, seen);
	    if (newfn == NULL)
	      return NULL;
	    if (newfn != oldfn)
	      {
		newexpr = temp_copy_call_expr (expr);
		CALL_EXPR_FN (newexpr) = get_value_handle (newfn);
	      }
	    if (oldsc)
	      {
		newsc = phi_translate_1 (find_leader_in_sets (oldsc, set1, set2),
					 set1, set2, pred, phiblock, seen);
		if (newsc == NULL)
		  return NULL;
		if (newsc != oldsc)
		  {
		    if (!newexpr)
		      newexpr = temp_copy_call_expr (expr);
		    CALL_EXPR_STATIC_CHAIN (newexpr) = get_value_handle (newsc);
		  }
	      }

	    /* phi translate the argument list piece by piece.  */
	    nargs = call_expr_nargs (expr);
	    for (i = 0; i < nargs; i++)
	      {
		tree oldval = CALL_EXPR_ARG (expr, i);
		tree newval;
		if (oldval)
		  {
		    /* This may seem like a weird place for this
		       check, but it's actually the easiest place to
		       do it.  We can't do it lower on in the
		       recursion because it's valid for pieces of a
		       component ref to be of AGGREGATE_TYPE, as long
		       as the outermost one is not.
		       To avoid *that* case, we have a check for
		       AGGREGATE_TYPE_P in insert_aux.  However, that
		       check will *not* catch this case because here
		       it occurs in the argument list.  */
		    if (AGGREGATE_TYPE_P (TREE_TYPE (oldval)))
		      return NULL;
		    oldval = find_leader_in_sets (oldval, set1, set2);
		    newval = phi_translate_1 (oldval, set1, set2, pred,
					    phiblock, seen);
		    if (newval == NULL)
		      return NULL;
		    if (newval != oldval)
		      {
			invariantarg |= is_gimple_min_invariant (newval);
			if (!newexpr)
			  newexpr = temp_copy_call_expr (expr);
			CALL_EXPR_ARG (newexpr, i) = get_value_handle (newval);
		      }
		  }
	      }

	    /* In case of new invariant args we might try to fold the call
	       again.  */
	    if (invariantarg && !newsc)
	      {
		tree tmp1 = build_call_array (TREE_TYPE (expr),
					      newfn, call_expr_nargs (newexpr),
					      CALL_EXPR_ARGP (newexpr));
		tree tmp2 = fold (tmp1);
		if (tmp2 != tmp1)
		  {
		    STRIP_TYPE_NOPS (tmp2);
		    if (is_gimple_min_invariant (tmp2))
		      return tmp2;
		  }
	      }

	    tvuses = translate_vuses_through_block (vuses, phiblock, pred);
	    if (vuses != tvuses && ! newexpr)
	      newexpr = temp_copy_call_expr (expr);

	    if (newexpr)
	      {
		newexpr->base.ann = NULL;
		vn_lookup_or_add_with_vuses (newexpr, tvuses);
		expr = newexpr;
		set_expression_vuses (newexpr, tvuses);
	      }
	    phi_trans_add (oldexpr, expr, pred, tvuses);
	  }
      }
      return expr;

    case tcc_declaration:
      {
	VEC (tree, gc) * oldvuses = NULL;
	VEC (tree, gc) * newvuses = NULL;

	oldvuses = get_expression_vuses (expr);
	if (oldvuses)
	  newvuses = translate_vuses_through_block (oldvuses, phiblock,
						    pred);

	if (oldvuses != newvuses)
	  {
	    vn_lookup_or_add_with_vuses (expr, newvuses);
	    set_expression_vuses (expr, newvuses);
	  }
	phi_trans_add (oldexpr, expr, pred, newvuses);
      }
      return expr;

    case tcc_reference:
      {
	tree oldop0 = TREE_OPERAND (expr, 0);
	tree oldop1 = NULL;
	tree newop0;
	tree newop1 = NULL;
	tree oldop2 = NULL;
	tree newop2 = NULL;
	tree oldop3 = NULL;
	tree newop3 = NULL;
	tree newexpr;
	VEC (tree, gc) * oldvuses = NULL;
	VEC (tree, gc) * newvuses = NULL;

	if (TREE_CODE (expr) != INDIRECT_REF
	    && TREE_CODE (expr) != COMPONENT_REF
	    && TREE_CODE (expr) != ARRAY_REF)
	  return NULL;

	oldop0 = find_leader_in_sets (oldop0, set1, set2);
	newop0 = phi_translate_1 (oldop0, set1, set2, pred, phiblock, seen);
	if (newop0 == NULL)
	  return NULL;

	if (TREE_CODE (expr) == ARRAY_REF)
	  {
	    oldop1 = TREE_OPERAND (expr, 1);
	    oldop1 = find_leader_in_sets (oldop1, set1, set2);
	    newop1 = phi_translate_1 (oldop1, set1, set2, pred, phiblock, seen);

	    if (newop1 == NULL)
	      return NULL;

	    oldop2 = TREE_OPERAND (expr, 2);
	    if (oldop2)
	      {
		oldop2 = find_leader_in_sets (oldop2, set1, set2);
		newop2 = phi_translate_1 (oldop2, set1, set2, pred, phiblock, seen);

		if (newop2 == NULL)
		  return NULL;
	      }
	    oldop3 = TREE_OPERAND (expr, 3);
	    if (oldop3)
	      {
		oldop3 = find_leader_in_sets (oldop3, set1, set2);
		newop3 = phi_translate_1 (oldop3, set1, set2, pred, phiblock, seen);

		if (newop3 == NULL)
		  return NULL;
	      }
	  }

	oldvuses = get_expression_vuses (expr);
	if (oldvuses)
	  newvuses = translate_vuses_through_block (oldvuses, phiblock,
						    pred);

	if (newop0 != oldop0 || newvuses != oldvuses
	    || newop1 != oldop1
	    || newop2 != oldop2
	    || newop3 != oldop3)
	  {
	    tree t;

	    newexpr = (tree) pool_alloc (reference_node_pool);
	    memcpy (newexpr, expr, tree_size (expr));
	    TREE_OPERAND (newexpr, 0) = get_value_handle (newop0);
	    if (TREE_CODE (expr) == ARRAY_REF)
	      {
		TREE_OPERAND (newexpr, 1) = get_value_handle (newop1);
		if (newop2)
		  TREE_OPERAND (newexpr, 2) = get_value_handle (newop2);
		if (newop3)
		  TREE_OPERAND (newexpr, 3) = get_value_handle (newop3);
	      }

	    t = fully_constant_expression (newexpr);

	    if (t != newexpr)
	      {
		pool_free (reference_node_pool, newexpr);
		newexpr = t;
	      }
	    else
	      {
		newexpr->base.ann = NULL;
		vn_lookup_or_add_with_vuses (newexpr, newvuses);
		set_expression_vuses (newexpr, newvuses);
	      }
	    expr = newexpr;
	  }
	phi_trans_add (oldexpr, expr, pred, newvuses);
      }
      return expr;
      break;

    case tcc_binary:
    case tcc_comparison:
      {
	tree oldop1 = TREE_OPERAND (expr, 0);
	tree oldval1 = oldop1;
	tree oldop2 = TREE_OPERAND (expr, 1);
	tree oldval2 = oldop2;
	tree newop1;
	tree newop2;
	tree newexpr;

	oldop1 = find_leader_in_sets (oldop1, set1, set2);
	newop1 = phi_translate_1 (oldop1, set1, set2, pred, phiblock, seen);
	if (newop1 == NULL)
	  return NULL;

	oldop2 = find_leader_in_sets (oldop2, set1, set2);
	newop2 = phi_translate_1 (oldop2, set1, set2, pred, phiblock, seen);
	if (newop2 == NULL)
	  return NULL;
	if (newop1 != oldop1 || newop2 != oldop2)
	  {
	    tree t;
	    newexpr = (tree) pool_alloc (binary_node_pool);
	    memcpy (newexpr, expr, tree_size (expr));
	    TREE_OPERAND (newexpr, 0) = newop1 == oldop1 ? oldval1 : get_value_handle (newop1);
	    TREE_OPERAND (newexpr, 1) = newop2 == oldop2 ? oldval2 : get_value_handle (newop2);
	    t = fully_constant_expression (newexpr);
	    if (t != newexpr)
	      {
		pool_free (binary_node_pool, newexpr);
		newexpr = t;
	      }
	    else
	      {
		newexpr->base.ann = NULL;
		vn_lookup_or_add (newexpr);
	      }
	    expr = newexpr;
	  }
	phi_trans_add (oldexpr, expr, pred, NULL);
      }
      return expr;

    case tcc_unary:
      {
	tree oldop1 = TREE_OPERAND (expr, 0);
	tree newop1;
	tree newexpr;

	oldop1 = find_leader_in_sets (oldop1, set1, set2);
	newop1 = phi_translate_1 (oldop1, set1, set2, pred, phiblock, seen);
	if (newop1 == NULL)
	  return NULL;
	if (newop1 != oldop1)
	  {
	    tree t;
	    newexpr = (tree) pool_alloc (unary_node_pool);
	    memcpy (newexpr, expr, tree_size (expr));
	    TREE_OPERAND (newexpr, 0) = get_value_handle (newop1);
	    t = fully_constant_expression (newexpr);
	    if (t != newexpr)
	      {
		pool_free (unary_node_pool, newexpr);
		newexpr = t;
	      }
	    else
	      {
		newexpr->base.ann = NULL;
		vn_lookup_or_add (newexpr);
	      }
	    expr = newexpr;
	  }
	phi_trans_add (oldexpr, expr, pred, NULL);
      }
      return expr;

    case tcc_exceptional:
      {
	tree phi = NULL;
	edge e;
	tree def_stmt;
	gcc_assert (TREE_CODE (expr) == SSA_NAME);

	def_stmt = SSA_NAME_DEF_STMT (expr);
	if (TREE_CODE (def_stmt) == PHI_NODE
	    && bb_for_stmt (def_stmt) == phiblock)
	  phi = def_stmt;
	else
	  return expr;

	e = find_edge (pred, bb_for_stmt (phi));
	if (e)
	  {
	    tree val;
	    tree def = PHI_ARG_DEF (phi, e->dest_idx);

	    if (is_gimple_min_invariant (def))
	      return def;

	    if (TREE_CODE (def) == SSA_NAME && ssa_undefined_value_p (def))
	      return NULL;

	    val = get_value_handle (def);
	    gcc_assert (val);
	    return def;
	  }
      }
      return expr;

    default:
      gcc_unreachable ();
    }
}

/* Translate EXPR using phis in PHIBLOCK, so that it has the values of
   the phis in PRED.
   Return NULL if we can't find a leader for each part of the
   translated expression.  */

static tree
phi_translate (tree expr, bitmap_set_t set1, bitmap_set_t set2,
	       basic_block pred, basic_block phiblock)
{
  bitmap_clear (seen_during_translate);
  return phi_translate_1 (expr, set1, set2, pred, phiblock,
			  seen_during_translate);
}

/* For each expression in SET, translate the value handles through phi nodes
   in PHIBLOCK using edge PHIBLOCK->PRED, and store the resulting
   expressions in DEST.  */

static void
phi_translate_set (bitmap_set_t dest, bitmap_set_t set, basic_block pred,
		   basic_block phiblock)
{
  VEC (tree, heap) *exprs;
  tree expr;
  int i;

  if (!phi_nodes (phiblock))
    {
      bitmap_set_copy (dest, set);
      return;
    }

  exprs = sorted_array_from_bitmap_set (set);
  for (i = 0; VEC_iterate (tree, exprs, i, expr); i++)
    {
      tree translated;
      translated = phi_translate (expr, set, NULL, pred, phiblock);

      /* Don't add constants or empty translations to the cache, since
	 we won't look them up that way, or use the result, anyway.  */
      if (translated && !is_gimple_min_invariant (translated))
	{
	  phi_trans_add (expr, translated, pred,
			 get_expression_vuses (translated));
	}

      if (translated != NULL)
	bitmap_value_insert_into_set (dest, translated);
    }
  VEC_free (tree, heap, exprs);
}

/* Find the leader for a value (i.e., the name representing that
   value) in a given set, and return it.  Return NULL if no leader is
   found.  */

static tree
bitmap_find_leader (bitmap_set_t set, tree val)
{
  if (val == NULL)
    return NULL;

  if (constant_expr_p (val))
    return val;

  if (bitmap_set_contains_value (set, val))
    {
      /* Rather than walk the entire bitmap of expressions, and see
	 whether any of them has the value we are looking for, we look
	 at the reverse mapping, which tells us the set of expressions
	 that have a given value (IE value->expressions with that
	 value) and see if any of those expressions are in our set.
	 The number of expressions per value is usually significantly
	 less than the number of expressions in the set.  In fact, for
	 large testcases, doing it this way is roughly 5-10x faster
	 than walking the bitmap.
	 If this is somehow a significant lose for some cases, we can
	 choose which set to walk based on which set is smaller.  */
      unsigned int i;
      bitmap_iterator bi;
      bitmap_set_t exprset = VALUE_HANDLE_EXPR_SET (val);

      EXECUTE_IF_AND_IN_BITMAP (exprset->expressions,
				set->expressions, 0, i, bi)
	return expression_for_id (i);
    }
  return NULL;
}

/* Determine if EXPR, a memory expression, is ANTIC_IN at the top of
   BLOCK by seeing if it is not killed in the block.  Note that we are
   only determining whether there is a store that kills it.  Because
   of the order in which clean iterates over values, we are guaranteed
   that altered operands will have caused us to be eliminated from the
   ANTIC_IN set already.  */

static bool
value_dies_in_block_x (tree expr, basic_block block)
{
  int i;
  tree vuse;
  VEC (tree, gc) *vuses = get_expression_vuses (expr);

  /* Conservatively, a value dies if it's vuses are defined in this
     block, unless they come from phi nodes (which are merge operations,
     rather than stores.  */
  for (i = 0; VEC_iterate (tree, vuses, i, vuse); i++)
    {
      tree def = SSA_NAME_DEF_STMT (vuse);

      if (bb_for_stmt (def) != block)
	continue;
      if (TREE_CODE (def) == PHI_NODE)
	continue;
      return true;
    }
  return false;
}

/* Determine if the expression EXPR is valid in SET1 U SET2.
   ONLY SET2 CAN BE NULL.
   This means that we have a leader for each part of the expression
   (if it consists of values), or the expression is an SSA_NAME.
   For loads/calls, we also see if the vuses are killed in this block.

   NB: We never should run into a case where we have SSA_NAME +
   SSA_NAME or SSA_NAME + value.  The sets valid_in_sets is called on,
   the ANTIC sets, will only ever have SSA_NAME's or value expressions
   (IE VALUE1 + VALUE2, *VALUE1, VALUE1 < VALUE2)  */

#define union_contains_value(SET1, SET2, VAL)			\
  (bitmap_set_contains_value ((SET1), (VAL))			\
   || ((SET2) && bitmap_set_contains_value ((SET2), (VAL))))

static bool
valid_in_sets (bitmap_set_t set1, bitmap_set_t set2, tree expr,
	       basic_block block)
{
 switch (TREE_CODE_CLASS (TREE_CODE (expr)))
    {
    case tcc_binary:
    case tcc_comparison:
      {
	tree op1 = TREE_OPERAND (expr, 0);
	tree op2 = TREE_OPERAND (expr, 1);

	return union_contains_value (set1, set2, op1)
	  && union_contains_value (set1, set2, op2);
      }

    case tcc_unary:
      {
	tree op1 = TREE_OPERAND (expr, 0);
	return union_contains_value (set1, set2, op1);
      }

    case tcc_expression:
      return false;

    case tcc_vl_exp:
      {
	if (TREE_CODE (expr) == CALL_EXPR)
	  {
	    tree fn = CALL_EXPR_FN (expr);
	    tree sc = CALL_EXPR_STATIC_CHAIN (expr);
	    tree arg;
	    call_expr_arg_iterator iter;

	    /* Check the non-argument operands first.  */
	    if (!union_contains_value (set1, set2, fn)
		|| (sc && !union_contains_value (set1, set2, sc)))
	      return false;

	    /* Now check the operands.  */
	    FOR_EACH_CALL_EXPR_ARG (arg, iter, expr)
	      {
		if (!union_contains_value (set1, set2, arg))
		  return false;
	      }
	    return !value_dies_in_block_x (expr, block);
	  }
	return false;
      }

    case tcc_reference:
      {
	if (TREE_CODE (expr) == INDIRECT_REF
	    || TREE_CODE (expr) == COMPONENT_REF
	    || TREE_CODE (expr) == ARRAY_REF)
	  {
	    tree op0 = TREE_OPERAND (expr, 0);
	    gcc_assert (is_gimple_min_invariant (op0)
			|| TREE_CODE (op0) == VALUE_HANDLE);
	    if (!union_contains_value (set1, set2, op0))
	      return false;
	    if (TREE_CODE (expr) == ARRAY_REF)
	      {
		tree op1 = TREE_OPERAND (expr, 1);
		tree op2 = TREE_OPERAND (expr, 2);
		tree op3 = TREE_OPERAND (expr, 3);
		gcc_assert (is_gimple_min_invariant (op1)
			    || TREE_CODE (op1) == VALUE_HANDLE);
		if (!union_contains_value (set1, set2, op1))
		  return false;
		gcc_assert (!op2 || is_gimple_min_invariant (op2)
			    || TREE_CODE (op2) == VALUE_HANDLE);
		if (op2
		    && !union_contains_value (set1, set2, op2))
		  return false;
		gcc_assert (!op3 || is_gimple_min_invariant (op3)
			    || TREE_CODE (op3) == VALUE_HANDLE);
		if (op3
		    && !union_contains_value (set1, set2, op3))
		  return false;
	    }
	    return !value_dies_in_block_x (expr, block);
	  }
      }
      return false;

    case tcc_exceptional:
      {
	gcc_assert (TREE_CODE (expr) == SSA_NAME);
	return bitmap_set_contains_expr (AVAIL_OUT (block), expr);
      }

    case tcc_declaration:
      return !value_dies_in_block_x (expr, block);

    default:
      /* No other cases should be encountered.  */
      gcc_unreachable ();
   }
}

/* Clean the set of expressions that are no longer valid in SET1 or
   SET2.  This means expressions that are made up of values we have no
   leaders for in SET1 or SET2.  This version is used for partial
   anticipation, which means it is not valid in either ANTIC_IN or
   PA_IN.  */

static void
dependent_clean (bitmap_set_t set1, bitmap_set_t set2, basic_block block)
{
  VEC (tree, heap) *exprs = sorted_array_from_bitmap_set (set1);
  tree expr;
  int i;

  for (i = 0; VEC_iterate (tree, exprs, i, expr); i++)
    {
      if (!valid_in_sets (set1, set2, expr, block))
	bitmap_remove_from_set (set1, expr);
    }
  VEC_free (tree, heap, exprs);
}

/* Clean the set of expressions that are no longer valid in SET.  This
   means expressions that are made up of values we have no leaders for
   in SET.  */

static void
clean (bitmap_set_t set, basic_block block)
{
  VEC (tree, heap) *exprs = sorted_array_from_bitmap_set (set);
  tree expr;
  int i;

  for (i = 0; VEC_iterate (tree, exprs, i, expr); i++)
    {
      if (!valid_in_sets (set, NULL, expr, block))
	bitmap_remove_from_set (set, expr);
    }
  VEC_free (tree, heap, exprs);
}

static sbitmap has_abnormal_preds;

/* List of blocks that may have changed during ANTIC computation and
   thus need to be iterated over.  */

static sbitmap changed_blocks;

/* Decide whether to defer a block for a later iteration, or PHI
   translate SOURCE to DEST using phis in PHIBLOCK.  Return false if we
   should defer the block, and true if we processed it.  */

static bool
defer_or_phi_translate_block (bitmap_set_t dest, bitmap_set_t source,
			      basic_block block, basic_block phiblock)
{
  if (!BB_VISITED (phiblock))
    {
      SET_BIT (changed_blocks, block->index);
      BB_VISITED (block) = 0;
      BB_DEFERRED (block) = 1;
      return false;
    }
  else
    phi_translate_set (dest, source, block, phiblock);
  return true;
}

/* Compute the ANTIC set for BLOCK.

   If succs(BLOCK) > 1 then
     ANTIC_OUT[BLOCK] = intersection of ANTIC_IN[b] for all succ(BLOCK)
   else if succs(BLOCK) == 1 then
     ANTIC_OUT[BLOCK] = phi_translate (ANTIC_IN[succ(BLOCK)])

   ANTIC_IN[BLOCK] = clean(ANTIC_OUT[BLOCK] U EXP_GEN[BLOCK] - TMP_GEN[BLOCK])
*/

static bool
compute_antic_aux (basic_block block, bool block_has_abnormal_pred_edge)
{
  bool changed = false;
  bitmap_set_t S, old, ANTIC_OUT;
  bitmap_iterator bi;
  unsigned int bii;
  edge e;
  edge_iterator ei;

  old = ANTIC_OUT = S = NULL;
  BB_VISITED (block) = 1;

  /* If any edges from predecessors are abnormal, antic_in is empty,
     so do nothing.  */
  if (block_has_abnormal_pred_edge)
    goto maybe_dump_sets;

  old = ANTIC_IN (block);
  ANTIC_OUT = bitmap_set_new ();

  /* If the block has no successors, ANTIC_OUT is empty.  */
  if (EDGE_COUNT (block->succs) == 0)
    ;
  /* If we have one successor, we could have some phi nodes to
     translate through.  */
  else if (single_succ_p (block))
    {
      basic_block succ_bb = single_succ (block);

      /* We trade iterations of the dataflow equations for having to
	 phi translate the maximal set, which is incredibly slow
	 (since the maximal set often has 300+ members, even when you
	 have a small number of blocks).
	 Basically, we defer the computation of ANTIC for this block
	 until we have processed it's successor, which will inevitably
	 have a *much* smaller set of values to phi translate once
	 clean has been run on it.
	 The cost of doing this is that we technically perform more
	 iterations, however, they are lower cost iterations.

	 Timings for PRE on tramp3d-v4:
	 without maximal set fix: 11 seconds
	 with maximal set fix/without deferring: 26 seconds
	 with maximal set fix/with deferring: 11 seconds
     */

      if (!defer_or_phi_translate_block (ANTIC_OUT, ANTIC_IN (succ_bb),
					block, succ_bb))
	{
	  changed = true;
	  goto maybe_dump_sets;
	}
    }
  /* If we have multiple successors, we take the intersection of all of
     them.  Note that in the case of loop exit phi nodes, we may have
     phis to translate through.  */
  else
    {
      VEC(basic_block, heap) * worklist;
      size_t i;
      basic_block bprime, first;

      worklist = VEC_alloc (basic_block, heap, EDGE_COUNT (block->succs));
      FOR_EACH_EDGE (e, ei, block->succs)
	VEC_quick_push (basic_block, worklist, e->dest);
      first = VEC_index (basic_block, worklist, 0);

      if (phi_nodes (first))
	{
	  bitmap_set_t from = ANTIC_IN (first);

	  if (!BB_VISITED (first))
	    from = maximal_set;
	  phi_translate_set (ANTIC_OUT, from, block, first);
	}
      else
	{
	  if (!BB_VISITED (first))
	    bitmap_set_copy (ANTIC_OUT, maximal_set);
	  else
	    bitmap_set_copy (ANTIC_OUT, ANTIC_IN (first));
	}

      for (i = 1; VEC_iterate (basic_block, worklist, i, bprime); i++)
	{
	  if (phi_nodes (bprime))
	    {
	      bitmap_set_t tmp = bitmap_set_new ();
	      bitmap_set_t from = ANTIC_IN (bprime);

	      if (!BB_VISITED (bprime))
		from = maximal_set;
	      phi_translate_set (tmp, from, block, bprime);
	      bitmap_set_and (ANTIC_OUT, tmp);
	      bitmap_set_free (tmp);
	    }
	  else
	    {
	      if (!BB_VISITED (bprime))
		bitmap_set_and (ANTIC_OUT, maximal_set);
	      else
		bitmap_set_and (ANTIC_OUT, ANTIC_IN (bprime));
	    }
	}
      VEC_free (basic_block, heap, worklist);
    }

  /* Generate ANTIC_OUT - TMP_GEN.  */
  S = bitmap_set_subtract (ANTIC_OUT, TMP_GEN (block));

  /* Start ANTIC_IN with EXP_GEN - TMP_GEN.  */
  ANTIC_IN (block) = bitmap_set_subtract (EXP_GEN (block),
					  TMP_GEN (block));

  /* Then union in the ANTIC_OUT - TMP_GEN values,
     to get ANTIC_OUT U EXP_GEN - TMP_GEN */
  FOR_EACH_EXPR_ID_IN_SET (S, bii, bi)
    bitmap_value_insert_into_set (ANTIC_IN (block),
				  expression_for_id (bii));

  clean (ANTIC_IN (block), block);

  /* !old->expressions can happen when we deferred a block.  */
  if (!old->expressions || !bitmap_set_equal (old, ANTIC_IN (block)))
    {
      changed = true;
      SET_BIT (changed_blocks, block->index);
      FOR_EACH_EDGE (e, ei, block->preds)
	SET_BIT (changed_blocks, e->src->index);
    }
  else
    RESET_BIT (changed_blocks, block->index);

 maybe_dump_sets:
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      if (!BB_DEFERRED (block) || BB_VISITED (block))
	{
	  if (ANTIC_OUT)
	    print_bitmap_set (dump_file, ANTIC_OUT, "ANTIC_OUT", block->index);

	  print_bitmap_set (dump_file, ANTIC_IN (block), "ANTIC_IN",
			    block->index);

	  if (S)
	    print_bitmap_set (dump_file, S, "S", block->index);
	}
      else
	{
	  fprintf (dump_file,
		   "Block %d was deferred for a future iteration.\n",
		   block->index);
	}
    }
  if (old)
    bitmap_set_free (old);
  if (S)
    bitmap_set_free (S);
  if (ANTIC_OUT)
    bitmap_set_free (ANTIC_OUT);
  return changed;
}

/* Compute PARTIAL_ANTIC for BLOCK.

   If succs(BLOCK) > 1 then
     PA_OUT[BLOCK] = value wise union of PA_IN[b] + all ANTIC_IN not
     in ANTIC_OUT for all succ(BLOCK)
   else if succs(BLOCK) == 1 then
     PA_OUT[BLOCK] = phi_translate (PA_IN[succ(BLOCK)])

   PA_IN[BLOCK] = dependent_clean(PA_OUT[BLOCK] - TMP_GEN[BLOCK]
				  - ANTIC_IN[BLOCK])

*/
static bool
compute_partial_antic_aux (basic_block block,
			   bool block_has_abnormal_pred_edge)
{
  bool changed = false;
  bitmap_set_t old_PA_IN;
  bitmap_set_t PA_OUT;
  edge e;
  edge_iterator ei;
  unsigned long max_pa = PARAM_VALUE (PARAM_MAX_PARTIAL_ANTIC_LENGTH);

  old_PA_IN = PA_OUT = NULL;

  /* If any edges from predecessors are abnormal, antic_in is empty,
     so do nothing.  */
  if (block_has_abnormal_pred_edge)
    goto maybe_dump_sets;

  /* If there are too many partially anticipatable values in the
     block, phi_translate_set can take an exponential time: stop
     before the translation starts.  */
  if (max_pa
      && single_succ_p (block)
      && bitmap_count_bits (PA_IN (single_succ (block))->values) > max_pa)
    goto maybe_dump_sets;

  old_PA_IN = PA_IN (block);
  PA_OUT = bitmap_set_new ();

  /* If the block has no successors, ANTIC_OUT is empty.  */
  if (EDGE_COUNT (block->succs) == 0)
    ;
  /* If we have one successor, we could have some phi nodes to
     translate through.  Note that we can't phi translate across DFS
     back edges in partial antic, because it uses a union operation on
     the successors.  For recurrences like IV's, we will end up
     generating a new value in the set on each go around (i + 3 (VH.1)
     VH.1 + 1 (VH.2), VH.2 + 1 (VH.3), etc), forever.  */
  else if (single_succ_p (block))
    {
      basic_block succ = single_succ (block);
      if (!(single_succ_edge (block)->flags & EDGE_DFS_BACK))
	phi_translate_set (PA_OUT, PA_IN (succ), block, succ);
    }
  /* If we have multiple successors, we take the union of all of
     them.  */
  else
    {
      VEC(basic_block, heap) * worklist;
      size_t i;
      basic_block bprime;

      worklist = VEC_alloc (basic_block, heap, EDGE_COUNT (block->succs));
      FOR_EACH_EDGE (e, ei, block->succs)
	{
	  if (e->flags & EDGE_DFS_BACK)
	    continue;
	  VEC_quick_push (basic_block, worklist, e->dest);
	}
      if (VEC_length (basic_block, worklist) > 0)
	{
	  for (i = 0; VEC_iterate (basic_block, worklist, i, bprime); i++)
	    {
	      unsigned int i;
	      bitmap_iterator bi;

	      FOR_EACH_EXPR_ID_IN_SET (ANTIC_IN (bprime), i, bi)
		bitmap_value_insert_into_set (PA_OUT,
					      expression_for_id (i));
	      if (phi_nodes (bprime))
		{
		  bitmap_set_t pa_in = bitmap_set_new ();
		  phi_translate_set (pa_in, PA_IN (bprime), block, bprime);
		  FOR_EACH_EXPR_ID_IN_SET (pa_in, i, bi)
		    bitmap_value_insert_into_set (PA_OUT,
						  expression_for_id (i));
		  bitmap_set_free (pa_in);
		}
	      else
		FOR_EACH_EXPR_ID_IN_SET (PA_IN (bprime), i, bi)
		  bitmap_value_insert_into_set (PA_OUT,
						expression_for_id (i));
	    }
	}
      VEC_free (basic_block, heap, worklist);
    }

  /* PA_IN starts with PA_OUT - TMP_GEN.
     Then we subtract things from ANTIC_IN.  */
  PA_IN (block) = bitmap_set_subtract (PA_OUT, TMP_GEN (block));

  /* For partial antic, we want to put back in the phi results, since
     we will properly avoid making them partially antic over backedges.  */
  bitmap_ior_into (PA_IN (block)->values, PHI_GEN (block)->values);
  bitmap_ior_into (PA_IN (block)->expressions, PHI_GEN (block)->expressions);

  /* PA_IN[block] = PA_IN[block] - ANTIC_IN[block] */
  bitmap_set_subtract_values (PA_IN (block), ANTIC_IN (block));

  dependent_clean (PA_IN (block), ANTIC_IN (block), block);

  if (!bitmap_set_equal (old_PA_IN, PA_IN (block)))
    {
      changed = true;
      SET_BIT (changed_blocks, block->index);
      FOR_EACH_EDGE (e, ei, block->preds)
	SET_BIT (changed_blocks, e->src->index);
    }
  else
    RESET_BIT (changed_blocks, block->index);

 maybe_dump_sets:
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      if (PA_OUT)
	print_bitmap_set (dump_file, PA_OUT, "PA_OUT", block->index);

      print_bitmap_set (dump_file, PA_IN (block), "PA_IN", block->index);
    }
  if (old_PA_IN)
    bitmap_set_free (old_PA_IN);
  if (PA_OUT)
    bitmap_set_free (PA_OUT);
  return changed;
}

/* Compute ANTIC and partial ANTIC sets.  */

static void
compute_antic (void)
{
  bool changed = true;
  int num_iterations = 0;
  basic_block block;
  int i;

  /* If any predecessor edges are abnormal, we punt, so antic_in is empty.
     We pre-build the map of blocks with incoming abnormal edges here.  */
  has_abnormal_preds = sbitmap_alloc (last_basic_block);
  sbitmap_zero (has_abnormal_preds);

  FOR_EACH_BB (block)
    {
      edge_iterator ei;
      edge e;

      FOR_EACH_EDGE (e, ei, block->preds)
	{
	  e->flags &= ~EDGE_DFS_BACK;
	  if (e->flags & EDGE_ABNORMAL)
	    {
	      SET_BIT (has_abnormal_preds, block->index);
	      break;
	    }
	}

      BB_VISITED (block) = 0;
      BB_DEFERRED (block) = 0;
      /* While we are here, give empty ANTIC_IN sets to each block.  */
      ANTIC_IN (block) = bitmap_set_new ();
      PA_IN (block) = bitmap_set_new ();
    }

  /* At the exit block we anticipate nothing.  */
  ANTIC_IN (EXIT_BLOCK_PTR) = bitmap_set_new ();
  BB_VISITED (EXIT_BLOCK_PTR) = 1;
  PA_IN (EXIT_BLOCK_PTR) = bitmap_set_new ();

  changed_blocks = sbitmap_alloc (last_basic_block + 1);
  sbitmap_ones (changed_blocks);
  while (changed)
    {
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "Starting iteration %d\n", num_iterations);
      num_iterations++;
      changed = false;
      for (i = 0; i < last_basic_block - NUM_FIXED_BLOCKS; i++)
	{
	  if (TEST_BIT (changed_blocks, postorder[i]))
	    {
	      basic_block block = BASIC_BLOCK (postorder[i]);
	      changed |= compute_antic_aux (block,
					    TEST_BIT (has_abnormal_preds,
						      block->index));
	    }
	}
#ifdef ENABLE_CHECKING
      /* Theoretically possible, but *highly* unlikely.  */
      gcc_assert (num_iterations < 500);
#endif
    }

  if (dump_file && (dump_flags & TDF_STATS))
    fprintf (dump_file, "compute_antic required %d iterations\n",
	     num_iterations);

  if (do_partial_partial)
    {
      sbitmap_ones (changed_blocks);
      mark_dfs_back_edges ();
      num_iterations = 0;
      changed = true;
      while (changed)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Starting iteration %d\n", num_iterations);
	  num_iterations++;
	  changed = false;
	  for (i = 0; i < last_basic_block - NUM_FIXED_BLOCKS; i++)
	    {
	      if (TEST_BIT (changed_blocks, postorder[i]))
		{
		  basic_block block = BASIC_BLOCK (postorder[i]);
		  changed
		    |= compute_partial_antic_aux (block,
						  TEST_BIT (has_abnormal_preds,
							    block->index));
		}
	    }
#ifdef ENABLE_CHECKING
	  /* Theoretically possible, but *highly* unlikely.  */
	  gcc_assert (num_iterations < 500);
#endif
	}
      if (dump_file && (dump_flags & TDF_STATS))
	fprintf (dump_file, "compute_partial_antic required %d iterations\n",
		 num_iterations);
    }
  sbitmap_free (has_abnormal_preds);
  sbitmap_free (changed_blocks);
}

/* Return true if we can value number the call in STMT.  This is true
   if we have a pure or constant call.  */

static bool
can_value_number_call (tree stmt)
{
  tree call = get_call_expr_in (stmt);

  if (call_expr_flags (call)  & (ECF_PURE | ECF_CONST))
    return true;
  return false;
}

/* Return true if OP is an exception handler related operation, such as
   FILTER_EXPR or EXC_PTR_EXPR.  */

static bool
is_exception_related (tree op)
{
  return TREE_CODE (op) == FILTER_EXPR || TREE_CODE (op) == EXC_PTR_EXPR;
}

/* Return true if OP is a tree which we can perform value numbering
   on.  */

static bool
can_value_number_operation (tree op)
{
  return (UNARY_CLASS_P (op)
	  && !is_exception_related (TREE_OPERAND (op, 0)))
    || BINARY_CLASS_P (op)
    || COMPARISON_CLASS_P (op)
    || REFERENCE_CLASS_P (op)
    || (TREE_CODE (op) == CALL_EXPR
	&& can_value_number_call (op));
}


/* Return true if OP is a tree which we can perform PRE on
   on.  This may not match the operations we can value number, but in
   a perfect world would.  */

static bool
can_PRE_operation (tree op)
{
  return UNARY_CLASS_P (op)
    || BINARY_CLASS_P (op)
    || COMPARISON_CLASS_P (op)
    || TREE_CODE (op) == INDIRECT_REF
    || TREE_CODE (op) == COMPONENT_REF
    || TREE_CODE (op) == CALL_EXPR
    || TREE_CODE (op) == ARRAY_REF;
}


/* Inserted expressions are placed onto this worklist, which is used
   for performing quick dead code elimination of insertions we made
   that didn't turn out to be necessary.   */
static VEC(tree,heap) *inserted_exprs;

/* Pool allocated fake store expressions are placed onto this
   worklist, which, after performing dead code elimination, is walked
   to see which expressions need to be put into GC'able memory  */
static VEC(tree, heap) *need_creation;

/* For COMPONENT_REF's and ARRAY_REF's, we can't have any intermediates for the
   COMPONENT_REF or INDIRECT_REF or ARRAY_REF portion, because we'd end up with
   trying to rename aggregates into ssa form directly, which is a no
   no.

   Thus, this routine doesn't create temporaries, it just builds a
   single access expression for the array, calling
   find_or_generate_expression to build the innermost pieces.

   This function is a subroutine of create_expression_by_pieces, and
   should not be called on it's own unless you really know what you
   are doing.
*/
static tree
create_component_ref_by_pieces (basic_block block, tree expr, tree stmts)
{
  tree genop = expr;
  tree folded;

  if (TREE_CODE (genop) == VALUE_HANDLE)
    {
      tree found = bitmap_find_leader (AVAIL_OUT (block), expr);
      if (found)
	return found;
    }

  if (TREE_CODE (genop) == VALUE_HANDLE)
    {
      bitmap_set_t exprset = VALUE_HANDLE_EXPR_SET (expr);
      unsigned int firstbit = bitmap_first_set_bit (exprset->expressions);
      genop = expression_for_id (firstbit);
    }

  switch TREE_CODE (genop)
    {
    case ARRAY_REF:
      {
	tree op0;
	tree op1, op2, op3;
	op0 = create_component_ref_by_pieces (block,
					      TREE_OPERAND (genop, 0),
					      stmts);
	op1 = TREE_OPERAND (genop, 1);
	if (TREE_CODE (op1) == VALUE_HANDLE)
	  op1 = find_or_generate_expression (block, op1, stmts);
	op2 = TREE_OPERAND (genop, 2);
	if (op2 && TREE_CODE (op2) == VALUE_HANDLE)
	  op2 = find_or_generate_expression (block, op2, stmts);
	op3 = TREE_OPERAND (genop, 3);
	if (op3 && TREE_CODE (op3) == VALUE_HANDLE)
	  op3 = find_or_generate_expression (block, op3, stmts);
	folded = build4 (ARRAY_REF, TREE_TYPE (genop), op0, op1,
			      op2, op3);
	return folded;
      }
    case COMPONENT_REF:
      {
	tree op0;
	tree op1, op2;
	op0 = create_component_ref_by_pieces (block,
					      TREE_OPERAND (genop, 0),
					      stmts);
	/* op1 should be a FIELD_DECL, which are represented by
	   themselves.  */
	op1 = TREE_OPERAND (genop, 1);
	op2 = TREE_OPERAND (genop, 2);
	if (op2 && TREE_CODE (op2) == VALUE_HANDLE)
	  op2 = find_or_generate_expression (block, op2, stmts);
	folded = fold_build3 (COMPONENT_REF, TREE_TYPE (genop), op0, op1,
			      op2);
	return folded;
      }
      break;
    case INDIRECT_REF:
      {
	tree op1 = TREE_OPERAND (genop, 0);
	tree genop1 = find_or_generate_expression (block, op1, stmts);

	folded = fold_build1 (TREE_CODE (genop), TREE_TYPE (genop),
			      genop1);
	return folded;
      }
      break;
    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case SSA_NAME:
    case STRING_CST:
      return genop;
    default:
      gcc_unreachable ();
    }

  return NULL_TREE;
}

/* Find a leader for an expression, or generate one using
   create_expression_by_pieces if it's ANTIC but
   complex.
   BLOCK is the basic_block we are looking for leaders in.
   EXPR is the expression to find a leader or generate for.
   STMTS is the statement list to put the inserted expressions on.
   Returns the SSA_NAME of the LHS of the generated expression or the
   leader.  */

static tree
find_or_generate_expression (basic_block block, tree expr, tree stmts)
{
  tree genop = bitmap_find_leader (AVAIL_OUT (block), expr);

  /* If it's still NULL, it must be a complex expression, so generate
     it recursively.  */
  if (genop == NULL)
    {
      bitmap_set_t exprset = VALUE_HANDLE_EXPR_SET (expr);
      bool handled = false;
      bitmap_iterator bi;
      unsigned int i;

      /* We will hit cases where we have SSA_NAME's in exprset before
	 other operations, because we may have come up with the SCCVN
	 value before getting to the RHS of the expression.  */
      FOR_EACH_EXPR_ID_IN_SET (exprset, i, bi)
	{
	  genop = expression_for_id (i);
	  if (can_PRE_operation (genop))
	    {
	      handled = true;
	      genop = create_expression_by_pieces (block, genop, stmts);
	      break;
	    }
	}
      gcc_assert (handled);
    }
  return genop;
}

#define NECESSARY(stmt)		stmt->base.asm_written_flag
/* Create an expression in pieces, so that we can handle very complex
   expressions that may be ANTIC, but not necessary GIMPLE.
   BLOCK is the basic block the expression will be inserted into,
   EXPR is the expression to insert (in value form)
   STMTS is a statement list to append the necessary insertions into.

   This function will die if we hit some value that shouldn't be
   ANTIC but is (IE there is no leader for it, or its components).
   This function may also generate expressions that are themselves
   partially or fully redundant.  Those that are will be either made
   fully redundant during the next iteration of insert (for partially
   redundant ones), or eliminated by eliminate (for fully redundant
   ones).  */

static tree
create_expression_by_pieces (basic_block block, tree expr, tree stmts)
{
  tree temp, name;
  tree folded, forced_stmts, newexpr;
  tree v;
  tree_stmt_iterator tsi;

  switch (TREE_CODE_CLASS (TREE_CODE (expr)))
    {
    case tcc_vl_exp:
      {
	tree fn, sc;
	tree genfn;
	int i, nargs;
	tree *buffer;

	gcc_assert (TREE_CODE (expr) == CALL_EXPR);

	fn = CALL_EXPR_FN (expr);
	sc = CALL_EXPR_STATIC_CHAIN (expr);

	genfn = find_or_generate_expression (block, fn, stmts);

	nargs = call_expr_nargs (expr);
	buffer = (tree*) alloca (nargs * sizeof (tree));

	for (i = 0; i < nargs; i++)
	  {
	    tree arg = CALL_EXPR_ARG (expr, i);
	    buffer[i] = find_or_generate_expression (block, arg, stmts);
	  }

	folded = build_call_array (TREE_TYPE (expr), genfn, nargs, buffer);
	if (sc)
	  CALL_EXPR_STATIC_CHAIN (folded) =
	    find_or_generate_expression (block, sc, stmts);
	folded = fold (folded);
	break;
      }
      break;
    case tcc_reference:
      {
	if (TREE_CODE (expr) == COMPONENT_REF
	    || TREE_CODE (expr) == ARRAY_REF)
	  {
	    folded = create_component_ref_by_pieces (block, expr, stmts);
	  }
	else
	  {
	    tree op1 = TREE_OPERAND (expr, 0);
	    tree genop1 = find_or_generate_expression (block, op1, stmts);

	    folded = fold_build1 (TREE_CODE (expr), TREE_TYPE (expr),
				  genop1);
	  }
	break;
      }

    case tcc_binary:
    case tcc_comparison:
      {
	tree op1 = TREE_OPERAND (expr, 0);
	tree op2 = TREE_OPERAND (expr, 1);
	tree genop1 = find_or_generate_expression (block, op1, stmts);
	tree genop2 = find_or_generate_expression (block, op2, stmts);
	folded = fold_build2 (TREE_CODE (expr), TREE_TYPE (expr),
			      genop1, genop2);
	break;
      }

    case tcc_unary:
      {
	tree op1 = TREE_OPERAND (expr, 0);
	tree genop1 = find_or_generate_expression (block, op1, stmts);
	folded = fold_build1 (TREE_CODE (expr), TREE_TYPE (expr),
			      genop1);
	break;
      }

    default:
      gcc_unreachable ();
    }

  /* Force the generated expression to be a sequence of GIMPLE
     statements.
     We have to call unshare_expr because force_gimple_operand may
     modify the tree we pass to it.  */
  newexpr = force_gimple_operand (unshare_expr (folded), &forced_stmts,
				  false, NULL);

  /* If we have any intermediate expressions to the value sets, add them
     to the value sets and chain them on in the instruction stream.  */
  if (forced_stmts)
    {
      tsi = tsi_start (forced_stmts);
      for (; !tsi_end_p (tsi); tsi_next (&tsi))
	{
	  tree stmt = tsi_stmt (tsi);
	  tree forcedname = GIMPLE_STMT_OPERAND (stmt, 0);
	  tree forcedexpr = GIMPLE_STMT_OPERAND (stmt, 1);
	  tree val = vn_lookup_or_add (forcedexpr);

	  VEC_safe_push (tree, heap, inserted_exprs, stmt);
	  VN_INFO_GET (forcedname)->valnum = forcedname;
	  vn_add (forcedname, val);
	  bitmap_value_replace_in_set (NEW_SETS (block), forcedname);
	  bitmap_value_replace_in_set (AVAIL_OUT (block), forcedname);
	  mark_symbols_for_renaming (stmt);
	}
      tsi = tsi_last (stmts);
      tsi_link_after (&tsi, forced_stmts, TSI_CONTINUE_LINKING);
    }

  /* Build and insert the assignment of the end result to the temporary
     that we will return.  */
  if (!pretemp || TREE_TYPE (expr) != TREE_TYPE (pretemp))
    {
      pretemp = create_tmp_var (TREE_TYPE (expr), "pretmp");
      get_var_ann (pretemp);
    }

  temp = pretemp;
  add_referenced_var (temp);

  if (TREE_CODE (TREE_TYPE (expr)) == COMPLEX_TYPE
      || TREE_CODE (TREE_TYPE (expr)) == VECTOR_TYPE)
    DECL_GIMPLE_REG_P (temp) = 1;

  newexpr = build_gimple_modify_stmt (temp, newexpr);
  name = make_ssa_name (temp, newexpr);
  GIMPLE_STMT_OPERAND (newexpr, 0) = name;
  NECESSARY (newexpr) = 0;

  tsi = tsi_last (stmts);
  tsi_link_after (&tsi, newexpr, TSI_CONTINUE_LINKING);
  VEC_safe_push (tree, heap, inserted_exprs, newexpr);

  /* All the symbols in NEWEXPR should be put into SSA form.  */
  mark_symbols_for_renaming (newexpr);

  /* Add a value handle to the temporary.
     The value may already exist in either NEW_SETS, or AVAIL_OUT, because
     we are creating the expression by pieces, and this particular piece of
     the expression may have been represented.  There is no harm in replacing
     here.  */
  v = get_value_handle (expr);
  vn_add (name, v);
  VN_INFO_GET (name)->valnum = name;
  get_or_alloc_expression_id (name);
  bitmap_value_replace_in_set (NEW_SETS (block), name);
  bitmap_value_replace_in_set (AVAIL_OUT (block), name);

  pre_stats.insertions++;
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Inserted ");
      print_generic_expr (dump_file, newexpr, 0);
      fprintf (dump_file, " in predecessor %d\n", block->index);
    }

  return name;
}

/* Insert the to-be-made-available values of expression EXPRNUM for each
   predecessor, stored in AVAIL, into the predecessors of BLOCK, and
   merge the result with a phi node, given the same value handle as
   NODE.  Return true if we have inserted new stuff.  */

static bool
insert_into_preds_of_block (basic_block block, unsigned int exprnum,
			    tree *avail)
{
  tree expr = expression_for_id (exprnum);
  tree val = get_value_handle (expr);
  edge pred;
  bool insertions = false;
  bool nophi = false;
  basic_block bprime;
  tree eprime;
  edge_iterator ei;
  tree type = TREE_TYPE (avail[EDGE_PRED (block, 0)->src->index]);
  tree temp;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Found partial redundancy for expression ");
      print_generic_expr (dump_file, expr, 0);
      fprintf (dump_file, " (");
      print_generic_expr (dump_file, val, 0);
      fprintf (dump_file, ")");
      fprintf (dump_file, "\n");
    }

  /* Make sure we aren't creating an induction variable.  */
  if (block->loop_depth > 0 && EDGE_COUNT (block->preds) == 2
      && TREE_CODE_CLASS (TREE_CODE (expr)) != tcc_reference )
    {
      bool firstinsideloop = false;
      bool secondinsideloop = false;
      firstinsideloop = flow_bb_inside_loop_p (block->loop_father,
					       EDGE_PRED (block, 0)->src);
      secondinsideloop = flow_bb_inside_loop_p (block->loop_father,
						EDGE_PRED (block, 1)->src);
      /* Induction variables only have one edge inside the loop.  */
      if (firstinsideloop ^ secondinsideloop)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Skipping insertion of phi for partial redundancy: Looks like an induction variable\n");
	  nophi = true;
	}
    }


  /* Make the necessary insertions.  */
  FOR_EACH_EDGE (pred, ei, block->preds)
    {
      tree stmts = alloc_stmt_list ();
      tree builtexpr;
      bprime = pred->src;
      eprime = avail[bprime->index];

      if (can_PRE_operation (eprime))
	{
	  builtexpr = create_expression_by_pieces (bprime,
						   eprime,
						   stmts);
	  gcc_assert (!(pred->flags & EDGE_ABNORMAL));
	  bsi_insert_on_edge (pred, stmts);
	  avail[bprime->index] = builtexpr;
	  insertions = true;
	}
    }
  /* If we didn't want a phi node, and we made insertions, we still have
     inserted new stuff, and thus return true.  If we didn't want a phi node,
     and didn't make insertions, we haven't added anything new, so return
     false.  */
  if (nophi && insertions)
    return true;
  else if (nophi && !insertions)
    return false;

  /* Now build a phi for the new variable.  */
  if (!prephitemp || TREE_TYPE (prephitemp) != type)
    {
      prephitemp = create_tmp_var (type, "prephitmp");
      get_var_ann (prephitemp);
    }

  temp = prephitemp;
  add_referenced_var (temp);


  if (TREE_CODE (type) == COMPLEX_TYPE
      || TREE_CODE (type) == VECTOR_TYPE)
    DECL_GIMPLE_REG_P (temp) = 1;
  temp = create_phi_node (temp, block);

  NECESSARY (temp) = 0;
  VN_INFO_GET (PHI_RESULT (temp))->valnum = PHI_RESULT (temp);

  VEC_safe_push (tree, heap, inserted_exprs, temp);
  FOR_EACH_EDGE (pred, ei, block->preds)
    add_phi_arg (temp, avail[pred->src->index], pred);

  vn_add (PHI_RESULT (temp), val);

  /* The value should *not* exist in PHI_GEN, or else we wouldn't be doing
     this insertion, since we test for the existence of this value in PHI_GEN
     before proceeding with the partial redundancy checks in insert_aux.

     The value may exist in AVAIL_OUT, in particular, it could be represented
     by the expression we are trying to eliminate, in which case we want the
     replacement to occur.  If it's not existing in AVAIL_OUT, we want it
     inserted there.

     Similarly, to the PHI_GEN case, the value should not exist in NEW_SETS of
     this block, because if it did, it would have existed in our dominator's
     AVAIL_OUT, and would have been skipped due to the full redundancy check.
  */

  bitmap_insert_into_set (PHI_GEN (block),
			  PHI_RESULT (temp));
  bitmap_value_replace_in_set (AVAIL_OUT (block),
			       PHI_RESULT (temp));
  bitmap_insert_into_set (NEW_SETS (block),
			  PHI_RESULT (temp));

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "Created phi ");
      print_generic_expr (dump_file, temp, 0);
      fprintf (dump_file, " in block %d\n", block->index);
    }
  pre_stats.phis++;
  return true;
}



/* Perform insertion of partially redundant values.
   For BLOCK, do the following:
   1.  Propagate the NEW_SETS of the dominator into the current block.
   If the block has multiple predecessors,
       2a. Iterate over the ANTIC expressions for the block to see if
	   any of them are partially redundant.
       2b. If so, insert them into the necessary predecessors to make
	   the expression fully redundant.
       2c. Insert a new PHI merging the values of the predecessors.
       2d. Insert the new PHI, and the new expressions, into the
	   NEW_SETS set.
   3. Recursively call ourselves on the dominator children of BLOCK.

   Steps 1, 2a, and 3 are done by insert_aux. 2b, 2c and 2d are done by
   do_regular_insertion and do_partial_insertion.

*/

static bool
do_regular_insertion (basic_block block, basic_block dom)
{
  bool new_stuff = false;
  VEC (tree, heap) *exprs = sorted_array_from_bitmap_set (ANTIC_IN (block));
  tree expr;
  int i;

  for (i = 0; VEC_iterate (tree, exprs, i, expr); i++)
    {
      if (can_PRE_operation (expr) && !AGGREGATE_TYPE_P (TREE_TYPE (expr)))
	{
	  tree *avail;
	  tree val;
	  bool by_some = false;
	  bool cant_insert = false;
	  bool all_same = true;
	  tree first_s = NULL;
	  edge pred;
	  basic_block bprime;
	  tree eprime = NULL_TREE;
	  edge_iterator ei;

	  val = get_value_handle (expr);
	  if (bitmap_set_contains_value (PHI_GEN (block), val))
	    continue;
	  if (bitmap_set_contains_value (AVAIL_OUT (dom), val))
	    {
	      if (dump_file && (dump_flags & TDF_DETAILS))
		fprintf (dump_file, "Found fully redundant value\n");
	      continue;
	    }

	  avail = XCNEWVEC (tree, last_basic_block);
	  FOR_EACH_EDGE (pred, ei, block->preds)
	    {
	      tree vprime;
	      tree edoubleprime;

	      /* This can happen in the very weird case
		 that our fake infinite loop edges have caused a
		 critical edge to appear.  */
	      if (EDGE_CRITICAL_P (pred))
		{
		  cant_insert = true;
		  break;
		}
	      bprime = pred->src;
	      eprime = phi_translate (expr, ANTIC_IN (block), NULL,
				      bprime, block);

	      /* eprime will generally only be NULL if the
		 value of the expression, translated
		 through the PHI for this predecessor, is
		 undefined.  If that is the case, we can't
		 make the expression fully redundant,
		 because its value is undefined along a
		 predecessor path.  We can thus break out
		 early because it doesn't matter what the
		 rest of the results are.  */
	      if (eprime == NULL)
		{
		  cant_insert = true;
		  break;
		}

	      eprime = fully_constant_expression (eprime);
	      vprime = get_value_handle (eprime);
	      gcc_assert (vprime);
	      edoubleprime = bitmap_find_leader (AVAIL_OUT (bprime),
						 vprime);
	      if (edoubleprime == NULL)
		{
		  avail[bprime->index] = eprime;
		  all_same = false;
		}
	      else
		{
		  avail[bprime->index] = edoubleprime;
		  by_some = true;
		  if (first_s == NULL)
		    first_s = edoubleprime;
		  else if (!operand_equal_p (first_s, edoubleprime,
					     0))
		    all_same = false;
		}
	    }
	  /* If we can insert it, it's not the same value
	     already existing along every predecessor, and
	     it's defined by some predecessor, it is
	     partially redundant.  */
	  if (!cant_insert && !all_same && by_some)
	    {
	      if (insert_into_preds_of_block (block, get_expression_id (expr),
					      avail))
		new_stuff = true;
	    }
	  /* If all edges produce the same value and that value is
	     an invariant, then the PHI has the same value on all
	     edges.  Note this.  */
	  else if (!cant_insert && all_same && eprime
		   && is_gimple_min_invariant (eprime)
		   && !is_gimple_min_invariant (val))
	    {
	      unsigned int j;
	      bitmap_iterator bi;

	      bitmap_set_t exprset = VALUE_HANDLE_EXPR_SET (val);
	      FOR_EACH_EXPR_ID_IN_SET (exprset, j, bi)
		{
		  tree expr = expression_for_id (j);
		  if (TREE_CODE (expr) == SSA_NAME)
		    {
		      vn_add (expr, eprime);
		      pre_stats.constified++;
		    }
		}
	    }
	  free (avail);
	}
    }

  VEC_free (tree, heap, exprs);
  return new_stuff;
}


/* Perform insertion for partially anticipatable expressions.  There
   is only one case we will perform insertion for these.  This case is
   if the expression is partially anticipatable, and fully available.
   In this case, we know that putting it earlier will enable us to
   remove the later computation.  */


static bool
do_partial_partial_insertion (basic_block block, basic_block dom)
{
  bool new_stuff = false;
  VEC (tree, heap) *exprs = sorted_array_from_bitmap_set (PA_IN (block));
  tree expr;
  int i;

  for (i = 0; VEC_iterate (tree, exprs, i, expr); i++)
    {
      if (can_PRE_operation (expr) && !AGGREGATE_TYPE_P (TREE_TYPE (expr)))
	{
	  tree *avail;
	  tree val;
	  bool by_all = true;
	  bool cant_insert = false;
	  edge pred;
	  basic_block bprime;
	  tree eprime = NULL_TREE;
	  edge_iterator ei;

	  val = get_value_handle (expr);
	  if (bitmap_set_contains_value (PHI_GEN (block), val))
	    continue;
	  if (bitmap_set_contains_value (AVAIL_OUT (dom), val))
	    continue;

	  avail = XCNEWVEC (tree, last_basic_block);
	  FOR_EACH_EDGE (pred, ei, block->preds)
	    {
	      tree vprime;
	      tree edoubleprime;

	      /* This can happen in the very weird case
		 that our fake infinite loop edges have caused a
		 critical edge to appear.  */
	      if (EDGE_CRITICAL_P (pred))
		{
		  cant_insert = true;
		  break;
		}
	      bprime = pred->src;
	      eprime = phi_translate (expr, ANTIC_IN (block),
				      PA_IN (block),
				      bprime, block);

	      /* eprime will generally only be NULL if the
		 value of the expression, translated
		 through the PHI for this predecessor, is
		 undefined.  If that is the case, we can't
		 make the expression fully redundant,
		 because its value is undefined along a
		 predecessor path.  We can thus break out
		 early because it doesn't matter what the
		 rest of the results are.  */
	      if (eprime == NULL)
		{
		  cant_insert = true;
		  break;
		}

	      eprime = fully_constant_expression (eprime);
	      vprime = get_value_handle (eprime);
	      gcc_assert (vprime);
	      edoubleprime = bitmap_find_leader (AVAIL_OUT (bprime),
						 vprime);
	      if (edoubleprime == NULL)
		{
		  by_all = false;
		  break;
		}
	      else
		avail[bprime->index] = edoubleprime;

	    }

	  /* If we can insert it, it's not the same value
	     already existing along every predecessor, and
	     it's defined by some predecessor, it is
	     partially redundant.  */
	  if (!cant_insert && by_all)
	    {
	      pre_stats.pa_insert++;
	      if (insert_into_preds_of_block (block, get_expression_id (expr),
					      avail))
		new_stuff = true;
	    }
	  free (avail);
	}
    }

  VEC_free (tree, heap, exprs);
  return new_stuff;
}

static bool
insert_aux (basic_block block)
{
  basic_block son;
  bool new_stuff = false;

  if (block)
    {
      basic_block dom;
      dom = get_immediate_dominator (CDI_DOMINATORS, block);
      if (dom)
	{
	  unsigned i;
	  bitmap_iterator bi;
	  bitmap_set_t newset = NEW_SETS (dom);
	  if (newset)
	    {
	      /* Note that we need to value_replace both NEW_SETS, and
		 AVAIL_OUT. For both the case of NEW_SETS, the value may be
		 represented by some non-simple expression here that we want
		 to replace it with.  */
	      FOR_EACH_EXPR_ID_IN_SET (newset, i, bi)
		{
		  tree expr = expression_for_id (i);
		  bitmap_value_replace_in_set (NEW_SETS (block), expr);
		  bitmap_value_replace_in_set (AVAIL_OUT (block), expr);
		}
	    }
	  if (!single_pred_p (block))
	    {
	      new_stuff |= do_regular_insertion (block, dom);
	      if (do_partial_partial)
		new_stuff |= do_partial_partial_insertion (block, dom);
	    }
	}
    }
  for (son = first_dom_son (CDI_DOMINATORS, block);
       son;
       son = next_dom_son (CDI_DOMINATORS, son))
    {
      new_stuff |= insert_aux (son);
    }

  return new_stuff;
}

/* Perform insertion of partially redundant values.  */

static void
insert (void)
{
  bool new_stuff = true;
  basic_block bb;
  int num_iterations = 0;

  FOR_ALL_BB (bb)
    NEW_SETS (bb) = bitmap_set_new ();

  while (new_stuff)
    {
      num_iterations++;
      new_stuff = false;
      new_stuff = insert_aux (ENTRY_BLOCK_PTR);
    }
  if (num_iterations > 2 && dump_file && (dump_flags & TDF_STATS))
    fprintf (dump_file, "insert required %d iterations\n", num_iterations);
}


/* Add OP to EXP_GEN (block), and possibly to the maximal set if it is
   not defined by a phi node.
   PHI nodes can't go in the maximal sets because they are not in
   TMP_GEN, so it is possible to get into non-monotonic situations
   during ANTIC calculation, because it will *add* bits.  */

static void
add_to_exp_gen (basic_block block, tree op)
{
  if (!in_fre)
    {
      if (TREE_CODE (op) == SSA_NAME && ssa_undefined_value_p (op))
	return;
      bitmap_value_insert_into_set (EXP_GEN (block), op);
      if (TREE_CODE (op) != SSA_NAME
	  || TREE_CODE (SSA_NAME_DEF_STMT (op)) != PHI_NODE)
	bitmap_value_insert_into_set (maximal_set, op);
    }
}


/* Given an SSA variable VAR and an expression EXPR, compute the value
   number for EXPR and create a value handle (VAL) for it.  If VAR and
   EXPR are not the same, associate VAL with VAR.  Finally, add VAR to
   S1 and its value handle to S2, and to the maximal set if
   ADD_TO_MAXIMAL is true.

   VUSES represent the virtual use operands associated with EXPR (if
   any).  */

static inline void
add_to_sets (tree var, tree expr, VEC(tree, gc) *vuses, bitmap_set_t s1,
	     bitmap_set_t s2)
{
  tree val;
  val = vn_lookup_or_add_with_vuses (expr, vuses);

  /* VAR and EXPR may be the same when processing statements for which
     we are not computing value numbers (e.g., non-assignments, or
     statements that make aliased stores).  In those cases, we are
     only interested in making VAR available as its own value.  */
  if (var != expr)
    vn_add (var, val);

  if (s1)
    bitmap_insert_into_set (s1, var);

  bitmap_value_insert_into_set (s2, var);
}

/* Find existing value expression that is the same as T,
   and return it if it exists.  */

static inline tree
find_existing_value_expr (tree t, VEC (tree, gc) *vuses)
{
  bitmap_iterator bi;
  unsigned int bii;
  tree vh;
  bitmap_set_t exprset;

  if (REFERENCE_CLASS_P (t) || TREE_CODE (t) == CALL_EXPR || DECL_P (t))
    vh = vn_lookup_with_vuses (t, vuses);
  else
    vh = vn_lookup (t);

  if (!vh)
    return NULL;
  exprset = VALUE_HANDLE_EXPR_SET (vh);
  FOR_EACH_EXPR_ID_IN_SET (exprset, bii, bi)
    {
      tree efi = expression_for_id (bii);
      if (expressions_equal_p (t, efi))
	return efi;
    }
  return NULL;
}

/* Given a unary or binary expression EXPR, create and return a new
   expression with the same structure as EXPR but with its operands
   replaced with the value handles of each of the operands of EXPR.

   VUSES represent the virtual use operands associated with EXPR (if
   any). Insert EXPR's operands into the EXP_GEN set for BLOCK. */

static inline tree
create_value_expr_from (tree expr, basic_block block, VEC (tree, gc) *vuses)
{
  int i;
  enum tree_code code = TREE_CODE (expr);
  tree vexpr;
  alloc_pool pool = NULL;
  tree efi;

  gcc_assert (TREE_CODE_CLASS (code) == tcc_unary
	      || TREE_CODE_CLASS (code) == tcc_binary
	      || TREE_CODE_CLASS (code) == tcc_comparison
	      || TREE_CODE_CLASS (code) == tcc_reference
	      || TREE_CODE_CLASS (code) == tcc_expression
	      || TREE_CODE_CLASS (code) == tcc_vl_exp
	      || TREE_CODE_CLASS (code) == tcc_exceptional
	      || TREE_CODE_CLASS (code) == tcc_declaration);

  if (TREE_CODE_CLASS (code) == tcc_unary)
    pool = unary_node_pool;
  else if (TREE_CODE_CLASS (code) == tcc_reference)
    pool = reference_node_pool;
  else if (TREE_CODE_CLASS (code) == tcc_binary)
    pool = binary_node_pool;
  else if (TREE_CODE_CLASS (code) == tcc_comparison)
    pool = comparison_node_pool;
  else
    gcc_assert (code == CALL_EXPR);

  if (code == CALL_EXPR)
    vexpr = temp_copy_call_expr (expr);
  else
    {
      vexpr = (tree) pool_alloc (pool);
      memcpy (vexpr, expr, tree_size (expr));
    }

  for (i = 0; i < TREE_OPERAND_LENGTH (expr); i++)
    {
      tree val = NULL_TREE;
      tree op;

      op = TREE_OPERAND (expr, i);
      if (op == NULL_TREE)
	continue;

      /* Recursively value-numberize reference ops and tree lists.  */
      if (REFERENCE_CLASS_P (op))
	{
	  tree tempop = create_value_expr_from (op, block, vuses);
	  op = tempop ? tempop : op;
	  val = vn_lookup_or_add_with_vuses (op, vuses);
	  set_expression_vuses (op, vuses);
	}
      else
	{
	  val = vn_lookup_or_add (op);
	}
      if (TREE_CODE (op) != TREE_LIST)
	add_to_exp_gen (block, op);

      if (TREE_CODE (val) == VALUE_HANDLE)
	TREE_TYPE (val) = TREE_TYPE (TREE_OPERAND (vexpr, i));

      TREE_OPERAND (vexpr, i) = val;
    }
  efi = find_existing_value_expr (vexpr, vuses);
  if (efi)
    return efi;
  get_or_alloc_expression_id (vexpr);
  return vexpr;
}

/* Return a copy of NODE that is stored in the temporary alloc_pool's.
   This is made recursively true, so that the operands are stored in
   the pool as well.  */

static tree
poolify_tree (tree node)
{
  switch  (TREE_CODE (node))
    {
    case INDIRECT_REF:
      {
	tree temp = (tree) pool_alloc (reference_node_pool);
	memcpy (temp, node, tree_size (node));
	TREE_OPERAND (temp, 0) = poolify_tree (TREE_OPERAND (temp, 0));
	return temp;
      }
      break;
    case GIMPLE_MODIFY_STMT:
      {
	tree temp = (tree) pool_alloc (modify_expr_node_pool);
	memcpy (temp, node, tree_size (node));
	GIMPLE_STMT_OPERAND (temp, 0) =
	  poolify_tree (GIMPLE_STMT_OPERAND (temp, 0));
	GIMPLE_STMT_OPERAND (temp, 1) =
	  poolify_tree (GIMPLE_STMT_OPERAND (temp, 1));
	return temp;
      }
      break;
    case SSA_NAME:
    case INTEGER_CST:
    case STRING_CST:
    case REAL_CST:
    case FIXED_CST:
    case PARM_DECL:
    case VAR_DECL:
    case RESULT_DECL:
      return node;
    default:
      gcc_unreachable ();
    }
}

static tree modify_expr_template;

/* Allocate a GIMPLE_MODIFY_STMT with TYPE, and operands OP1, OP2 in the
   alloc pools and return it.  */
static tree
poolify_modify_stmt (tree op1, tree op2)
{
  if (modify_expr_template == NULL)
    modify_expr_template = build_gimple_modify_stmt (op1, op2);

  GIMPLE_STMT_OPERAND (modify_expr_template, 0) = op1;
  GIMPLE_STMT_OPERAND (modify_expr_template, 1) = op2;

  return poolify_tree (modify_expr_template);
}


/* For each real store operation of the form
   *a = <value> that we see, create a corresponding fake store of the
   form storetmp_<version> = *a.

   This enables AVAIL computation to mark the results of stores as
   available.  Without this, you'd need to do some computation to
   mark the result of stores as ANTIC and AVAIL at all the right
   points.
   To save memory, we keep the store
   statements pool allocated until we decide whether they are
   necessary or not.  */

static void
insert_fake_stores (void)
{
  basic_block block;

  FOR_ALL_BB (block)
    {
      block_stmt_iterator bsi;
      for (bsi = bsi_start (block); !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  tree stmt = bsi_stmt (bsi);

	  /* We can't generate SSA names for stores that are complex
	     or aggregate.  We also want to ignore things whose
	     virtual uses occur in abnormal phis.  */

	  if (TREE_CODE (stmt) == GIMPLE_MODIFY_STMT
	      && TREE_CODE (GIMPLE_STMT_OPERAND (stmt, 0)) == INDIRECT_REF
	      && !AGGREGATE_TYPE_P (TREE_TYPE (GIMPLE_STMT_OPERAND (stmt, 0)))
	      && TREE_CODE (TREE_TYPE (GIMPLE_STMT_OPERAND
					(stmt, 0))) != COMPLEX_TYPE)
	    {
	      ssa_op_iter iter;
	      def_operand_p defp;
	      tree lhs = GIMPLE_STMT_OPERAND (stmt, 0);
	      tree rhs = GIMPLE_STMT_OPERAND (stmt, 1);
	      tree new_tree;
	      bool notokay = false;

	      FOR_EACH_SSA_DEF_OPERAND (defp, stmt, iter, SSA_OP_VIRTUAL_DEFS)
		{
		  tree defvar = DEF_FROM_PTR (defp);
		  if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (defvar))
		    {
		      notokay = true;
		      break;
		    }
		}

	      if (notokay)
		continue;

	      if (!storetemp || TREE_TYPE (rhs) != TREE_TYPE (storetemp))
		{
		  storetemp = create_tmp_var (TREE_TYPE (rhs), "storetmp");
		  if (TREE_CODE (TREE_TYPE (storetemp)) == VECTOR_TYPE)
		    DECL_GIMPLE_REG_P (storetemp) = 1;
		  get_var_ann (storetemp);
		}

	      new_tree = poolify_modify_stmt (storetemp, lhs);

	      lhs = make_ssa_name (storetemp, new_tree);
	      GIMPLE_STMT_OPERAND (new_tree, 0) = lhs;
	      create_ssa_artificial_load_stmt (new_tree, stmt, false);

	      NECESSARY (new_tree) = 0;
	      VEC_safe_push (tree, heap, inserted_exprs, new_tree);
	      VEC_safe_push (tree, heap, need_creation, new_tree);
	      bsi_insert_after (&bsi, new_tree, BSI_NEW_STMT);
	    }
	}
    }
}

/* Turn the pool allocated fake stores that we created back into real
   GC allocated ones if they turned out to be necessary to PRE some
   expressions.  */

static void
realify_fake_stores (void)
{
  unsigned int i;
  tree stmt;

  for (i = 0; VEC_iterate (tree, need_creation, i, stmt); i++)
    {
      if (NECESSARY (stmt))
	{
	  block_stmt_iterator bsi;
	  tree newstmt, tmp;

	  /* Mark the temp variable as referenced */
	  add_referenced_var (SSA_NAME_VAR (GIMPLE_STMT_OPERAND (stmt, 0)));

	  /* Put the new statement in GC memory, fix up the
	     SSA_NAME_DEF_STMT on it, and then put it in place of
	     the old statement before the store in the IR stream
	     as a plain ssa name copy.  */
	  bsi = bsi_for_stmt (stmt);
	  bsi_prev (&bsi);
	  tmp = GIMPLE_STMT_OPERAND (bsi_stmt (bsi), 1);
	  newstmt = build_gimple_modify_stmt (GIMPLE_STMT_OPERAND (stmt, 0),
					      tmp);
	  SSA_NAME_DEF_STMT (GIMPLE_STMT_OPERAND (newstmt, 0)) = newstmt;
	  bsi_insert_before (&bsi, newstmt, BSI_SAME_STMT);
	  bsi = bsi_for_stmt (stmt);
	  bsi_remove (&bsi, true);
	}
      else
	release_defs (stmt);
    }
}

/* Given an SSA_NAME, see if SCCVN has a value number for it, and if
   so, return the value handle for this value number, creating it if
   necessary.
   Return NULL if SCCVN has no info for us.  */

static tree
get_sccvn_value (tree name)
{
  if (TREE_CODE (name) == SSA_NAME
      && VN_INFO (name)->valnum != name
      && VN_INFO (name)->valnum != VN_TOP)
    {
      tree val = VN_INFO (name)->valnum;
      bool is_invariant = is_gimple_min_invariant (val);
      tree valvh = !is_invariant ? get_value_handle (val) : NULL_TREE;

      /* We may end up with situations where SCCVN has chosen a
	 representative for the equivalence set that we have not
	 visited yet.  In this case, just create the value handle for
	 it.  */
      if (!valvh && !is_invariant)
	{
	  tree defstmt = SSA_NAME_DEF_STMT (val);

	  gcc_assert (VN_INFO (val)->valnum == val);
	  /* PHI nodes can't have vuses and attempts to iterate over
	     their VUSE operands will crash.  */
	  if (TREE_CODE (defstmt) == PHI_NODE || IS_EMPTY_STMT (defstmt))
	    defstmt = NULL;
	  {
	    tree defstmt2 = SSA_NAME_DEF_STMT (name);
	    if (TREE_CODE (defstmt2) != PHI_NODE &&
		!ZERO_SSA_OPERANDS (defstmt2, SSA_OP_ALL_VIRTUALS))
	      gcc_assert (defstmt);
	  }
	  valvh = vn_lookup_or_add_with_stmt (val, defstmt);
	}

      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "SCCVN says ");
	  print_generic_expr (dump_file, name, 0);
	  fprintf (dump_file, " value numbers to ");
	  if (valvh && !is_invariant)
	    {
	      print_generic_expr (dump_file, val, 0);
	      fprintf (dump_file, " (");
	      print_generic_expr (dump_file, valvh, 0);
	      fprintf (dump_file, ")\n");
	    }
	  else
	    print_generic_stmt (dump_file, val, 0);
	}
      if (valvh)
	return valvh;
      else
	return val;
    }
  return NULL_TREE;
}

/* Create value handles for PHI in BLOCK.  */

static void
make_values_for_phi (tree phi, basic_block block)
{
  tree result = PHI_RESULT (phi);
  /* We have no need for virtual phis, as they don't represent
     actual computations.  */
  if (is_gimple_reg (result))
    {
      tree sccvnval = get_sccvn_value (result);
      if (sccvnval)
	{
	  vn_add (result, sccvnval);
	  bitmap_insert_into_set (PHI_GEN (block), result);
	  bitmap_value_insert_into_set (AVAIL_OUT (block), result);
	}
      else
	add_to_sets (result, result, NULL,
		     PHI_GEN (block), AVAIL_OUT (block));
    }
}

/* Create value handles for STMT in BLOCK.  Return true if we handled
   the statement.  */

static bool
make_values_for_stmt (tree stmt, basic_block block)
{

  tree lhs = GIMPLE_STMT_OPERAND (stmt, 0);
  tree rhs = GIMPLE_STMT_OPERAND (stmt, 1);
  tree valvh = NULL_TREE;
  tree lhsval;
  VEC (tree, gc) *vuses = NULL;

  valvh = get_sccvn_value (lhs);

  if (valvh)
    {
      vn_add (lhs, valvh);
      bitmap_value_insert_into_set (AVAIL_OUT (block), lhs);
      /* Shortcut for FRE. We have no need to create value expressions,
	 just want to know what values are available where.  */
      if (in_fre)
	return true;

    }
  else if (in_fre)
    {
      /* For FRE, if SCCVN didn't find anything, we aren't going to
	 either, so just make up a new value number if necessary and
	 call it a day */
      if (get_value_handle (lhs) == NULL)
	vn_lookup_or_add (lhs);
      bitmap_value_insert_into_set (AVAIL_OUT (block), lhs);
      return true;
    }

  lhsval = valvh ? valvh : get_value_handle (lhs);
  vuses = copy_vuses_from_stmt (stmt);
  STRIP_USELESS_TYPE_CONVERSION (rhs);
  if (can_value_number_operation (rhs)
      && (!lhsval || !is_gimple_min_invariant (lhsval)))
    {
      /* For value numberable operation, create a
	 duplicate expression with the operands replaced
	 with the value handles of the original RHS.  */
      tree newt = create_value_expr_from (rhs, block, vuses);
      if (newt)
	{
	  set_expression_vuses (newt, vuses);
	  /* If we already have a value number for the LHS, reuse
	     it rather than creating a new one.  */
	  if (lhsval)
	    {
	      set_value_handle (newt, lhsval);
	      if (!is_gimple_min_invariant (lhsval))
		add_to_value (lhsval, newt);
	    }
	  else
	    {
	      tree val = vn_lookup_or_add_with_vuses (newt, vuses);
	      vn_add (lhs, val);
	    }

	  add_to_exp_gen (block, newt);
	}

      bitmap_insert_into_set (TMP_GEN (block), lhs);
      bitmap_value_insert_into_set (AVAIL_OUT (block), lhs);
      return true;
    }
  else if ((TREE_CODE (rhs) == SSA_NAME
	    && !SSA_NAME_OCCURS_IN_ABNORMAL_PHI (rhs))
	   || is_gimple_min_invariant (rhs)
	   || TREE_CODE (rhs) == ADDR_EXPR
	   || TREE_INVARIANT (rhs)
	   || DECL_P (rhs))
    {

      if (lhsval)
	{
	  set_expression_vuses (rhs, vuses);
	  set_value_handle (rhs, lhsval);
	  if (!is_gimple_min_invariant (lhsval))
	    add_to_value (lhsval, rhs);
	  bitmap_insert_into_set (TMP_GEN (block), lhs);
	  bitmap_value_insert_into_set (AVAIL_OUT (block), lhs);
	}
      else
	{
	  /* Compute a value number for the RHS of the statement
	     and add its value to the AVAIL_OUT set for the block.
	     Add the LHS to TMP_GEN.  */
	  set_expression_vuses (rhs, vuses);
	  add_to_sets (lhs, rhs, vuses, TMP_GEN (block),
		       AVAIL_OUT (block));
	}
      /* None of the rest of these can be PRE'd.  */
      if (TREE_CODE (rhs) == SSA_NAME && !ssa_undefined_value_p (rhs))
	add_to_exp_gen (block, rhs);
      return true;
    }
  return false;

}

/* Compute the AVAIL set for all basic blocks.

   This function performs value numbering of the statements in each basic
   block.  The AVAIL sets are built from information we glean while doing
   this value numbering, since the AVAIL sets contain only one entry per
   value.

   AVAIL_IN[BLOCK] = AVAIL_OUT[dom(BLOCK)].
   AVAIL_OUT[BLOCK] = AVAIL_IN[BLOCK] U PHI_GEN[BLOCK] U TMP_GEN[BLOCK].  */

static void
compute_avail (void)
{
  basic_block block, son;
  basic_block *worklist;
  size_t sp = 0;
  tree param;

  /* For arguments with default definitions, we pretend they are
     defined in the entry block.  */
  for (param = DECL_ARGUMENTS (current_function_decl);
       param;
       param = TREE_CHAIN (param))
    {
      if (gimple_default_def (cfun, param) != NULL)
	{
	  tree def = gimple_default_def (cfun, param);

	  vn_lookup_or_add (def);
	  if (!in_fre)
	    {
	      bitmap_insert_into_set (TMP_GEN (ENTRY_BLOCK_PTR), def);
	      bitmap_value_insert_into_set (maximal_set, def);
	    }
	  bitmap_value_insert_into_set (AVAIL_OUT (ENTRY_BLOCK_PTR), def);
	}
    }

  /* Likewise for the static chain decl. */
  if (cfun->static_chain_decl)
    {
      param = cfun->static_chain_decl;
      if (gimple_default_def (cfun, param) != NULL)
	{
	  tree def = gimple_default_def (cfun, param);

	  vn_lookup_or_add (def);
	  if (!in_fre)
	    {
	      bitmap_insert_into_set (TMP_GEN (ENTRY_BLOCK_PTR), def);
	      bitmap_value_insert_into_set (maximal_set, def);
	    }
	  bitmap_value_insert_into_set (AVAIL_OUT (ENTRY_BLOCK_PTR), def);
	}
    }

  /* Allocate the worklist.  */
  worklist = XNEWVEC (basic_block, n_basic_blocks);

  /* Seed the algorithm by putting the dominator children of the entry
     block on the worklist.  */
  for (son = first_dom_son (CDI_DOMINATORS, ENTRY_BLOCK_PTR);
       son;
       son = next_dom_son (CDI_DOMINATORS, son))
    worklist[sp++] = son;

  /* Loop until the worklist is empty.  */
  while (sp)
    {
      block_stmt_iterator bsi;
      tree stmt, phi;
      basic_block dom;
      unsigned int stmt_uid = 1;

      /* Pick a block from the worklist.  */
      block = worklist[--sp];

      /* Initially, the set of available values in BLOCK is that of
	 its immediate dominator.  */
      dom = get_immediate_dominator (CDI_DOMINATORS, block);
      if (dom)
	bitmap_set_copy (AVAIL_OUT (block), AVAIL_OUT (dom));

      /* Generate values for PHI nodes.  */
      for (phi = phi_nodes (block); phi; phi = PHI_CHAIN (phi))
	make_values_for_phi (phi, block);

      /* Now compute value numbers and populate value sets with all
	 the expressions computed in BLOCK.  */
      for (bsi = bsi_start (block); !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  stmt_ann_t ann;
	  ssa_op_iter iter;
	  tree op;

	  stmt = bsi_stmt (bsi);
	  ann = stmt_ann (stmt);

	  ann->uid = stmt_uid++;

	  /* For regular value numbering, we are only interested in
	     assignments of the form X_i = EXPR, where EXPR represents
	     an "interesting" computation, it has no volatile operands
	     and X_i doesn't flow through an abnormal edge.  */
	  if (TREE_CODE (stmt) == RETURN_EXPR
	      && !ann->has_volatile_ops)
	    {
	      tree realstmt = stmt;
	      tree lhs;
	      tree rhs;

	      stmt = TREE_OPERAND (stmt, 0);
	      if (stmt && TREE_CODE (stmt) == GIMPLE_MODIFY_STMT)
		{
		  lhs = GIMPLE_STMT_OPERAND (stmt, 0);
		  rhs = GIMPLE_STMT_OPERAND (stmt, 1);
		  if (TREE_CODE (lhs) == SSA_NAME
		      && is_gimple_min_invariant (VN_INFO (lhs)->valnum))
		    {
		      if (dump_file && (dump_flags & TDF_DETAILS))
			{
			  fprintf (dump_file, "SCCVN says ");
			  print_generic_expr (dump_file, lhs, 0);
			  fprintf (dump_file, " value numbers to ");
			  print_generic_stmt (dump_file, VN_INFO (lhs)->valnum,
					      0);
			}
		      vn_add (lhs, VN_INFO (lhs)->valnum);
		      continue;
		    }

		  if (TREE_CODE (rhs) == SSA_NAME)
		    add_to_exp_gen (block, rhs);

		  FOR_EACH_SSA_TREE_OPERAND (op, realstmt, iter, SSA_OP_DEF)
		    add_to_sets (op, op, NULL, TMP_GEN (block),
				 AVAIL_OUT (block));
		}
	      continue;
	    }

	  else if (TREE_CODE (stmt) == GIMPLE_MODIFY_STMT
		   && !ann->has_volatile_ops
		   && TREE_CODE (GIMPLE_STMT_OPERAND (stmt, 0)) == SSA_NAME
		   && (!SSA_NAME_OCCURS_IN_ABNORMAL_PHI
		       (GIMPLE_STMT_OPERAND (stmt, 0)))
		   && !tree_could_throw_p (stmt))
	    {
	      if (make_values_for_stmt (stmt, block))
		continue;

	    }

	  /* For any other statement that we don't recognize, simply
	     make the names generated by the statement available in
	     AVAIL_OUT and TMP_GEN.  */
	  FOR_EACH_SSA_TREE_OPERAND (op, stmt, iter, SSA_OP_DEF)
	    add_to_sets (op, op, NULL, TMP_GEN (block), AVAIL_OUT (block));

	  FOR_EACH_SSA_TREE_OPERAND (op, stmt, iter, SSA_OP_USE)
	    {
	      add_to_sets (op, op, NULL, NULL , AVAIL_OUT (block));
	      if (TREE_CODE (op) == SSA_NAME || can_PRE_operation (op))
		add_to_exp_gen (block, op);
	    }
	}

      /* Put the dominator children of BLOCK on the worklist of blocks
	 to compute available sets for.  */
      for (son = first_dom_son (CDI_DOMINATORS, block);
	   son;
	   son = next_dom_son (CDI_DOMINATORS, son))
	worklist[sp++] = son;
    }

  free (worklist);
}


/* Eliminate fully redundant computations.  */

static void
eliminate (void)
{
  basic_block b;

  FOR_EACH_BB (b)
    {
      block_stmt_iterator i;

      for (i = bsi_start (b); !bsi_end_p (i); bsi_next (&i))
	{
	  tree stmt = bsi_stmt (i);

	  /* Lookup the RHS of the expression, see if we have an
	     available computation for it.  If so, replace the RHS with
	     the available computation.  */
	  if (TREE_CODE (stmt) == GIMPLE_MODIFY_STMT
	      && TREE_CODE (GIMPLE_STMT_OPERAND (stmt, 0)) == SSA_NAME
	      && TREE_CODE (GIMPLE_STMT_OPERAND (stmt, 1)) != SSA_NAME
	      && !is_gimple_min_invariant (GIMPLE_STMT_OPERAND (stmt, 1))
	      && !stmt_ann (stmt)->has_volatile_ops)
	    {
	      tree lhs = GIMPLE_STMT_OPERAND (stmt, 0);
	      tree *rhs_p = &GIMPLE_STMT_OPERAND (stmt, 1);
	      tree sprime;

	      sprime = bitmap_find_leader (AVAIL_OUT (b),
					   get_value_handle (lhs));

	      if (sprime
		  && sprime != lhs
		  && (TREE_CODE (*rhs_p) != SSA_NAME
		      || may_propagate_copy (*rhs_p, sprime)))
		{
		  gcc_assert (sprime != *rhs_p);

		  if (dump_file && (dump_flags & TDF_DETAILS))
		    {
		      fprintf (dump_file, "Replaced ");
		      print_generic_expr (dump_file, *rhs_p, 0);
		      fprintf (dump_file, " with ");
		      print_generic_expr (dump_file, sprime, 0);
		      fprintf (dump_file, " in ");
		      print_generic_stmt (dump_file, stmt, 0);
		    }

		  if (TREE_CODE (sprime) == SSA_NAME)
		    NECESSARY (SSA_NAME_DEF_STMT (sprime)) = 1;
		  /* We need to make sure the new and old types actually match,
		     which may require adding a simple cast, which fold_convert
		     will do for us.  */
		  if (TREE_CODE (*rhs_p) != SSA_NAME
		      && !useless_type_conversion_p (TREE_TYPE (*rhs_p),
						    TREE_TYPE (sprime)))
		    sprime = fold_convert (TREE_TYPE (*rhs_p), sprime);

		  pre_stats.eliminations++;
		  propagate_tree_value (rhs_p, sprime);
		  update_stmt (stmt);

		  /* If we removed EH side effects from the statement, clean
		     its EH information.  */
		  if (maybe_clean_or_replace_eh_stmt (stmt, stmt))
		    {
		      bitmap_set_bit (need_eh_cleanup,
				      bb_for_stmt (stmt)->index);
		      if (dump_file && (dump_flags & TDF_DETAILS))
			fprintf (dump_file, "  Removed EH side effects.\n");
		    }
		}
	    }
	}
    }
}

/* Borrow a bit of tree-ssa-dce.c for the moment.
   XXX: In 4.1, we should be able to just run a DCE pass after PRE, though
   this may be a bit faster, and we may want critical edges kept split.  */

/* If OP's defining statement has not already been determined to be necessary,
   mark that statement necessary. Return the stmt, if it is newly
   necessary.  */

static inline tree
mark_operand_necessary (tree op)
{
  tree stmt;

  gcc_assert (op);

  if (TREE_CODE (op) != SSA_NAME)
    return NULL;

  stmt = SSA_NAME_DEF_STMT (op);
  gcc_assert (stmt);

  if (NECESSARY (stmt)
      || IS_EMPTY_STMT (stmt))
    return NULL;

  NECESSARY (stmt) = 1;
  return stmt;
}

/* Because we don't follow exactly the standard PRE algorithm, and decide not
   to insert PHI nodes sometimes, and because value numbering of casts isn't
   perfect, we sometimes end up inserting dead code.   This simple DCE-like
   pass removes any insertions we made that weren't actually used.  */

static void
remove_dead_inserted_code (void)
{
  VEC(tree,heap) *worklist = NULL;
  int i;
  tree t;

  worklist = VEC_alloc (tree, heap, VEC_length (tree, inserted_exprs));
  for (i = 0; VEC_iterate (tree, inserted_exprs, i, t); i++)
    {
      if (NECESSARY (t))
	VEC_quick_push (tree, worklist, t);
    }
  while (VEC_length (tree, worklist) > 0)
    {
      t = VEC_pop (tree, worklist);

      /* PHI nodes are somewhat special in that each PHI alternative has
	 data and control dependencies.  All the statements feeding the
	 PHI node's arguments are always necessary. */
      if (TREE_CODE (t) == PHI_NODE)
	{
	  int k;

	  VEC_reserve (tree, heap, worklist, PHI_NUM_ARGS (t));
	  for (k = 0; k < PHI_NUM_ARGS (t); k++)
	    {
	      tree arg = PHI_ARG_DEF (t, k);
	      if (TREE_CODE (arg) == SSA_NAME)
		{
		  arg = mark_operand_necessary (arg);
		  if (arg)
		    VEC_quick_push (tree, worklist, arg);
		}
	    }
	}
      else
	{
	  /* Propagate through the operands.  Examine all the USE, VUSE and
	     VDEF operands in this statement.  Mark all the statements
	     which feed this statement's uses as necessary.  */
	  ssa_op_iter iter;
	  tree use;

	  /* The operands of VDEF expressions are also needed as they
	     represent potential definitions that may reach this
	     statement (VDEF operands allow us to follow def-def
	     links).  */

	  FOR_EACH_SSA_TREE_OPERAND (use, t, iter, SSA_OP_ALL_USES)
	    {
	      tree n = mark_operand_necessary (use);
	      if (n)
		VEC_safe_push (tree, heap, worklist, n);
	    }
	}
    }

  for (i = 0; VEC_iterate (tree, inserted_exprs, i, t); i++)
    {
      if (!NECESSARY (t))
	{
	  block_stmt_iterator bsi;

	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "Removing unnecessary insertion:");
	      print_generic_stmt (dump_file, t, 0);
	    }

	  if (TREE_CODE (t) == PHI_NODE)
	    {
	      remove_phi_node (t, NULL, true);
	    }
	  else
	    {
	      bsi = bsi_for_stmt (t);
	      bsi_remove (&bsi, true);
	      release_defs (t);
	    }
	}
    }
  VEC_free (tree, heap, worklist);
}

/* Initialize data structures used by PRE.  */

static void
init_pre (bool do_fre)
{
  basic_block bb;

  next_expression_id = 0;
  expressions = NULL;
  expression_vuses = NULL;
  in_fre = do_fre;

  inserted_exprs = NULL;
  need_creation = NULL;
  pretemp = NULL_TREE;
  storetemp = NULL_TREE;
  prephitemp = NULL_TREE;

  if (!do_fre)
    loop_optimizer_init (LOOPS_NORMAL);

  connect_infinite_loops_to_exit ();
  memset (&pre_stats, 0, sizeof (pre_stats));


  postorder = XNEWVEC (int, n_basic_blocks - NUM_FIXED_BLOCKS);
  post_order_compute (postorder, false, false);

  FOR_ALL_BB (bb)
    bb->aux = xcalloc (1, sizeof (struct bb_bitmap_sets));

  calculate_dominance_info (CDI_POST_DOMINATORS);
  calculate_dominance_info (CDI_DOMINATORS);

  bitmap_obstack_initialize (&grand_bitmap_obstack);
  phi_translate_table = htab_create (5110, expr_pred_trans_hash,
				     expr_pred_trans_eq, free);
  seen_during_translate = BITMAP_ALLOC (&grand_bitmap_obstack);
  bitmap_set_pool = create_alloc_pool ("Bitmap sets",
				       sizeof (struct bitmap_set), 30);
  binary_node_pool = create_alloc_pool ("Binary tree nodes",
					tree_code_size (PLUS_EXPR), 30);
  unary_node_pool = create_alloc_pool ("Unary tree nodes",
				       tree_code_size (NEGATE_EXPR), 30);
  reference_node_pool = create_alloc_pool ("Reference tree nodes",
					   tree_code_size (ARRAY_REF), 30);
  comparison_node_pool = create_alloc_pool ("Comparison tree nodes",
					    tree_code_size (EQ_EXPR), 30);
  modify_expr_node_pool = create_alloc_pool ("GIMPLE_MODIFY_STMT nodes",
					     tree_code_size (GIMPLE_MODIFY_STMT),
					     30);
  obstack_init (&temp_call_expr_obstack);
  modify_expr_template = NULL;

  FOR_ALL_BB (bb)
    {
      EXP_GEN (bb) = bitmap_set_new ();
      PHI_GEN (bb) = bitmap_set_new ();
      TMP_GEN (bb) = bitmap_set_new ();
      AVAIL_OUT (bb) = bitmap_set_new ();
    }
  maximal_set = in_fre ? NULL : bitmap_set_new ();

  need_eh_cleanup = BITMAP_ALLOC (NULL);
}


/* Deallocate data structures used by PRE.  */

static void
fini_pre (void)
{
  basic_block bb;
  unsigned int i;

  free (postorder);
  VEC_free (tree, heap, inserted_exprs);
  VEC_free (tree, heap, need_creation);
  bitmap_obstack_release (&grand_bitmap_obstack);
  free_alloc_pool (bitmap_set_pool);
  free_alloc_pool (binary_node_pool);
  free_alloc_pool (reference_node_pool);
  free_alloc_pool (unary_node_pool);
  free_alloc_pool (comparison_node_pool);
  free_alloc_pool (modify_expr_node_pool);
  htab_delete (phi_translate_table);
  remove_fake_exit_edges ();

  FOR_ALL_BB (bb)
    {
      free (bb->aux);
      bb->aux = NULL;
    }

  free_dominance_info (CDI_POST_DOMINATORS);

  if (!bitmap_empty_p (need_eh_cleanup))
    {
      tree_purge_all_dead_eh_edges (need_eh_cleanup);
      cleanup_tree_cfg ();
    }

  BITMAP_FREE (need_eh_cleanup);

  /* Wipe out pointers to VALUE_HANDLEs.  In the not terribly distant
     future we will want them to be persistent though.  */
  for (i = 0; i < num_ssa_names; i++)
    {
      tree name = ssa_name (i);

      if (!name)
	continue;

      if (SSA_NAME_VALUE (name)
	  && TREE_CODE (SSA_NAME_VALUE (name)) == VALUE_HANDLE)
	SSA_NAME_VALUE (name) = NULL;
    }
  if (current_loops != NULL)
    loop_optimizer_finalize ();
}

/* Main entry point to the SSA-PRE pass.  DO_FRE is true if the caller
   only wants to do full redundancy elimination.  */

static void
execute_pre (bool do_fre)
{

  do_partial_partial = optimize > 2;
  init_pre (do_fre);

  if (!do_fre)
    insert_fake_stores ();

  /* Collect and value number expressions computed in each basic block.  */
  if (!run_scc_vn ())
    {
      if (!do_fre)
	remove_dead_inserted_code ();
      fini_pre ();
      return;
    }
  switch_to_PRE_table ();
  compute_avail ();

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      basic_block bb;

      FOR_ALL_BB (bb)
	{
	  print_bitmap_set (dump_file, EXP_GEN (bb), "exp_gen", bb->index);
	  print_bitmap_set (dump_file, TMP_GEN (bb), "tmp_gen",
				  bb->index);
	  print_bitmap_set (dump_file, AVAIL_OUT (bb), "avail_out",
				  bb->index);
	}
    }

  /* Insert can get quite slow on an incredibly large number of basic
     blocks due to some quadratic behavior.  Until this behavior is
     fixed, don't run it when he have an incredibly large number of
     bb's.  If we aren't going to run insert, there is no point in
     computing ANTIC, either, even though it's plenty fast.  */
  if (!do_fre && n_basic_blocks < 4000)
    {
      compute_antic ();
      insert ();
    }

  /* Remove all the redundant expressions.  */
  eliminate ();

  if (dump_file && (dump_flags & TDF_STATS))
    {
      fprintf (dump_file, "Insertions: %d\n", pre_stats.insertions);
      fprintf (dump_file, "PA inserted: %d\n", pre_stats.pa_insert);
      fprintf (dump_file, "New PHIs: %d\n", pre_stats.phis);
      fprintf (dump_file, "Eliminated: %d\n", pre_stats.eliminations);
      fprintf (dump_file, "Constified: %d\n", pre_stats.constified);
    }
  bsi_commit_edge_inserts ();

  free_scc_vn ();
  clear_expression_ids ();
  if (!do_fre)
    {
      remove_dead_inserted_code ();
      realify_fake_stores ();
    }

  fini_pre ();
}

/* Gate and execute functions for PRE.  */

static unsigned int
do_pre (void)
{
  execute_pre (false);
  return TODO_rebuild_alias;
}

static bool
gate_pre (void)
{
  return flag_tree_pre != 0;
}

struct tree_opt_pass pass_pre =
{
  "pre",				/* name */
  gate_pre,				/* gate */
  do_pre,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_PRE,				/* tv_id */
  PROP_no_crit_edges | PROP_cfg
    | PROP_ssa | PROP_alias,		/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_update_ssa_only_virtuals | TODO_dump_func | TODO_ggc_collect
  | TODO_verify_ssa, /* todo_flags_finish */
  0					/* letter */
};


/* Gate and execute functions for FRE.  */

static unsigned int
execute_fre (void)
{
  execute_pre (true);
  return 0;
}

static bool
gate_fre (void)
{
  return flag_tree_fre != 0;
}

struct tree_opt_pass pass_fre =
{
  "fre",				/* name */
  gate_fre,				/* gate */
  execute_fre,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_FRE,				/* tv_id */
  PROP_cfg | PROP_ssa | PROP_alias,	/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_ggc_collect | TODO_verify_ssa, /* todo_flags_finish */
  0					/* letter */
};
