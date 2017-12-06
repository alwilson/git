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
#include "exec_cmd.h"
#include "diff.h"
#include "revision.h"
#include "list-objects.h"
#include "run-command.h"
#include "connect.h"
#include "sigchain.h"
#include "version.h"
#include "string-list.h"
#include "parse-options.h"

#include "config.h"

#include "hashmap.h"

static unsigned int oid_hash(const struct object_id *);
static int oid_hash_cmp(const void *, const void *, const void *, const void *);
static void cache_set(const struct object_id *, const struct object_id *);
static void find_existing_splits(void);

struct hashmap revcache;

struct oid2oid {
	struct hashmap_entry ent;
	struct object_id key;
	struct object_id value;
};

static unsigned int oid_hash(const struct object_id *oid) {
	return memihash(oid->hash, sizeof(oid->hash));
}

static int oid_hash_cmp(const void *unused_cmp_data,
			const void *entry,
			const void *entry_or_key,
			const void *unused_keydata)
{
	const struct oid2oid *a = entry;
	const struct oid2oid *b = entry_or_key;
	return memcmp(&(a->key), &(b->key), sizeof(a->key));
};

static const char * const split_usage[] = {
	N_("git split [<options>] <dir>"),
	NULL
};

static void cache_set(const struct object_id *old_rev,
		     const struct object_id *new_rev)
{
	struct oid2oid *e;
	e = malloc(sizeof(struct oid2oid));
	hashmap_entry_init(e, oid_hash(old_rev));
	memcpy(&(e->key),   old_rev, sizeof(struct object_id));
	memcpy(&(e->value), new_rev, sizeof(struct object_id));

	struct oid2oid *found;
	found = hashmap_put(&revcache, e);
	if (found != NULL) {
		printf("Found a previous cache entry!?\n");
		// FIXME die here? print out some debug?
		free(found);
	}
};


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

int cmd_main(int argc, const char **argv)
{
	//const char *dir;
	//int strict = 0;
	//struct option options[] = {
	//	OPT_BOOL(0, "strict", &strict,
	//		 N_("do not try <directory>/.git/ if <directory> is no Git directory")),
	//	OPT_END()
	//};

	//argc = parse_options(argc, argv, NULL, options, split_usage, 0);

	//if (argc != 1)ww//	usage_with_options(split_usage, options);w
	//setup_path();

	//dir = argv[0];

	//if (!enter_repo(dir, strict))
	//	die("'%s' does not appear to be a git repository", dir);

	// FIXME This is _not_ a subtree prefix! What is it exactly? Just regular init to find our git config info?
	const char * prefix;
	prefix = setup_git_directory();
	printf("prefix: %s\n", prefix);
	git_config(git_default_config, NULL);

	// FIXME need cmd line arg here
	char *obj_name = "HEAD";
	struct object_id oid;
	struct object_context obj_context;

	if (get_oid_with_context(obj_name, 0, &oid, &obj_context))
		die("Not a valid object name %s", obj_name);

	hashmap_init(&revcache, (hashmap_cmp_fn) oid_hash_cmp, NULL, 0);
	printf("size/table: %i/%i\n", hashmap_get_size(&revcache), revcache.tablesize);

	// JUNK This is just a fun way to see the hashmap in action as it auto-resizes with growth
	// int i;
	// for (i = 0; i < 1025; i++) {
	// 	struct oid2oid *e;
	// 	e = malloc(sizeof(struct oid2oid));
	// 	hashmap_entry_init(e, oid_hash(&oid));
	// 	memcpy(&(e->key), &oid, sizeof(oid));
	// 	memcpy(&(e->value), &oid, sizeof(oid));

	// 	struct oid2oid *found;
	// 	found = hashmap_put(&revcache, e);
	// 	if (found != NULL) {
	// 		printf("found it!\n");
	// 		free(found);
	// 	} else {
	// 		printf("new entry added\n");
	// 	}

	// 	printf("size/table: %i/%i\n", hashmap_get_size(&revcache), revcache.tablesize);
	// }

	//find_existing_splits();

	struct rev_info revs;
	struct argv_array rev_argv = ARGV_ARRAY_INIT;

	init_revisions(&revs, NULL);

	argv_array_push(&rev_argv, "cmd_main");
	argv_array_push(&rev_argv, "--topo-order");
	argv_array_push(&rev_argv, "--reverse");
	argv_array_push(&rev_argv, "--parents");
	argv_array_push(&rev_argv, "HEAD");

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
		num_revs++;
	}
	printf("%i revisions\n", num_revs);

	return 0;
}

