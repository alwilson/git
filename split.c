/* git-subtree
 *
 * git-subtree-split is slow... rewrite it all in C!
 *   - the bash script needs to fork _very_ often
 *   - rewrote it in libgit2+python and is over 1000x faster
 *
 * Use this opportunity to improve git-subtree?
 *   - tracking info for subtrees like submodules
 *   - make it easy to track remote
 *   - subrepo features? and what would those be?
 *   - remove weird bashisms like '\' escapes in original script
 *
 */

// FIXME How much of this is actually needed?
#include "cache.h"
#include "refs.h"
#include "pkt-line.h"
#include "sideband.h"
#include "tag.h"
#include "object.h"
#include "commit.h"
#include "exec-cmd.h"
#include "diff.h"
#include "revision.h"
#include "list-objects.h"
#include "run-command.h"
#include "connect.h"
#include "sigchain.h"
#include "version.h"
#include "string-list.h"
#include "parse-options.h"
#include "blob.h"

#include "config.h"

#include "hashmap.h"

static struct hashmap revcache;

struct oid2oid {
	struct hashmap_entry ent;
	struct object_id rev;
	struct object_id newrev;
};

static unsigned int oidhash(const struct object_id *oid) {
	return memihash(oid->hash, sizeof(oid->hash));
}

static int oidhash_cmp(const void *unused_cmp_data,
		       const void *entry,
		       const void *entry_or_key,
		       const void *unused_keydata)
{
	const struct oid2oid *a = entry;
	const struct oid2oid *b = entry_or_key;
	return memcmp(&(a->rev), &(b->rev), sizeof(a->rev));
};

static const char * const split_usage[] = {
	N_("git split [<options>] [<path>...]"),
	NULL
};

static void cache_set(const struct object_id *rev,
		      const struct object_id *newrev)
{
	struct oid2oid *e = xmalloc(sizeof(*e));
	hashmap_entry_init(e, oidhash(rev));
	memcpy(&(e->rev),    rev,    sizeof(struct object_id));
	memcpy(&(e->newrev), newrev, sizeof(struct object_id));

	hashmap_add(&revcache, e);
};

static const void *cache_get(const struct object_id *rev) {
	struct oid2oid key;

	hashmap_entry_init(&key, oidhash(rev));
	memcpy(&(key.rev), rev, sizeof(struct object_id));

	//printf("looking up hash: 0x%08x with key %s\n", key.ent.hash, oid_to_hex(rev));

	return hashmap_get(&revcache, &key, NULL);
}

static void setenv_from_commit(struct commit *commit, const char *env, const char *format) {
	struct pretty_print_context pp = {0};
	struct strbuf sb = STRBUF_INIT;
	format_commit_message(commit, format, &sb, &pp);
	setenv(env, sb.buf, 1);
	// DEBUG printf("%s: %s\n", env, sb.buf);
	return;
}

static void copy_commit(struct commit *rev, const char *tree, struct commit_list *parents, char *newrev) {
	struct commit *newrev_commit;
	struct commit_list *p;

	printf("copy_commit {%s} {%s} {", oid_to_hex(&rev->object.oid), sha1_to_hex(tree));
	int more_parents = 0;
	for (p = parents; p; p = p->next) {
		printf("%s-p %s", (more_parents)?" ":"", oid_to_hex(&p->item->object.oid));
		more_parents = 1;
	}
	printf("}\n");

	// FIXME - This mimics the bash script version, but we shouldn't have to use env vars
	setenv_from_commit(rev, "GIT_AUTHOR_NAME",  "%an");
	setenv_from_commit(rev, "GIT_AUTHOR_EMAIL", "%ae");
	setenv_from_commit(rev, "GIT_AUTHOR_DATE",  "%aD");

	setenv_from_commit(rev, "GIT_COMMITTER_NAME",  "%cn");
	setenv_from_commit(rev, "GIT_COMMITTER_EMAIL", "%ce");
	setenv_from_commit(rev, "GIT_COMMITTER_DATE",  "%cD");

	struct strbuf sb = STRBUF_INIT;
	struct pretty_print_context pp = {0};
	format_commit_message(rev, "%B", &sb, &pp);
	// printf("BODY: \"%s\"\n", sb.buf);
	const char *msg = sb.buf;
	const size_t msg_len = strlen(msg);

	int result = commit_tree(msg, msg_len, tree, parents, newrev, NULL, NULL);

	return;
}

static void copy_or_skip(struct commit *rev, const char *tree, struct commit_list *newparents, const char *newrev) {
	char *identical = NULL, *nonidentical = NULL;
	struct commit_list *np, *p = NULL, **pptr = &p;
	struct commit_list *gp, *gotparents = NULL, **gotparentsptr = &gotparents;
	int copycommit = 0, is_new;

	for (np = newparents; np; np = np->next) {
		if (!hashcmp(np->item->maybe_tree->object.oid.hash, tree))
			identical = np->item->object.oid.hash;
		else
			nonidentical = np->item->object.oid.hash;

		// sometimes both old parents map to the same newparent;
		// eliminate duplicates
		is_new = 1;
		for (gp = gotparents; gp; gp = gp->next) {
			if (!hashcmp(gp->item->object.oid.hash, np->item->object.oid.hash)) {
				// printf("copy_or_skip - is not new\n");
				is_new = 0;
				break;
			}
		}
		if (is_new) {
			// printf("copy_or_skip - new parent\n");
			gotparentsptr = &commit_list_insert(np->item, gotparentsptr)->next;
			pptr          = &commit_list_insert(np->item, pptr)->next;
		}
	}

	// printf("copy_or_skip - identical: %s nonidentical: %s\n", (identical)?"yes":"no", (nonidentical)?"yes":"no");
	if (identical != NULL && nonidentical != NULL) {
		//extras=$(git rev-list --count $identical..$nonidentical)
		// struct rev_info revs;
		// struct argv_array rev_argv = ARGV_ARRAY_INIT;

		// init_revisions(&revs, NULL);

		// /* rev_argv.argv[0] will be ignored by setup_revisions */
		// argv_array_push(&rev_argv, "find_existing_splits");
		// argv_array_push(&rev_argv, "f6727b0509ec3417a5183ba6e658143275a734f5..efaf6d46e082bf46320f95eb7a31d03b54825d1d");

		// rev_argv.argc = setup_revisions(rev_argv.argc, rev_argv.argv, &revs, NULL);

		// add_head_to_pending(&revs);

		// int ret_val = prepare_revision_walk(&revs);
		// if (ret_val < 0)
		//	die("prepare_revision_walk failed");

		int extras_count = 0;
		if (extras_count > 0)
			// we need to preserve history along the other branch
			copycommit = 1;
	}

	if (identical != NULL && !copycommit) {
		// printf("copy_or_skip - skip\n");
		hashcpy(newrev, identical);
	} else {
		// printf("copy_or_skip - copy\n");
		copy_commit(rev, tree, p, newrev);
	}

	return;
}

// FIXME maybe pass stuff in rather than globals?!?!
static void find_existing_splits(void) {
	struct rev_info revs;
	struct argv_array rev_argv = ARGV_ARRAY_INIT;

	init_revisions(&revs, NULL);

	argv_array_push(&rev_argv, "find_existing_splits");
	// FIXME char *dir = prefix_from_args
	// FIXME argv_array_push(&rev_argv, "--grep=^git-subtree-dir: $dir/*$");
	argv_array_push(&rev_argv, "--grep=^add");

	setup_revisions(rev_argv.argc, rev_argv.argv, &revs, NULL);

	add_head_to_pending(&revs);

	int ret_val = prepare_revision_walk(&revs);
	if (ret_val < 0)
		die("prepare_revision_walk failed");

	struct pretty_print_context ctx = {0};
	ctx.fmt = CMIT_FMT_USERFORMAT;
	ctx.date_mode.type = DATE_NORMAL;

	struct commit *commit;
	int num_revs = 0;
	while ((commit = get_revision(&revs))) {
		struct strbuf cmt_msg = STRBUF_INIT;
		format_commit_message(commit, "START %H%n%s%n%n%b%nEND%n", &cmt_msg, &ctx);
		char *nl_tk, *nl_str, *nl_tofree;
		nl_tofree = nl_str = strdup(cmt_msg.buf);

		struct object_id sq, mainline, sub;
		int mainline_exists = 0, sub_exists = 0;
		while ((nl_tk = strsep(&nl_str, "\n"))) {
			//printf("%3d: \"%s\"\n", num_revs, nl_tk);

			char *space_tk_a, *space_tk_b, *space_str, *space_tofree;
			space_tofree = space_str = strdup(nl_tk);
			if (!(space_tk_a = strsep(&space_str, " "))) {
				space_tk_a = "NOTHING";
				space_tk_b = "NOTHING";
			} else if (!(space_tk_b = strsep(&space_str, " "))) {
				space_tk_b = "NOTHING";
			}

			if (strcmp(space_tk_a, "START") == 0) {
				get_oid_hex(space_tk_b, &sq);
			} else if (strcmp(space_tk_a, "git-subtree-mainline:") == 0) {
				get_oid_hex(space_tk_b, &mainline);
				mainline_exists = 1;
			} else if (strcmp(space_tk_a, "git-subtree-split:") == 0) {
				// FIXME needs to run equivelant of git rev-parse "space_tk_b^0" Why?
				get_oid_hex(space_tk_b, &sub);
				sub_exists = 1;
				// or die "could not rev-parse split hash $b from commit $sq"
			} else if (strcmp(space_tk_a, "END") == 0) {
				printf("  Main is: %s\n", oid_to_hex(&mainline));

				if (sub_exists) {
					if (mainline_exists) {
						printf("  Squash: %s from %s\n", oid_to_hex(&sq), oid_to_hex(&sub));
						cache_set(&sq, &sub);
					} else {
						printf("  Prior: %s -> %s\n", oid_to_hex(&mainline), oid_to_hex(&sub));
						cache_set(&mainline, &sub);
						cache_set(&sub,  &sub);

						// FIXME low priority and doesn't affect outcome, just performance
						// try_remove_previous main
						// try_remove_previous sub
					}
				}
			}

			free(space_tofree);
		}
		free(nl_tofree);
		strbuf_release(&cmt_msg);
		num_revs++;
	}
	printf("%i revisions\n", num_revs);

	return;
}

static void find_subtree_commits(void) {
	struct rev_info revs;
	struct argv_array rev_argv = ARGV_ARRAY_INIT;

	init_revisions(&revs, NULL);

	/* rev_argv.argv[0] will be ignored by setup_revisions */
	argv_array_push(&rev_argv, "find_existing_splits");
	argv_array_push(&rev_argv, "--topo-order");
	argv_array_push(&rev_argv, "--reverse");
	//argv_array_push(&rev_argv, "f6727b0509ec3417a5183ba6e658143275a734f5..efaf6d46e082bf46320f95eb7a31d03b54825d1d");

	rev_argv.argc = setup_revisions(rev_argv.argc, rev_argv.argv, &revs, NULL);

	add_head_to_pending(&revs);

	int ret_val = prepare_revision_walk(&revs);
	if (ret_val < 0)
		die("prepare_revision_walk failed");

	struct commit *commit;
	int num_revs = 0;
	// int num_commits = 3;
	while ((commit = get_revision(&revs))) {
		printf("Processing commit: %s\n", oid_to_hex(&commit->object.oid));

		const struct oid2oid *exists;
		exists = cache_get(&(commit->object.oid));
		if (exists != NULL) {
			printf("  prior: %s\n");
			continue;
		}

		printf("  parents:");
		struct commit_list *p, *newparents = NULL, **npptr = &newparents;
		for (p = commit->parents; p; p = p->next) {
			printf(" %s", oid_to_hex(&p->item->object.oid));
			exists = cache_get(&(p->item->object.oid));
			if (exists != NULL) {
				struct commit *np = lookup_commit_reference(the_repository, &(exists->newrev));
				if (np) {
					npptr = &commit_list_insert(np, npptr)->next;
				} else {
					die("attempted to look up %s, but commit didn't exist?", oid_to_hex(&exists->newrev));
				}
			}
		}
		printf("\n  newparents: ");
		for (p = newparents; p; p = p->next)
			printf("%s\n", oid_to_hex(&p->item->object.oid));
		if (p == newparents)
			printf("\n");

		char tree_sha1[GIT_SHA1_RAWSZ];
		char newrev[GIT_SHA1_RAWSZ];
		unsigned tree_mode;
		ret_val = get_tree_entry(commit->maybe_tree->object.oid.hash, "contrib/subtree/", tree_sha1, &tree_mode);
		printf("  tree is:");
		if (!ret_val) {
			printf("  %s\n", sha1_to_hex(tree_sha1));
			copy_or_skip(commit, tree_sha1, newparents, newrev);
			// printf("  cacheset: %s -> %s\n", sha1_to_hex(commit->object.oid.hash), sha1_to_hex(newrev));
			struct object_id newrev_oid;
			hashcpy(&newrev_oid.hash, newrev);
			cache_set(&commit->object.oid, &newrev_oid);
			printf("  newrev is: %s\n", sha1_to_hex(newrev));
			// exists = cache_get(&(commit->object.oid));
			// if (exists == NULL) {
			// 	printf("  failed to find newrev in cache!\n");
			// 	die("dying\n");
			// } else {
			// 	printf("  newrev recovered: %s\n", sha1_to_hex(exists->newrev));
			// }
			// num_commits--;
			// if (num_commits == 0)
			// 	goto early_fail;
		} else {
			printf("\n  notree\n");
			// TODO some notree cache magic? check newparents?
		}

		num_revs++;
	}
early_fail:
	printf("%i revisions\n", num_revs);

	return;
}

int cmd_main(int argc, const char **argv)
{
	int strict = 0;
	struct option options[] = {
		OPT_BOOL(0, "strict", &strict,
			 N_("do not try <directory>/.git/ if <directory> is no Git directory")),
		OPT_END()
	};

	//const char *dir;
	//setup_path();
	//dir = argv[0];
	// if (!enter_repo(dir, strict))
	// 	die("'%s' does not appear to be a git repository", dir);

	const char * prefix;
	prefix = setup_git_directory();
	printf("prefix: %s\n", prefix);
	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, options, split_usage, 0);
	if (argc < 1)
		usage_with_options(split_usage, options);

	// char *obj_name = "HEAD";
	char *obj_name = "efaf6d46e082bf46320f95eb7a31d03b54825d1d";
	struct object_id oid;
	struct object_context obj_context;

	if (get_oid_with_context(obj_name, 0, oid.hash, &obj_context))
		die("Not a valid object name %s", obj_name);

	hashmap_init(&revcache, (hashmap_cmp_fn) oidhash_cmp, NULL, 0);
	printf("size/table: %i/%i\n", hashmap_get_size(&revcache), revcache.tablesize);

	// find_existing_splits();

	find_subtree_commits();

	return 0;
}

