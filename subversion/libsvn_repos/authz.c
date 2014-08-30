/* authz.c : path-based access control
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */


/*** Includes. ***/

#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_fnmatch.h>

#include "svn_hash.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_repos.h"
#include "svn_config.h"
#include "svn_ctype.h"
#include "private/svn_fspath.h"
#include "private/svn_repos_private.h"
#include "private/svn_sorts_private.h"
#include "private/svn_subr_private.h"
#include "repos.h"
#include "authz.h"


/*** Access rights. ***/

/* This structure describes the access rights given to a specific user by
 * a path rule (actually the rule set specified for a path).  I.e. there is
 * one instance of this per path rule.
 */
typedef struct access_t
{
  /* Sequence number of the path rule that this struct was derived from.
   * If multiple rules apply to the same path (only possible with wildcard
   * matching), the one with the highest SEQUENCE_NUMBER wins, i.e. the latest
   * one defined in the authz file.
   *
   * A value of 0 denotes the default rule at the repository root denying
   * access to everybody.  User-defined path rules start with ID 1.
   */
  int sequence_number;

  /* Access rights of the respective user as defined by the rule set. */
  svn_repos_authz_access_t rights;
} access_t;

/* Use this to indicate that no sequence ID has been assigned.
 * It will automatically be inferior to (less than) any other sequence ID. */
#define NO_SEQUENCE_NUMBER (-1)

/* Convenience structure combining the node-local access rights with the
 * min and max rights granted within the sub-tree. */
typedef struct limited_rights_t
{
  /* Access granted to the current user.  If the SEQUENCE_NUMBER member is
   * NO_SEQUENCE_NUMBER, there has been no specific path rule for this PATH
   * but only for some sub-path(s).  There is always a rule at the root node.
   */
  access_t access;

  /* Minimal access rights that the user has on this or any other node in 
   * the sub-tree. */
  svn_repos_authz_access_t min_rights;

  /* Maximal access rights that the user has on this or any other node in 
   * the sub-tree. */
  svn_repos_authz_access_t max_rights;

} limited_rights_t;

/* Return TRUE, if RIGHTS has local rights defined in the ACCESS member. */
static svn_boolean_t
has_local_rule(const limited_rights_t *rights)
{
  return rights->access.sequence_number != NO_SEQUENCE_NUMBER;
}

/* Aggregate the ACCESS spec of TARGET and RIGHTS into TARGET.  I.e. if both
 * are specified, pick one in accordance to the precedence rules. */
static void
combine_access(limited_rights_t *target,
               const limited_rights_t *rights)
{
  /* This implies the check for NO_SEQUENCE_NUMBER, i.e no rights being
   * specified. */
  if (target->access.sequence_number < rights->access.sequence_number)
    target->access = rights->access;
}

/* Aggregate the min / max access rights of TARGET and RIGHTS into TARGET. */
static void
combine_right_limits(limited_rights_t *target,
                     const limited_rights_t *rights)
{
  target->max_rights |= rights->max_rights;
  target->min_rights &= rights->min_rights;
}


/*** Constructing the prefix tree. ***/

/* Substructure of node_t.  It contains all sub-node that use patterns
 * in the next segment level. We keep it separate to save a bit of memory
 * and to be able to check for pattern presence in a single operation.
 */
typedef struct node_pattern_t
{
  /* If not NULL, this represents the "*" follow-segment. */
  struct node_t *any;

  /* If not NULL, this represents the "**" follow-segment. */
  struct node_t *any_var;

  /* If not NULL, the segments of all nodes_t* in this array are the prefix
   * part of "prefix*" patterns.  Sorted by segment prefix. */
  apr_array_header_t *prefixes;

  /* If not NULL, the segments of all nodes_t* in this array are the
   * reversed suffix part of "*suffix" patterns.  Sorted by reversed
   * segment suffix. */
  apr_array_header_t *suffixes;

  /* If not NULL, the segments of all nodes_t* in this array contain
   * wildcards and don't fit into any of the above categories. */
  apr_array_header_t *complex;

  /* This node itself is a "**" segment and must therefore itself be added
   * to the matching node list for the next level. */
  svn_boolean_t repeat;
} node_pattern_t;

/* The pattern tree.  All relevant path rules are being folded into this
 * prefix tree, with a single, whole segment stored at each node.  The whole
 * tree applies to a single user only.
 */
typedef struct node_t
{
  /* The segment as specified in the path rule.  During the lookup tree walk,
   * this will compared to the respective segment of the path to check. */
  svn_string_t segment;

  /* Immediate access rights granted by rules on this node and the min /
   * max rights on any path in this sub-tree. */
  limited_rights_t rights;

  /* Map of sub-segment(const char *) to respective node (node_t) for all
   * sub-segments that have rules on themselves or their respective subtrees.
   * NULL, if there are no rules for sub-paths relevant to the user. */
  apr_hash_t *sub_nodes;

  /* If not NULL, this contains the pattern-based segment sub-nodes. */
  node_pattern_t *pattern_sub_nodes;
} node_t;

/* Create a new tree node for SEGMENT.
   Note: SEGMENT->pattern is always interned and therefore does not
   have to be copied into the result pool. */
static node_t *
create_node(authz_rule_segment_t *segment,
            apr_pool_t *result_pool)
{
  node_t *result = apr_pcalloc(result_pool, sizeof(*result));
  if (segment)
    result->segment = segment->pattern;
  else
    {
      result->segment.data = "";
      result->segment.len = 0;
    }
  result->rights.access.sequence_number = NO_SEQUENCE_NUMBER;
  return result;
}

/* Auto-create a node in *NODE, make it apply to SEGMENT and return it. */
static node_t *
ensure_node(node_t **node,
            authz_rule_segment_t *segment,
            apr_pool_t *result_pool)
{
  if (!*node)
    *node = create_node(segment, result_pool);

  return *node;
}

/* compare_func comparing segment names. It takes a node_t** as VOID_LHS
 * and a const authz_rule_segment_t * as VOID_RHS.
 */
static int
compare_node_rule_segment(const void *void_lhs,
                          const void *void_rhs)
{
  const node_t *node = *(const node_t **)void_lhs;
  const authz_rule_segment_t *segment = void_rhs;

  return strcmp(node->segment.data, segment->pattern.data);
}

/* compare_func comparing segment names. It takes a node_t** as VOID_LHS
 * and a const char * as VOID_RHS.
 */
static int
compare_node_path_segment(const void *void_lhs,
                          const void *void_rhs)
{
  const node_t *node = *(const node_t **)void_lhs;
  const char *segment = void_rhs;

  return strcmp(node->segment.data, segment);
}

/* Make sure a node_t* for SEGMENT exists in *ARRAY and return it.
 * Auto-create either if they don't exist.  Entries in *ARRAY are
 * sorted by their segment strings.
 */
static node_t *
ensure_node_in_array(apr_array_header_t **array,
                     authz_rule_segment_t *segment,
                     apr_pool_t *result_pool)
{
  int idx;
  node_t *node;
  node_t **node_ref;

  /* Auto-create the array. */
  if (!*array)
    *array = apr_array_make(result_pool, 4, sizeof(node_t *));

  /* Find the node in ARRAY and the IDX at which it were to be inserted.
   * Initialize IDX such that we won't attempt a hinted lookup (likely
   * to fail and therefore pure overhead). */
  idx = (*array)->nelts;
  node_ref = svn_sort__array_lookup(*array, segment, &idx,
                                    compare_node_rule_segment);
  if (node_ref)
    return *node_ref;

  /* There is no such node, yet.
   * Create one and insert it into the sorted array. */
  node = create_node(segment, result_pool);
  svn_sort__array_insert(*array, &node, idx);

  return node;
}

/* Auto-create the PATTERN_SUB_NODES sub-structure in *NODE and return it. */
static node_pattern_t *
ensure_pattern_sub_nodes(node_t *node,
                         apr_pool_t *result_pool)
{
  if (node->pattern_sub_nodes == NULL)
    node->pattern_sub_nodes = apr_pcalloc(result_pool,
                                          sizeof(*node->pattern_sub_nodes));

  return node->pattern_sub_nodes;
}

/* Combine an ACL rule segment with the corresponding node in our filtered
 * data model. */
typedef struct node_segment_pair_t
{
  authz_rule_segment_t *segment;
  node_t *node;
} node_segment_pair_t;

/* Context object to be used with process_acl. It allows us to re-use
 * information from previous insertions. */
typedef struct construction_context_t
{
  /* Array of node_segment_pair_t.  It contains all segments already
   * processed of the current insertion together with the respective
   * nodes in our filtered tree.  Before the next lookup, the tree
   * walk for the common prefix can be skipped. */
  apr_array_header_t *path;
} construction_context_t;

/* Return a new context object allocated in RESULT_POOL. */
static construction_context_t *
create_construction_context(apr_pool_t *result_pool)
{
  construction_context_t *result = apr_pcalloc(result_pool, sizeof(*result));

  /* Array will be auto-extended but this initial size will make it rarely
   * ever necessary. */
  result->path = apr_array_make(result_pool, 32, sizeof(node_segment_pair_t));

  return result;
}

/* Constructor utility:  Below NODE, recursively insert sub-nodes for the
 * path given as *SEGMENTS of length SEGMENT_COUNT. If matching nodes
 * already exist, use those instead of creating new ones.  Set the leave
 * node's access rights spec to ACCESS.  Update the conext info in CTX.
 */
static void
insert_path(construction_context_t *ctx,
            node_t *node,
            access_t *access,
            int segment_count,
            authz_rule_segment_t *segment,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  node_t *sub_node;
  node_segment_pair_t *node_segment;

  /* End of path? */
  if (segment_count == 0)
    {
      /* Set access rights.  Since we call this function once per authz
       * config file section, there cannot be multiple paths having the
       * same leave node.  Hence, access gets never overwritten.
       */
      SVN_ERR_ASSERT_NO_RETURN(!has_local_rule(&node->rights));
      node->rights.access = *access;
      return;
    }

  /* Any wildcards?  They will go into a separate sub-structure. */
  if (segment->kind != authz_rule_literal)
    ensure_pattern_sub_nodes(node, result_pool);

  switch (segment->kind)
    {
      /* A full wildcard segment? */
    case authz_rule_any_segment:
      sub_node = ensure_node(&node->pattern_sub_nodes->any,
                             segment, result_pool);
      break;

      /* One or more full wildcard segments? */
    case authz_rule_any_recursive:
      sub_node = ensure_node(&node->pattern_sub_nodes->any_var,
                             segment, result_pool);
      ensure_pattern_sub_nodes(sub_node, result_pool)->repeat = TRUE;
      break;

      /* A single wildcard at the end of the segment? */
    case authz_rule_prefix:
      sub_node = ensure_node_in_array(&node->pattern_sub_nodes->prefixes,
                                      segment, result_pool);
      break;

      /* A single wildcard at the start of segments? */
    case authz_rule_suffix:
      sub_node = ensure_node_in_array(&node->pattern_sub_nodes->suffixes,
                                      segment, result_pool);
      break;

      /* General pattern? */
    case authz_rule_fnmatch:
      sub_node = ensure_node_in_array(&node->pattern_sub_nodes->complex,
                                      segment, result_pool);
      break;

      /* Then it must be a literal. */
    default:
      SVN_ERR_ASSERT_NO_RETURN(segment->kind == authz_rule_literal);

      if (!node->sub_nodes)
        {
          node->sub_nodes = svn_hash__make(result_pool);
          sub_node = NULL;
        }
      else
        {
          sub_node = svn_hash_gets(node->sub_nodes, segment->pattern.data);
        }

      /* Auto-insert a sub-node for the current segment. */
      if (!sub_node)
        {
          sub_node = create_node(segment, result_pool);
          apr_hash_set(node->sub_nodes,
                       sub_node->segment.data,
                       sub_node->segment.len,
                       sub_node);
        }
    }

  /* Update context. */
  node_segment = apr_array_push(ctx->path);
  node_segment->segment = segment;
  node_segment->node = sub_node;

  /* Continue at the sub-node with the next segment. */
  insert_path(ctx, sub_node, access, segment_count - 1, segment + 1,
              result_pool, scratch_pool);
}


/* If the ACL is relevant to the REPOSITORY and user (given as MEMBERSHIPS
 * plus ANONYMOUS flag), insert the respective nodes into tree starting
 * at ROOT.  Use the context info of the previous call in CTX to eliminate
 * repeated lookups.  Allocate new nodes in RESULT_POOL and use SCRATCH_POOL
 * for temporary allocations.
 */
static void
process_acl(construction_context_t *ctx,
            const authz_acl_t *acl,
            node_t *root,
            const char *repository,
            const char *user,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  access_t access;
  int i;
  node_t *node;

  /* Skip ACLs that don't say anything about the current user
     and/or repository. */
  if (!svn_authz__acl_get_access(&access.rights, acl, user, repository))
    return;

  /* Insert the rule into the filtered tree. */
  access.sequence_number = acl->sequence_number;

  /* Try to reuse results from previous runs.
   * Basically, skip the commen prefix. */
  node = root;
  for (i = 0; i < ctx->path->nelts; ++i)
    {
      const node_segment_pair_t *step
        = &APR_ARRAY_IDX(ctx->path, i, const node_segment_pair_t);

      /* Exploit the fact that all strings in the authz model are unique /
       * internized and can be identified by address alone. */
      if (   !step->node
          || i >= acl->rule.len
          || step->segment->kind != acl->rule.path[i].kind
          || step->segment->pattern.data != acl->rule.path[i].pattern.data)
        {
          ctx->path->nelts = i;
          break;
        }
      else
        {
          node = step->node;
        }
    }

  /* Insert the path rule into the filtered tree. */
  insert_path(ctx, node, &access,
              acl->rule.len - i, acl->rule.path + i,
              result_pool, scratch_pool);
}

/* Forward declaration ... */
static void
finalize_up_tree(node_t *parent,
                 access_t *inherited_access,
                 node_t *node,
                 apr_pool_t *scratch_pool);

/* Call finalize_up_tree() on all elements in the ARRAY of node_t *.
 * ARRAY may be NULL.
 */
static void
finalize_up_subnode_array(node_t *parent,
                          access_t *inherited_access,
                          apr_array_header_t *array,
                          apr_pool_t *scratch_pool)
{
  if (array)
    {
      int i;
      for (i = 0; i < array->nelts; ++i)
        finalize_up_tree(parent, inherited_access,
                         APR_ARRAY_IDX(array, i, node_t *), scratch_pool);
    }
}

/* Bottomp-up phase of the recursive update / finalization of the tree
 * node properties for NODE immediately below PARENT.  The access rights
 * inherited from the parent path are given in INHERITED_ACCESS.  None of
 * the pointers may be NULL. The tree root node may be used as its own
 * parent.
 */
static void
finalize_up_tree(node_t *parent,
                 access_t *inherited_access,
                 node_t *node,
                 apr_pool_t *scratch_pool)
{
  /* Access rights at NODE. */
  access_t *access = has_local_rule(&node->rights)
                   ? &node->rights.access
                   : inherited_access;

  /* So far, min and max rights at NODE are the immediate access rights. */
  node->rights.min_rights = access->rights;
  node->rights.max_rights = access->rights;

  /* Combine that information with sub-tree data. */
  if (node->sub_nodes)
    {
      apr_hash_index_t *hi;
      for (hi = apr_hash_first(scratch_pool, node->sub_nodes);
           hi;
           hi = apr_hash_next(hi))
        finalize_up_tree(node, access, apr_hash_this_val(hi),
                         scratch_pool);
    }

  /* Do the same thing for all other sub-nodes as well. */
  if (node->pattern_sub_nodes)
    {
      if (node->pattern_sub_nodes->any)
        finalize_up_tree(node, access, node->pattern_sub_nodes->any,
                         scratch_pool);
      if (node->pattern_sub_nodes->any_var)
        finalize_up_tree(node, access, node->pattern_sub_nodes->any_var,
                         scratch_pool);

      finalize_up_subnode_array(node, access,
                                node->pattern_sub_nodes->prefixes,
                                scratch_pool);
      finalize_up_subnode_array(node, access,
                                node->pattern_sub_nodes->suffixes,
                                scratch_pool);
      finalize_up_subnode_array(node, access,
                                node->pattern_sub_nodes->complex,
                                scratch_pool);
    }

  /* Add our min / max info to the parent's info.
   * Idempotent for parent == node (happens at root). */
  combine_right_limits(&parent->rights, &node->rights);
}

/* Forward declaration ... */
static void
finalize_down_tree(node_t *node,
                   limited_rights_t rights,
                   apr_pool_t *scratch_pool);

/* Call finalize_down_tree() on all elements in the ARRAY of node_t *.
 * ARRAY may be NULL.
 */
static void
finalize_down_subnode_array(apr_array_header_t *array,
                            const limited_rights_t *rights,
                            apr_pool_t *scratch_pool)
{
  if (array)
    {
      int i;
      for (i = 0; i < array->nelts; ++i)
        finalize_down_tree(APR_ARRAY_IDX(array, i, node_t *),
                           *rights, scratch_pool);
    }
}

/* Top-down phase of the recursive update / finalization of the tree
 * node properties for NODE.  The min / max access rights of all var-
 * segment rules that apply to the sub-tree of NODE are given in RIGHTS.
 */
static void
finalize_down_tree(node_t *node,
                   limited_rights_t rights,
                   apr_pool_t *scratch_pool)
{
  /* Update the NODE's right limits. */
  combine_right_limits(&node->rights, &rights);

  /* If there are more var-segment rules, aggregate their rights as all
   * these rules are implictly repeated on all sub-nodes. */
  if (node->pattern_sub_nodes && node->pattern_sub_nodes->any_var)
    combine_right_limits(&rights, &node->pattern_sub_nodes->any_var->rights);

  /* Resurse into the sub-nodes. */
  if (node->sub_nodes)
    {
      apr_hash_index_t *hi;
      for (hi = apr_hash_first(scratch_pool, node->sub_nodes);
           hi;
           hi = apr_hash_next(hi))
        finalize_down_tree(apr_hash_this_val(hi), rights, scratch_pool);
    }

  if (node->pattern_sub_nodes)
    {
      if (node->pattern_sub_nodes->any)
        finalize_down_tree(node->pattern_sub_nodes->any, rights,
                           scratch_pool);
      if (node->pattern_sub_nodes->any_var)
        finalize_down_tree(node->pattern_sub_nodes->any_var, rights,
                           scratch_pool);

      finalize_down_subnode_array(node->pattern_sub_nodes->prefixes,
                                  &rights, scratch_pool);
      finalize_down_subnode_array(node->pattern_sub_nodes->suffixes,
                                  &rights, scratch_pool);
      finalize_down_subnode_array(node->pattern_sub_nodes->complex,
                                  &rights, scratch_pool);
    }
}

/* From the authz CONFIG, extract the parts relevant to USER and REPOSITORY.
 * Return the filtered rule tree.
 */
static node_t *
create_user_authz(svn_authz_t *authz,
                  const char *repository,
                  const char *user,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  int i;
  limited_rights_t var_rights;
  node_t *root = create_node(NULL, result_pool);
  construction_context_t *ctx = create_construction_context(scratch_pool);

  /* Use a separate sub-pool to keep memory usage tight. */
  apr_pool_t *subpool = svn_pool_create(scratch_pool);

  /* Filtering and tree construction. */
  for (i = 0; i < authz->acls->nelts; ++i)
    process_acl(ctx, &APR_ARRAY_IDX(authz->acls, i, authz_acl_t),
                root, repository, user, result_pool, subpool);

  /* If there is no relevant rule at the root node, the "no access" default
   * applies. Give it a SEQUENCE_NUMBER that will never overrule others. */
  if (!has_local_rule(&root->rights))
    {
      root->rights.access.sequence_number = 0;
      root->rights.access.rights = svn_authz_none;
    }

  /* Calculate recursive rights etc. */
  svn_pool_clear(subpool);
  finalize_up_tree(root, &root->rights.access, root, subpool);

  svn_pool_clear(subpool);
  var_rights.max_rights = svn_authz_none;
  var_rights.min_rights = svn_authz_read | svn_authz_write;
  finalize_down_tree(root, var_rights, subpool);

  /* Done. */
  svn_pool_destroy(subpool);
  return root;
}


/*** Lookup. ***/

/* Reusable lookup state object. It is easy to pass to functions and
 * recycling it between lookups saves significant setup costs. */
typedef struct lookup_state_t
{
  /* Rights immediately applying to this node and limits to the rights to
   * any sub-path. */
  limited_rights_t rights;

  /* Nodes applying to the path followed so far. */
  apr_array_header_t *current;

  /* Temporary array containing the nodes applying to the next path
   * segment (used to build up the next contents of CURRENT). */
  apr_array_header_t *next;

  /* Scratch pad for path operations. */
  svn_stringbuf_t *scratch_pad;

  /* After each lookup iteration, CURRENT and PARENT_RIGHTS will
   * apply to this path. */
  svn_stringbuf_t *parent_path;

  /* Rights that apply at PARENT_PATH, if PARENT_PATH is not empty. */
  limited_rights_t parent_rights;

} lookup_state_t;

/* Constructor for lookup_state_t. */
static lookup_state_t *
create_lookup_state(apr_pool_t *result_pool)
{
  lookup_state_t *state = apr_pcalloc(result_pool, sizeof(*state));
 
  state->next = apr_array_make(result_pool, 4, sizeof(node_t *));
  state->current = apr_array_make(result_pool, 4, sizeof(node_t *));

  /* Virtually all path segments should fit into this buffer.  If they
   * don't, the buffer gets automatically reallocated.
   *
   * Using a smaller initial size would be fine as well but does not
   * buy us much for the increased risk of being expanded anyway - at
   * some extra cost. */
  state->scratch_pad = svn_stringbuf_create_ensure(200, result_pool);

  /* Most paths should fit into this buffer.  The same rationale as
   * above applies. */
  state->parent_path = svn_stringbuf_create_ensure(200, result_pool);

  return state;
}

/* Clear the current contents of STATE and re-initialize it for ROOT.
 * Check whether we can reuse a previous parent path lookup to shorten
 * the current PATH walk.  Return the full or remaining portion of
 * PATH, respectively.  PATH must not be NULL. */
static const char *
init_lockup_state(lookup_state_t *state,
                  node_t *root,
                  const char *path)
{
  apr_size_t len = strlen(path);
  if (   (len > state->parent_path->len)
      && state->parent_path->len
      && (path[state->parent_path->len] == '/')
      && !memcmp(path, state->parent_path->data, state->parent_path->len))
    {
      /* The PARENT_PATH of the previous lookup is actually a parent path
       * of PATH.  The CURRENT node list already matches the parent path
       * and we only have to set the correct rights info. */
      state->rights = state->parent_rights;

      /* Tell the caller where to proceed. */
      return path + state->parent_path->len;
    }

  /* Start lookup at ROOT for the full PATH. */
  state->rights = root->rights;
  state->parent_rights = root->rights;

  apr_array_clear(state->next);
  apr_array_clear(state->current);
  APR_ARRAY_PUSH(state->current, node_t *) = root;

  /* Var-segment rules match empty segments as well */
  if (root->pattern_sub_nodes && root->pattern_sub_nodes->any_var)
   {
      node_t *node = root->pattern_sub_nodes->any_var;

      /* This is non-recursive due to ACL normalization. */
      combine_access(&state->rights, &node->rights);
      combine_right_limits(&state->rights, &node->rights);
      APR_ARRAY_PUSH(state->current, node_t *) = node;
   }

  svn_stringbuf_setempty(state->parent_path);
  svn_stringbuf_setempty(state->scratch_pad);

  return path;
}

/* Add NODE to the list of NEXT nodes in STATE.  NODE may be NULL in which
 * case this is a no-op.  Also update and aggregate the access rights data
 * for the next path segment.
 */
static void
add_next_node(lookup_state_t *state,
              node_t *node)
{
  /* Allowing NULL nodes simplifies the caller. */
  if (node)
    {
      /* The rule with the highest sequence number is the one that applies.
       * Not all nodes that we are following have rules that apply directly
       * to this path but only some deep sub-node. */
      combine_access(&state->rights, &node->rights);

      /* The rule tree node can be seen as an overlay of all the nodes that
       * we are following.  Any of them _may_ match eventually, so the min/
       * max possible access rights are a combination of all these sub-trees.
       */
      combine_right_limits(&state->rights, &node->rights);

      /* NODE is now enlisted as a (potential) match for the next segment. */
      APR_ARRAY_PUSH(state->next, node_t *) = node;

      /* Variable length sub-segment sequences apply to the same node due
       * to matching empty sequences as well. */
      if (node->pattern_sub_nodes && node->pattern_sub_nodes->any_var)
        {
          node = node->pattern_sub_nodes->any_var;

          /* This is non-recursive due to ACL normalization. */
          combine_access(&state->rights, &node->rights);
          combine_right_limits(&state->rights, &node->rights);
          APR_ARRAY_PUSH(state->next, node_t *) = node;
        }
    }
}

/* Scan the PREFIXES array of node_t* for all entries whose SEGMENT members
 * are prefixes of SEGMENT.  Add these to STATE for the next tree level. */
static void
add_prefix_matches(lookup_state_t *state,
                   const svn_stringbuf_t *segment,
                   apr_array_header_t *prefixes)
{
  /* Index of the first node that might be a match.  All matches will
   * be at this and the immediately following indexes. */
  int i = svn_sort__bsearch_lower_bound(prefixes, segment->data,
                                        compare_node_path_segment);
  for (; i < prefixes->nelts; ++i)
    {
      node_t *node = APR_ARRAY_IDX(prefixes, i, node_t *);

      /* The first mismatch will mean no more matches will follow. */
      if (node->segment.len > segment->len)
        return;

      if (memcmp(node->segment.data, segment->data, node->segment.len))
        return;

      add_next_node(state, node);
    }
}

/* Scan the PATTERNS array of node_t* for all entries whose SEGMENT members
 * (usually containing wildcards) match SEGMENT.  Add these to STATE for the
 * next tree level. */
static void
add_complex_matches(lookup_state_t *state,
                    const svn_stringbuf_t *segment,
                    apr_array_header_t *patterns)
{
  int i;
  for (i = 0; i < patterns->nelts; ++i)
    {
      node_t *node = APR_ARRAY_IDX(patterns, i, node_t *);
      if (0 == apr_fnmatch(node->segment.data, segment->data, 0))
        add_next_node(state, node);
    }
}

/* Extract the next segment from PATH and copy it into SEGMENT, whose current
 * contents get overwritten.  Empty paths ("") are supported and leading '/'
 * segment separators will be interpreted as an empty segment ("").  Non-
 * normalizes parts, i.e. sequences of '/', will be treated as a single '/'.
 *
 * Return the start of the next segment within PATH, skipping the '/'
 * separator(s).  Return NULL, if there are no further segments.
 *
 * The caller (only called by lookup(), ATM) must ensure that SEGMENT has
 * enough room to store all of PATH.
 */
static const char *
next_segment(svn_stringbuf_t *segment,
             const char *path)
{
  apr_size_t len;
  char c;

  /* Read and scan PATH for NUL and '/' -- whichever comes first. */
  for (len = 0, c = *path; c; c = path[++len])
    if (c == '/')
      {
        /* End of segment. */
        segment->data[len] = 0;
        segment->len = len;

        /* If PATH is not normalized, this is where we skip whole sequences
         * of separators. */
        while (path[++len] == '/')
          ;

        /* Continue behind the last separator in the sequence.  We will
         * treat trailing '/' as indicating an empty trailing segment.
         * Therefore, we never have to return NULL here. */
        return path + len;
      }
    else
      {
        /* Copy segment contents directly into the result buffer.
         * On many architectures, this is almost or entirely for free. */
        segment->data[len] = c;
      }

  /* No separator found, so all of PATH has been the last segment. */
  segment->data[len] = 0;
  segment->len = len;

  /* Tell the caller that this has been the last segment. */
  return NULL;
}

/* Starting at the respective user's authz root node provided with STATE,
 * follow PATH and return TRUE, iff the REQUIRED access has been granted to
 * that user for this PATH.  REQUIRED must not contain svn_authz_recursive.
 * If RECURSIVE is set, all paths in the sub-tree at and below PATH must
 * have REQUIRED access.  PATH does not need to be normalized, may be empty
 * but must not be NULL.
 */
static svn_boolean_t
lookup(lookup_state_t *state,
       const char *path,
       svn_repos_authz_access_t required,
       svn_boolean_t recursive,
       apr_pool_t *scratch_pool)
{
  /* Create a scratch pad large enough to hold any of PATH's segments. */
  apr_size_t path_len = strlen(path);
  svn_stringbuf_ensure(state->scratch_pad, path_len);

  /* Normalize start and end of PATH.  Most paths will be fully normalized,
   * so keep the overhead as low as possible. */
  if (path_len && path[path_len-1] == '/')
    {
      do
      {
        --path_len;
      }
      while (path_len && path[path_len-1] == '/');
      path = apr_pstrmemdup(scratch_pool, path, path_len);
    }

  while (path[0] == '/')
    ++path;     /* Don't update PATH_LEN as we won't need it anymore. */

  /* Actually walk the path rule tree following PATH until we run out of
   * either tree or PATH. */
  while (state->current->nelts && path)
    {
      apr_array_header_t *temp;
      int i;
      svn_stringbuf_t *segment = state->scratch_pad;

      /* Shortcut 1: We could nowhere find enough rights in this sub-tree. */
      if ((state->rights.max_rights & required) != required)
        return FALSE;

      /* Shortcut 2: We will find enough rights everywhere in this sub-tree. */
      if ((state->rights.min_rights & required) == required)
        return TRUE;

      /* Extract the next segment. */
      path = next_segment(segment, path);

      /* Initial state for this segment. */
      apr_array_clear(state->next);
      state->rights.access.sequence_number = NO_SEQUENCE_NUMBER;
      state->rights.access.rights = svn_authz_none;

      /* These init values ensure that the first node's value will be used
       * when combined with them.  If there is no first node,
       * state->access.sequence_number remains unchanged and we will use
       * the parent's (i.e. inherited) access rights. */
      state->rights.min_rights = svn_authz_read | svn_authz_write;
      state->rights.max_rights = svn_authz_none;

      /* Update the PARENT_PATH member in STATE to match the nodes in
       * CURRENT at the end of this iteration, i.e. if and when NEXT
       * has become CURRENT. */
      if (path)
        {
          svn_stringbuf_appendbyte(state->parent_path, '/');
          svn_stringbuf_appendbytes(state->parent_path, segment->data,
                                    segment->len);
        }

      /* Scan follow all alternative routes to the next level. */
      for (i = 0; i < state->current->nelts; ++i)
        {
          node_t *node = APR_ARRAY_IDX(state->current, i, node_t *);
          if (node->sub_nodes)
            add_next_node(state, apr_hash_get(node->sub_nodes, segment->data,
                                              segment->len));

          /* Process alternative, wildcard-based sub-nodes. */
          if (node->pattern_sub_nodes)
            {
              add_next_node(state, node->pattern_sub_nodes->any);

              /* If the current node represents a "**" pattern, it matches
               * to all levels. So, add it to the list for the NEXT level. */
              if (node->pattern_sub_nodes->repeat)
                add_next_node(state, node);

              /* Find all prefix pattern matches. */
              if (node->pattern_sub_nodes->prefixes)
                add_prefix_matches(state, segment,
                                   node->pattern_sub_nodes->prefixes);

              if (node->pattern_sub_nodes->complex)
                add_complex_matches(state, segment,
                                    node->pattern_sub_nodes->complex);

              /* Find all suffux pattern matches.
               * This must be the last check as it destroys SEGMENT. */
              if (node->pattern_sub_nodes->suffixes)
                {
                  /* Suffixes behave like reversed prefixes. */
                  svn_authz__reverse_string(segment->data, segment->len);
                  add_prefix_matches(state, segment,
                                     node->pattern_sub_nodes->suffixes);
                }
            }
        }

      /* If no rule applied to this SEGMENT directly, the parent rights
       * will apply to at least the SEGMENT node itself and possibly
       * other parts deeper in it's subtree. */
      if (!has_local_rule(&state->rights))
        {
          state->rights.access = state->parent_rights.access;
          state->rights.min_rights &= state->parent_rights.access.rights;
          state->rights.max_rights |= state->parent_rights.access.rights;
        }

      /* The list of nodes for SEGMENT is now complete.  If we need to
       * continue, make it the current and put the old one into the recycler.
       *
       * If this is the end of the path, keep the parent path and rights in
       * STATE as are such that sibbling lookups will benefit from it.
       */
      if (path)
        {
          temp = state->current;
          state->current = state->next;
          state->next = temp;

		  /* In STATE, PARENT_PATH, PARENT_RIGHTS and CURRENT are now in sync. */
          state->parent_rights = state->rights;
        }
    }

  /* If we check recursively, none of the (potential) sub-paths must have
   * less than the REQUIRED access rights.  "Potential" because we don't
   * verify that the respective paths actually exist in the repository.
   */
  if (recursive)
    return (state->rights.min_rights & required) == required;

  /* Return whether the access rights on PATH fully include REQUIRED. */
  return (state->rights.access.rights & required) == required;
}



/*** The authz data structure. ***/

/* An entry in svn_authz_t's USER_RULES cache.  All members must be
 * allocated in the POOL and the latter has to be cleared / destroyed
 * before overwriting the entries' contents.
 */
struct authz_user_rules_t
{
  /* User name for which we filtered the rules.
   * User NULL for the anonymous user. */
  const char *user;

  /* Repository name for which we filtered the rules.
   * May be empty but never NULL for used entries. */
  const char *repository;

  /* Root of the filtered path rule tree.  NULL for unused entries. */
  node_t *root;

  /* Reusable lookup state instance. */
  lookup_state_t *lookup_state;

  /* Pool from which all data within this struct got allocated.
   * Can be destroyed or cleaned up with no further side-effects. */
  apr_pool_t *pool;
};


/* Retrieve the file at DIRENT (contained in a repo) and return its contents
 * as *STREAM allocated in RESULT_POOL.
 *
 * If MUST_EXIST is TRUE, a missing authz file is also an error, otherwise
 * an empty stream is returned.
 *
 * SCRATCH_POOL will be used for temporary allocations.
 */
static svn_error_t *
authz_retrieve_config_repo(svn_stream_t **stream,
                           const char *dirent,
                           svn_boolean_t must_exist,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_repos_t *repos;
  const char *repos_root_dirent;
  const char *fs_path;
  svn_fs_t *fs;
  svn_fs_root_t *root;
  svn_revnum_t youngest_rev;
  svn_node_kind_t node_kind;

  /* Search for a repository in the full path. */
  repos_root_dirent = svn_repos_find_root_path(dirent, scratch_pool);
  if (!repos_root_dirent)
    return svn_error_createf(SVN_ERR_RA_LOCAL_REPOS_NOT_FOUND, NULL,
                             "Unable to find repository at '%s'", dirent);

  /* Attempt to open a repository at repos_root_dirent. */
  SVN_ERR(svn_repos_open3(&repos, repos_root_dirent, NULL, result_pool,
                          scratch_pool));

  fs_path = &dirent[strlen(repos_root_dirent)];

  /* Root path is always a directory so no reason to go any further */
  if (*fs_path == '\0')
    return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                             "'/' is not a file in repo '%s'",
                             repos_root_dirent);

  /* We skip some things that are non-important for how we're going to use
   * this repo connection.  We do not set any capabilities since none of
   * the current ones are important for what we're doing.  We also do not
   * setup the environment that repos hooks would run under since we won't
   * be triggering any. */

  /* Get the filesystem. */
  fs = svn_repos_fs(repos);

  /* Find HEAD and the revision root */
  SVN_ERR(svn_fs_youngest_rev(&youngest_rev, fs, scratch_pool));
  SVN_ERR(svn_fs_revision_root(&root, fs, youngest_rev, result_pool));

  SVN_ERR(svn_fs_check_path(&node_kind, root, fs_path, scratch_pool));
  if (node_kind == svn_node_none)
    {
      if (!must_exist)
        {
          *stream = svn_stream_empty(result_pool);
          return SVN_NO_ERROR;
        }
      else
        {
          return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                   "'%s' path not found in repo '%s'", fs_path,
                                   repos_root_dirent);
        }
    }
  else if (node_kind != svn_node_file)
    {
      return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                               "'%s' is not a file in repo '%s'", fs_path,
                               repos_root_dirent);
    }

  SVN_ERR(svn_fs_file_contents(stream, root, fs_path, result_pool));
  return SVN_NO_ERROR;
}

/* Retrieve the file at PATH and return its contents as *STREAM allocated in
 * RESULT_POOL.
 *
 * If MUST_EXIST is TRUE, a missing authz file is also an error, otherwise
 * an empty stream is returned.
 *
 * SCRATCH_POOL will be used for temporary allocations.
 */
static svn_error_t *
authz_retrieve_config_file(svn_stream_t **stream,
                           const char *path,
                           svn_boolean_t must_exist,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_node_kind_t node_kind;
  apr_file_t *file;

  SVN_ERR(svn_io_check_path(path, &node_kind, scratch_pool));
  if (node_kind == svn_node_none)
    {
      if (!must_exist)
        {
          *stream = svn_stream_empty(result_pool);
          return SVN_NO_ERROR;
        }
      else
        {
          return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                   "'%s' path not found", path);
        }
    }
  else if (node_kind != svn_node_file)
    {
      return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                               "'%s' is not a file", path);
    }

  SVN_ERR(svn_io_file_open(&file, path, APR_READ | APR_BUFFERED,
                           APR_OS_DEFAULT, result_pool));
  *stream = svn_stream_from_aprfile2(file, FALSE, result_pool);

  return SVN_NO_ERROR;
}

/* Look through AUTHZ's cache for a path rule tree already filtered for
 * this USER, REPOS_NAME combination.  If that does not exist, yet, create
 * one and return the fully initialized authz_user_rules_t to start lookup
 * at *PATH.
 *
 * If *PATH is not NULL, *PATH may be reduced to the sub-path that has 
 * still to be walked leveraging exisiting parent info from previous runs.
 * If *PATH is NULL, keep the lockup_state member as is - assuming the
 * caller will not use it but only the root node data.
 */
static authz_user_rules_t *
get_filtered_tree(svn_authz_t *authz,
                  const char *repos_name,
                  const char **path,
                  const char *user,
                  apr_pool_t *scratch_pool)
{
  apr_pool_t *pool;
  apr_size_t i;

  /* Search our cache for a suitable previously filtered tree. */
  for (i = 0; i < AUTHZ_FILTERED_CACHE_SIZE && authz->user_rules[i]; ++i)
    {
      /* Does the user match? */
      if (user == NULL)
        {
          if (authz->user_rules[i]->user != NULL)
            continue;
        }
      else
        {
          if (   authz->user_rules[i]->user == NULL
              || strcmp(user, authz->user_rules[i]->user))
            continue;
        }

      /* Does the repository match as well? */
      if (strcmp(repos_name, authz->user_rules[i]->repository))
        continue;

      /* LRU: Move up to first entry. */
      if (i > 0)
        {
          authz_user_rules_t *temp = authz->user_rules[i];
          memmove(&authz->user_rules[1], &authz->user_rules[0],
                  i * sizeof(temp));
          authz->user_rules[0] = temp;
        }

      if (*path)
        *path = init_lockup_state(authz->user_rules[0]->lookup_state,
                                  authz->user_rules[0]->root,
                                  *path);

      return authz->user_rules[0];
    }

  /* Cache full? Drop last (i.e. oldest) entry. */
  if (i == AUTHZ_FILTERED_CACHE_SIZE)
    svn_pool_destroy(authz->user_rules[--i]->pool);

  /* Write a new entry. */
  pool = svn_pool_create(authz->pool);
  authz->user_rules[i] = apr_palloc(pool, sizeof(*authz->user_rules[i]));
  authz->user_rules[i]->pool = pool;
  authz->user_rules[i]->repository = apr_pstrdup(pool, repos_name);
  authz->user_rules[i]->user = user ? apr_pstrdup(pool, user) : NULL;
  authz->user_rules[i]->root = create_user_authz(authz,
                                                 repos_name, user, pool,
                                                 scratch_pool);
  authz->user_rules[i]->lookup_state = create_lookup_state(pool);
  if (*path)
    init_lockup_state(authz->user_rules[i]->lookup_state,
                      authz->user_rules[i]->root, *path);

  return authz->user_rules[i];
}


/* Retrieve the file at PATH (local path or repository URL) and return its
 * contents as *STREAM allocated in RESULT_POOL.
 *
 * If MUST_EXIST is TRUE, a missing authz file is also an error, otherwise
 * an empty stream is returned.
 *
 * SCRATCH_POOL will be used for temporary allocations.
 */
static svn_error_t *
retrieve_config(svn_stream_t **stream,
                const char *path,
                svn_boolean_t must_exist,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  if (svn_path_is_url(path))
    {
      const char *dirent;
      SVN_ERR(svn_uri_get_dirent_from_file_url(&dirent, path, scratch_pool));
      SVN_ERR(authz_retrieve_config_repo(stream, dirent, must_exist,
                                         result_pool, scratch_pool));
    }
  else
    {
      /* Outside of repo file or Windows registry*/
      SVN_ERR(authz_retrieve_config_file(stream, path, must_exist,
                                         result_pool, scratch_pool));
    }

  return SVN_NO_ERROR;
}


/*** Private API functions. ***/

svn_error_t *
svn_repos__retrieve_config(svn_config_t **cfg_p,
                           const char *path,
                           svn_boolean_t must_exist,
                           svn_boolean_t case_sensitive,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_stream_t *stream;
  SVN_ERR(retrieve_config(&stream, path, must_exist, scratch_pool,
                          scratch_pool));
  err = svn_config_parse(cfg_p, stream, case_sensitive, case_sensitive,
                         result_pool);

  /* Add the URL to the error stack since the parser doesn't have it. */
  if (err != SVN_NO_ERROR)
    return svn_error_createf(err->apr_err, err,
                             "Error while parsing config file: '%s':",
                             path);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_repos__authz_read(svn_authz_t **authz_p, const char *path,
                      const char *groups_path, svn_boolean_t must_exist,
                      svn_boolean_t accept_urls, apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  svn_stream_t *rules;
  svn_stream_t *groups;
  svn_error_t* err;

  /* Open the main authz file */
  if (accept_urls)
    SVN_ERR(retrieve_config(&rules, path, must_exist, scratch_pool,
                            scratch_pool));
  else
    SVN_ERR(authz_retrieve_config_file(&rules, path, must_exist,
                                       scratch_pool, scratch_pool));

  /* Open the optional groups file */
  if (groups_path)
    {
      if (accept_urls)
        SVN_ERR(retrieve_config(&groups, groups_path, must_exist,
                                scratch_pool, scratch_pool));
      else
        SVN_ERR(authz_retrieve_config_file(&groups, groups_path, must_exist,
                                           scratch_pool, scratch_pool));
    }
  else
    {
      groups = NULL;
    }

  /* Parse the configuration(s) and construct the full authz model from it. */
  err = svn_authz__parse(authz_p, rules, groups, result_pool,
                         scratch_pool);

  /* Add the URL / file name to the error stack since the parser doesn't
   * have it. */
  if (err != SVN_NO_ERROR)
    return svn_error_createf(err->apr_err, err,
                             "Error while parsing config file: '%s':",
                             path);

  return SVN_NO_ERROR;
}



/*** Public functions. ***/

svn_error_t *
svn_repos_authz_read2(svn_authz_t **authz_p, const char *path,
                      const char *groups_path, svn_boolean_t must_exist,
                      apr_pool_t *pool)
{
  apr_pool_t *scratch_pool = svn_pool_create(pool);

  SVN_ERR(svn_repos__authz_read(authz_p, path, groups_path, must_exist,
                                TRUE, pool, scratch_pool));

  svn_pool_destroy(scratch_pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_authz_parse(svn_authz_t **authz_p, svn_stream_t *stream,
                      svn_stream_t *groups_stream, apr_pool_t *pool)
{
  apr_pool_t *scratch_pool = svn_pool_create(pool);

  /* Parse the configuration and construct the full authz model from it. */
  SVN_ERR(svn_authz__parse(authz_p, stream, groups_stream, pool,
                           scratch_pool));

  svn_pool_destroy(scratch_pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_repos_authz_check_access(svn_authz_t *authz, const char *repos_name,
                             const char *path, const char *user,
                             svn_repos_authz_access_t required_access,
                             svn_boolean_t *access_granted,
                             apr_pool_t *pool)
{
  /* Pick or create the suitable pre-filtered path rule tree. */
  authz_user_rules_t *rules = get_filtered_tree(
      authz,
      (repos_name ? repos_name : AUTHZ_ANY_REPOSITORY),
      &path, user, pool);

  /* If PATH is NULL, check if the user has *any* access. */
  if (!path)
    {
      svn_repos_authz_access_t required = required_access & ~svn_authz_recursive;
      *access_granted = (rules->root->rights.max_rights & required) == required;
      return SVN_NO_ERROR;
    }

  /* Sanity check. */
  SVN_ERR_ASSERT(path[0] == '/');

  /* Determine the granted access for the requested path.
   * PATH does not need to be normalized for lockup(). */
  *access_granted = lookup(rules->lookup_state, path,
                           required_access & ~svn_authz_recursive,
                           required_access & svn_authz_recursive, pool);

  return SVN_NO_ERROR;
}
