// SPDX-License-Identifier: GPL-2.0+
/*
 * Show and change the runtime options that coreboot describes in its CFR table
 *
 * Values are UEFI variables with the coreboot vendor GUID, which coreboot
 * reads with its USE_UBOOT_VARIABLE_STORE option backend. Changes are staged
 * and written together by 'cfr save'.
 *
 * Copyright (C) 2026 9elements GmbH
 */

#include <charset.h>
#include <command.h>
#include <efi_loader.h>
#include <efi_variable.h>
#include <vsprintf.h>
#include <asm/cb_sysinfo.h>
#include <asm/coreboot_tables.h>
#include <linux/kernel.h>
#include <linux/string.h>

#define CFR_MAX_ENTRIES	64
#define CFR_NAME_MAX	64
#define CFR_VAR_ATTR	(EFI_VARIABLE_NON_VOLATILE | \
			 EFI_VARIABLE_BOOTSERVICE_ACCESS | \
			 EFI_VARIABLE_RUNTIME_ACCESS)

static const efi_guid_t cfr_guid =
	EFI_GUID(0xceae4c1d, 0x335b, 0x4685,
		 0xa4, 0xa0, 0xfc, 0x4a, 0x94, 0xee, 0xa0, 0x85);

/**
 * struct cfr_entry - a form, option or comment from the CFR table
 *
 * @obj: Record in the coreboot table
 * @end: End of the record
 * @values: First record after the strings (enum values or form contents)
 * @name: Option name, NULL for forms and comments
 * @ui_name: Display name
 * @help: Help text or NULL
 * @dep_values: Values of the dependency that make this entry active
 * @num_dep_values: Number of @dep_values
 * @staged: A change is staged
 * @staged_default: The staged change removes the stored value
 * @staged_value: Staged value, if not @staged_default
 */
struct cfr_entry {
	const struct cb_cfr_object *obj;
	const void *end;
	const void *values;
	const char *name;
	const char *ui_name;
	const char *help;
	const u32 *dep_values;
	u32 num_dep_values;
	bool staged;
	bool staged_default;
	u32 staged_value;
};

static struct cfr_entry entries[CFR_MAX_ENTRIES];
static int num_entries;
static const struct cb_cfr *parsed;

static bool is_numeric(const struct cfr_entry *e)
{
	return e->obj->tag == CFR_TAG_OPTION_ENUM ||
	       e->obj->tag == CFR_TAG_OPTION_NUMBER ||
	       e->obj->tag == CFR_TAG_OPTION_BOOL;
}

static const struct cb_cfr_numeric_option *numeric(const struct cfr_entry *e)
{
	return (const struct cb_cfr_numeric_option *)e->obj;
}

/* Reads the strings that follow an object; returns the first other record */
static const void *parse_strings(struct cfr_entry *e, const void *p,
				 const void *end)
{
	while (p + sizeof(struct cb_cfr_varbinary) <= end) {
		const struct cb_cfr_varbinary *v = p;
		const char *str = (const char *)(v + 1);

		if (v->size < sizeof(*v) || p + v->size > end)
			break;
		switch (v->tag) {
		case CFR_TAG_VARCHAR_OPT_NAME:
			e->name = str;
			break;
		case CFR_TAG_VARCHAR_UI_NAME:
			e->ui_name = str;
			break;
		case CFR_TAG_VARCHAR_UI_HELPTEXT:
			e->help = str;
			break;
		case CFR_TAG_DEP_VALUES:
			e->dep_values = (const u32 *)str;
			e->num_dep_values = v->data_length / sizeof(u32);
			break;
		case CFR_TAG_VARCHAR_DEF_VALUE:
			break;
		default:
			return p;
		}
		p += v->size;
	}

	return p;
}

static void parse_records(const void *p, const void *end)
{
	while (p + sizeof(struct cb_cfr_record) <= end &&
	       num_entries < CFR_MAX_ENTRIES) {
		const struct cb_cfr_record *rec = p;
		const void *rec_end = p + rec->size;
		struct cfr_entry *e = &entries[num_entries];
		size_t fixed = sizeof(struct cb_cfr_object);

		if (rec->size < sizeof(*rec) || rec_end > end)
			return;
		switch (rec->tag) {
		case CFR_TAG_OPTION_ENUM:
		case CFR_TAG_OPTION_NUMBER:
		case CFR_TAG_OPTION_BOOL:
			fixed = sizeof(struct cb_cfr_numeric_option);
			fallthrough;
		case CFR_TAG_OPTION_FORM:
		case CFR_TAG_OPTION_VARCHAR:
		case CFR_TAG_OPTION_COMMENT:
			if (rec->size < fixed)
				return;
			memset(e, 0, sizeof(*e));
			e->obj = p;
			e->end = rec_end;
			e->values = parse_strings(e, p + fixed, rec_end);
			num_entries++;
			if (rec->tag == CFR_TAG_OPTION_FORM)
				parse_records(e->values, rec_end);
			break;
		}
		p = rec_end;
	}
}

static int load_table(void)
{
	const struct sysinfo_t *info = cb_get_sysinfo();
	const struct cb_cfr *root = info ? info->cfr : NULL;

	if (!root) {
		printf("coreboot provides no option table (CFR)\n");
		return -ENOENT;
	}
	if (root != parsed) {
		num_entries = 0;
		parse_records(root + 1, (const void *)root + root->size);
		parsed = root;
	}
	if (efi_init_obj_list() != EFI_SUCCESS) {
		printf("Cannot initialize the UEFI variable store\n");
		return -EIO;
	}

	return 0;
}

static struct cfr_entry *find_option(const char *name)
{
	for (int i = 0; i < num_entries; i++)
		if (entries[i].name && is_numeric(&entries[i]) &&
		    !strcmp(entries[i].name, name))
			return &entries[i];

	return NULL;
}

static bool var_name(const struct cfr_entry *e, u16 *buf)
{
	u16 *p = buf;

	if (strlen(e->name) >= CFR_NAME_MAX)
		return false;
	utf8_utf16_strncpy(&p, e->name, CFR_NAME_MAX - 1);

	return true;
}

/* Returns true and the value if a value is stored for the option */
static bool stored_value(const struct cfr_entry *e, u32 *value)
{
	u16 name[CFR_NAME_MAX];
	efi_uintn_t size = sizeof(*value);

	return var_name(e, name) &&
	       efi_get_variable_int(name, &cfr_guid, NULL, &size, value,
				    NULL) == EFI_SUCCESS &&
	       size == sizeof(*value);
}

/* The value coreboot uses on the next boot, including staged changes */
static u32 value_of(const struct cfr_entry *e)
{
	u32 value;

	if (e->staged)
		return e->staged_default ? numeric(e)->default_value :
		       e->staged_value;
	if (stored_value(e, &value))
		return value;

	return numeric(e)->default_value;
}

static const char *enum_label(const struct cfr_entry *e, u32 value)
{
	const void *p;

	if (e->obj->tag == CFR_TAG_OPTION_BOOL)
		return value ? "Enabled" : "Disabled";
	if (e->obj->tag != CFR_TAG_OPTION_ENUM)
		return NULL;
	for (p = e->values; p + sizeof(struct cb_cfr_enum_value) <= e->end;
	     p += ((const struct cb_cfr_record *)p)->size) {
		const struct cb_cfr_enum_value *v = p;

		if (v->size < sizeof(*v))
			break;
		if (v->tag == CFR_TAG_ENUM_VALUE && v->value == value)
			return (const char *)((const struct cb_cfr_varbinary *)
					      (v + 1) + 1);
	}

	return NULL;
}

static void format_value(const struct cfr_entry *e, u32 value, char *buf,
			 size_t size)
{
	const char *label = enum_label(e, value);

	if (label)
		snprintf(buf, size, "%s", label);
	else if (e->obj->tag == CFR_TAG_OPTION_NUMBER &&
		 !(numeric(e)->display_flags & CFR_NUM_OPT_DISPFLAG_HEX))
		snprintf(buf, size, "%u", value);
	else
		snprintf(buf, size, "%#x", value);
}

static bool is_active(const struct cfr_entry *e)
{
	if (e->obj->flags & CFR_OPTFLAG_INACTIVE)
		return false;
	if (!e->obj->dependency_id)
		return true;
	for (int i = 0; i < num_entries; i++) {
		const struct cfr_entry *dep = &entries[i];
		u32 value;

		if (dep->obj->object_id != e->obj->dependency_id ||
		    !is_numeric(dep))
			continue;
		value = value_of(dep);
		if (!e->num_dep_values)
			return value != 0;
		for (u32 k = 0; k < e->num_dep_values; k++)
			if (e->dep_values[k] == value)
				return true;
		return false;
	}

	return true;
}

static void print_wrapped(const char *text, int indent)
{
	const int width = 78;
	int col = indent;

	printf("%*s", indent, "");
	while (*text) {
		int len = strcspn(text, " ");

		if (col > indent && col + 1 + len > width) {
			printf("\n%*s", indent, "");
			col = indent;
		} else if (col > indent) {
			putc(' ');
			col++;
		}
		printf("%.*s", len, text);
		col += len;
		text += len;
		text += strspn(text, " ");
	}
	putc('\n');
}

static void print_values(const struct cfr_entry *e)
{
	const struct cb_cfr_numeric_option *opt = numeric(e);
	const void *p;
	int n = 0;

	printf("  Values:  ");
	if (e->obj->tag == CFR_TAG_OPTION_BOOL) {
		printf("Disabled, Enabled\n");
		return;
	}
	if (e->obj->tag == CFR_TAG_OPTION_NUMBER) {
		printf("%u..%u, step %u\n", opt->min, opt->max,
		       opt->step ? opt->step : 1);
		return;
	}
	for (p = e->values; p + sizeof(struct cb_cfr_enum_value) <= e->end;
	     p += ((const struct cb_cfr_record *)p)->size) {
		const struct cb_cfr_enum_value *v = p;

		if (v->size < sizeof(*v))
			break;
		if (v->tag == CFR_TAG_ENUM_VALUE)
			printf("%s%s", n++ ? ", " : "", enum_label(e, v->value));
	}
	putc('\n');
}

static void print_entry(const struct cfr_entry *e, bool verbose)
{
	char value[32], def[32];
	u32 stored;

	switch (e->obj->tag) {
	case CFR_TAG_OPTION_FORM:
		printf("%s\n", e->ui_name ?: "");
		return;
	case CFR_TAG_OPTION_COMMENT:
		printf("  %s\n", e->ui_name ?: "");
		return;
	case CFR_TAG_OPTION_VARCHAR:
		printf("  %-20s %-26s (text, not supported)\n", e->name,
		       e->ui_name ?: "");
		return;
	}

	format_value(e, value_of(e), value, sizeof(value));
	printf("  %-20s %-26s %s%s", e->name, e->ui_name ?: "", value,
	       e->staged ? " *" : "");
	if (e->staged ? !e->staged_default : stored_value(e, &stored)) {
		format_value(e, numeric(e)->default_value, def, sizeof(def));
		printf("  (default %s)", def);
	}
	if (!is_active(e))
		printf("  (inactive)");
	if (e->obj->flags & CFR_OPTFLAG_READONLY)
		printf("  (read-only)");
	putc('\n');
	if (verbose) {
		if (e->help)
			print_wrapped(e->help, 4);
		printf("  ");
		print_values(e);
	}
}

static int do_cfr_list(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	bool verbose = argc > 1 && !strcmp(argv[1], "-v");
	bool pending = false;

	const void *hidden_end = NULL;

	if (load_table())
		return CMD_RET_FAILURE;
	for (int i = 0; i < num_entries; i++) {
		const struct cfr_entry *e = &entries[i];

		/* a suppressed form hides everything inside it */
		if ((const void *)e->obj < hidden_end)
			continue;
		if (e->obj->flags & CFR_OPTFLAG_SUPPRESS) {
			hidden_end = e->end;
			continue;
		}
		print_entry(e, verbose);
		pending |= e->staged;
	}
	if (pending)
		printf("* not saved yet: 'cfr save' stores, 'cfr discard' drops the changes\n");

	return CMD_RET_SUCCESS;
}

static struct cfr_entry *get_option(const char *name)
{
	struct cfr_entry *e;

	if (load_table())
		return NULL;
	e = find_option(name);
	if (!e)
		printf("Unknown option '%s'; 'cfr list' shows the options\n",
		       name);

	return e;
}

static int do_cfr_show(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct cfr_entry *e;
	char buf[32];
	u32 stored;

	if (argc != 2)
		return CMD_RET_USAGE;
	e = get_option(argv[1]);
	if (!e)
		return CMD_RET_FAILURE;

	printf("%s: %s\n", e->name, e->ui_name ?: "");
	if (e->help)
		print_wrapped(e->help, 2);
	format_value(e, value_of(e), buf, sizeof(buf));
	printf("  Value:   %s%s\n", buf, e->staged ? " (not saved)" :
	       stored_value(e, &stored) ? "" : " (default)");
	format_value(e, numeric(e)->default_value, buf, sizeof(buf));
	printf("  Default: %s\n", buf);
	print_values(e);
	if (!is_active(e))
		printf("  Inactive: another option disables it\n");
	if (e->obj->flags & CFR_OPTFLAG_READONLY)
		printf("  Read-only\n");

	return CMD_RET_SUCCESS;
}

static bool parse_value(const struct cfr_entry *e, const char *arg, u32 *value)
{
	const struct cb_cfr_numeric_option *opt = numeric(e);
	static const char *const off[] = { "0", "off", "no", "false", "disabled" };
	static const char *const on[] = { "1", "on", "yes", "true", "enabled" };
	const void *p;
	char *end;

	switch (e->obj->tag) {
	case CFR_TAG_OPTION_BOOL:
		for (int i = 0; i < ARRAY_SIZE(off); i++) {
			if (!strcasecmp(arg, off[i]))
				return *value = 0, true;
			if (!strcasecmp(arg, on[i]))
				return *value = 1, true;
		}
		return false;
	case CFR_TAG_OPTION_NUMBER:
		*value = simple_strtoul(arg, &end, 0);
		return *arg && !*end && *value >= opt->min &&
		       *value <= opt->max &&
		       (!opt->step || (*value - opt->min) % opt->step == 0);
	}

	*value = simple_strtoul(arg, &end, 0);
	if (*arg && !*end)
		return !!enum_label(e, *value);
	for (p = e->values; p + sizeof(struct cb_cfr_enum_value) <= e->end;
	     p += ((const struct cb_cfr_record *)p)->size) {
		const struct cb_cfr_enum_value *v = p;

		if (v->size < sizeof(*v))
			break;
		if (v->tag == CFR_TAG_ENUM_VALUE &&
		    !strcasecmp(arg, enum_label(e, v->value)))
			return *value = v->value, true;
	}

	return false;
}

static int do_cfr_set(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	char from[32], to[32];
	struct cfr_entry *e;
	u32 value;

	if (argc != 3)
		return CMD_RET_USAGE;
	e = get_option(argv[1]);
	if (!e)
		return CMD_RET_FAILURE;
	if (e->obj->flags & CFR_OPTFLAG_READONLY) {
		printf("%s is read-only\n", e->name);
		return CMD_RET_FAILURE;
	}
	if (!parse_value(e, argv[2], &value)) {
		printf("Invalid value '%s' for %s\n", argv[2], e->name);
		print_values(e);
		return CMD_RET_FAILURE;
	}

	format_value(e, value_of(e), from, sizeof(from));
	format_value(e, value, to, sizeof(to));
	e->staged = true;
	e->staged_default = false;
	e->staged_value = value;
	printf("%s: %s -> %s (not saved yet)\n", e->name, from, to);

	return CMD_RET_SUCCESS;
}

static void stage_default(struct cfr_entry *e)
{
	u32 stored;

	if (!stored_value(e, &stored)) {
		e->staged = false;
		return;
	}
	e->staged = true;
	e->staged_default = true;
}

static int do_cfr_default(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	struct cfr_entry *e;

	if (argc != 2)
		return CMD_RET_USAGE;
	if (!strcmp(argv[1], "-a")) {
		if (load_table())
			return CMD_RET_FAILURE;
		for (int i = 0; i < num_entries; i++)
			if (entries[i].name && is_numeric(&entries[i]))
				stage_default(&entries[i]);
		printf("All options staged to return to their defaults (not saved yet)\n");
		return CMD_RET_SUCCESS;
	}

	e = get_option(argv[1]);
	if (!e)
		return CMD_RET_FAILURE;
	stage_default(e);
	printf("%s: back to the default (not saved yet)\n", e->name);

	return CMD_RET_SUCCESS;
}

static int do_cfr_pending(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	int n = 0;

	if (load_table())
		return CMD_RET_FAILURE;
	for (int i = 0; i < num_entries; i++) {
		struct cfr_entry *e = &entries[i];
		char buf[32];

		if (!e->staged)
			continue;
		format_value(e, value_of(e), buf, sizeof(buf));
		printf("  %-20s -> %s%s\n", e->name, buf,
		       e->staged_default ? " (default)" : "");
		n++;
	}
	if (!n)
		printf("No unsaved changes\n");

	return CMD_RET_SUCCESS;
}

static int do_cfr_discard(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	for (int i = 0; i < num_entries; i++)
		entries[i].staged = false;
	printf("Unsaved changes dropped\n");

	return CMD_RET_SUCCESS;
}

static int do_cfr_save(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	int n = 0;

	if (load_table())
		return CMD_RET_FAILURE;
	for (int i = 0; i < num_entries; i++) {
		struct cfr_entry *e = &entries[i];
		u16 name[CFR_NAME_MAX];
		efi_status_t ret;

		if (!e->staged || !var_name(e, name))
			continue;
		if (e->staged_default)
			ret = efi_set_variable_int(name, &cfr_guid, 0, 0, NULL,
						   false);
		else
			ret = efi_set_variable_int(name, &cfr_guid, CFR_VAR_ATTR,
						   sizeof(e->staged_value),
						   &e->staged_value, false);
		if (ret != EFI_SUCCESS && ret != EFI_NOT_FOUND) {
			printf("Saving %s failed: %lu\n", e->name,
			       ret & ~EFI_ERROR_MASK);
			return CMD_RET_FAILURE;
		}
		e->staged = false;
		n++;
	}

	printf("%d option(s) saved; they apply on the next boot\n", n);
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cfr_cmds[] = {
	U_BOOT_CMD_MKENT(list, 2, 0, do_cfr_list, "", ""),
	U_BOOT_CMD_MKENT(show, 2, 0, do_cfr_show, "", ""),
	U_BOOT_CMD_MKENT(set, 3, 0, do_cfr_set, "", ""),
	U_BOOT_CMD_MKENT(default, 2, 0, do_cfr_default, "", ""),
	U_BOOT_CMD_MKENT(pending, 1, 0, do_cfr_pending, "", ""),
	U_BOOT_CMD_MKENT(save, 1, 0, do_cfr_save, "", ""),
	U_BOOT_CMD_MKENT(discard, 1, 0, do_cfr_discard, "", ""),
};

static int do_cfr(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	struct cmd_tbl *cmd;

	if (argc < 2)
		return do_cfr_list(cmdtp, flag, 1, argv);
	cmd = find_cmd_tbl(argv[1], cfr_cmds, ARRAY_SIZE(cfr_cmds));
	if (!cmd || argc > cmd->maxargs + 1)
		return CMD_RET_USAGE;

	return cmd->cmd(cmd, flag, argc - 1, argv + 1);
}

U_BOOT_LONGHELP(cfr,
	"[list [-v]]          - show the options (-v: with help and values)\n"
	"cfr show <name>          - show one option\n"
	"cfr set <name> <value>   - change an option; value is a label or a number\n"
	"cfr default <name> | -a  - return an option (-a: all) to its default\n"
	"cfr pending              - show the changes that are not saved yet\n"
	"cfr save                 - store the changes; they apply on the next boot\n"
	"cfr discard              - drop the changes that are not saved yet");

U_BOOT_CMD(cfr, 4, 1, do_cfr, "coreboot runtime options (CFR)", cfr_help_text);
