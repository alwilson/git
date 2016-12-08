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

#include "hashmap.h"

struct hashmap revcache;

static const char * const split_usage[] = {
	N_("git split [<options>] <dir>"),
	NULL
};

void print_hash(unsigned char hash[])
{
	int i;
	for (i = 0; i < 20; i++)
		printf("%02x", hash[i]);
	printf("\n");
}

void find_existing_splits(void) {
	struct rev_info revs;	
	struct argv_array rev_argv = ARGV_ARRAY_INIT;

	init_revisions(&revs, NULL);

	/* rev_argv.argv[0] will be ignored by setup_revisions */
	argv_array_push(&rev_argv, "find_existing_splits");
	argv_array_push(&rev_argv, "--grep=subtree");

	setup_revisions(rev_argv.argc, rev_argv.argv, &revs, NULL);

	add_head_to_pending(&revs);

	int ret_val = prepare_revision_walk(&revs);
	if (ret_val < 0)
		die("prepare_revision_walk failed");

	struct pretty_print_context ctx = {0};
	ctx.fmt = CMIT_FMT_USERFORMAT;
	//ctx.abbrev = log->abbrev;
	//ctx.subject = "";
	//ctx.after_subject = "";
	ctx.date_mode.type = DATE_NORMAL;
	//ctx.output_encoding = get_log_output_encoding();

	struct commit *commit;
	int num_revs = 0;
	while ((commit = get_revision(&revs))) {
		print_hash(commit->object.oid.hash);
		struct strbuf oneline = STRBUF_INIT;
		format_commit_message(commit, "%s", &oneline, &ctx);
		printf("\t%s\n", oneline.buf);
		strbuf_release(&oneline);
		num_revs++;
	}
	printf("%i revisions\n", num_revs);

	return;
}

struct str2rev {
	struct hashmap_entry ent;
	char key[20];
	struct object_id value;
};

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

	const char * prefix;
	prefix = setup_git_directory();
	printf("prefix: %s\n", prefix);
	git_config(git_default_config, NULL);

	//char *obj_name = "f6727b0509ec3417a5183ba6e658143275a734f5";
	//char *obj_name = "ff6727b0509ec3417a5183ba6e658143275a734f5";
	char *obj_name = "HEAD";
	struct object_id oid;
	struct object_context obj_context;

	if (get_sha1_with_context(obj_name, 0, oid.hash, &obj_context))
		die("Not a valid object name %s", obj_name);

	hashmap_init(&revcache, (hashmap_cmp_fn)strihash, 0);
	printf("size/table: %i/%i\n", revcache.size, revcache.tablesize);

	struct str2rev *e;
	e = malloc(sizeof(struct str2rev));
	hashmap_entry_init(e, strihash("hey"));
	e->key[0] = 'h';
	e->key[1] = 'e';
	e->key[2] = 'y';
	e->key[3] = '\0';
	e->value = oid;
	hashmap_add(&revcache, e);
	printf("size/table: %i/%i\n", revcache.size, revcache.tablesize);

	//find_existing_splits();

	return 0;
}

