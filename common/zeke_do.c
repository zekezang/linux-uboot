#include <common.h>
#include <command.h>
#include <zeke_do.h>

int run_command_n(const char *cmd, int flag);

void setenvx(){
	run_command_n("setenv bootdelay 9", CMD_FLAG_REPEAT);
	run_command_n("setenv gatewayip 10.0.161.254", CMD_FLAG_REPEAT);
	run_command_n("setenv serverip 10.0.161.235", CMD_FLAG_REPEAT);
	run_command_n("setenv ipaddr 10.0.161.236", CMD_FLAG_REPEAT);
	run_command_n(
			"setenv bootargs console=ttySAC0 root=/dev/nfs nfsroot=10.0.161.235:/sourcecode/arm-workspace/rootfs-6410/rfs,proto=tcp,nfsvers=3,nolock ip=10.0.161.236:10.0.161.235:10.0.161.254:255.255.255.0:zekezang:eth0:off",
			CMD_FLAG_REPEAT);
	run_command_n("saveenv", CMD_FLAG_REPEAT);
}

void test() {
	printf("hello world.\n");
	setenvx();
	run_command_n("tftp 0xC0008000 uImage",CMD_FLAG_REPEAT);
	run_command_n("bootm 0xC0008000",CMD_FLAG_REPEAT);
}

void erase_nand() {
	int xx = run_command_n("nand erase 0x0 0x800000", CMD_FLAG_REPEAT);
	printf("------%d--.\n", xx);
}

int parse_line_n(char *line, char *argv[]) {
	int nargs = 0;

#ifdef DEBUG_PARSER
	printf ("parse_line_n: \"%s\"\n", line);
#endif
	while (nargs < CFG_MAXARGS) {

		/* skip any white space */
		while ((*line == ' ') || (*line == '\t')) {
			++line;
		}

		if (*line == '\0') { /* end of line, no more args	*/
			argv[nargs] = NULL;
#ifdef DEBUG_PARSER
			printf ("parse_line_n: nargs=%d\n", nargs);
#endif
			return (nargs);
		}

		argv[nargs++] = line; /* begin of argument string	*/

		/* find end of string */
		while (*line && (*line != ' ') && (*line != '\t')) {
			++line;
		}

		if (*line == '\0') { /* end of line, no more args	*/
			argv[nargs] = NULL;
#ifdef DEBUG_PARSER
			printf ("parse_line_n: nargs=%d\n", nargs);
#endif
			return (nargs);
		}

		*line++ = '\0'; /* terminate current arg	 */
	}

	printf("** Too many args (max. %d) **\n", CFG_MAXARGS);

#ifdef DEBUG_PARSER
	printf ("parse_line_n: nargs=%d\n", nargs);
#endif
	return (nargs);
}

/****************************************************************************/

static void process_macros_n(const char *input, char *output) {
	char c, prev;
	const char *varname_start = NULL;
	int inputcnt = strlen(input);
	int outputcnt = CFG_CBSIZE;
	int state = 0; /* 0 = waiting for '$'  */

	/* 1 = waiting for '(' or '{' */
	/* 2 = waiting for ')' or '}' */
	/* 3 = waiting for '''  */
#ifdef DEBUG_PARSER
	char *output_start = output;

	printf ("[process_macros_n] INPUT len %d: \"%s\"\n", strlen (input),
			input);
#endif

	prev = '\0'; /* previous character   */

	while (inputcnt && outputcnt) {
		c = *input++;
		inputcnt--;

		if (state != 3) {
			/* remove one level of escape characters */
			if ((c == '\\') && (prev != '\\')) {
				if (inputcnt-- == 0)
					break;
				prev = c;
				c = *input++;
			}
		}

		switch (state) {
		case 0: /* Waiting for (unescaped) $    */
			if ((c == '\'') && (prev != '\\')) {
				state = 3;
				break;
			}
			if ((c == '$') && (prev != '\\')) {
				state++;
			} else {
				*(output++) = c;
				outputcnt--;
			}
			break;
		case 1: /* Waiting for (        */
			if (c == '(' || c == '{') {
				state++;
				varname_start = input;
			} else {
				state = 0;
				*(output++) = '$';
				outputcnt--;

				if (outputcnt) {
					*(output++) = c;
					outputcnt--;
				}
			}
			break;
		case 2: /* Waiting for )        */
			if (c == ')' || c == '}') {
				int i;
				char envname[CFG_CBSIZE], *envval;
				int envcnt = input - varname_start - 1; /* Varname # of chars */

				/* Get the varname */
				for (i = 0; i < envcnt; i++) {
					envname[i] = varname_start[i];
				}
				envname[i] = 0;

				/* Get its value */
				envval = getenv(envname);

				/* Copy into the line if it exists */
				if (envval != NULL)
					while ((*envval) && outputcnt) {
						*(output++) = *(envval++);
						outputcnt--;
					}
				/* Look for another '$' */
				state = 0;
			}
			break;
		case 3: /* Waiting for '        */
			if ((c == '\'') && (prev != '\\')) {
				state = 0;
			} else {
				*(output++) = c;
				outputcnt--;
			}
			break;
		}
		prev = c;
	}

	if (outputcnt)
		*output = 0;

#ifdef DEBUG_PARSER
	printf ("[process_macros_n] OUTPUT len %d: \"%s\"\n",
			strlen (output_start), output_start);
#endif
}

int run_command_n(const char *cmd, int flag) {
	cmd_tbl_t *cmdtp;
	char cmdbuf[CFG_CBSIZE]; /* working copy of cmd		*/
	char *token; /* start of token in cmdbuf	*/
	char *sep; /* end of token (separator) in cmdbuf */
	char finaltoken[CFG_CBSIZE];
	char *str = cmdbuf;
	char *argv[CFG_MAXARGS + 1]; /* NULL terminated	*/
	int argc, inquotes;
	int repeatable = 1;
	int rc = 0;

#ifdef DEBUG_PARSER
	printf ("[RUN_COMMAND] cmd[%p]=\"", cmd);
	puts (cmd ? cmd : "NULL"); /* use puts - string may be loooong */
	puts ("\"\n");
#endif

	clear_ctrlc(); /* forget any previous Control C */

	if (!cmd || !*cmd) {
		return -1; /* empty command */
	}

	if (strlen(cmd) >= CFG_CBSIZE) {
		puts("## Command too long!\n");
		return -1;
	}

	strcpy(cmdbuf, cmd);

	/* Process separators and check for invalid
	 * repeatable commands
	 */

#ifdef DEBUG_PARSER
	printf ("[PROCESS_SEPARATORS] %s\n", cmd);
#endif
	while (*str) {

		/*
		 * Find separator, or string end
		 * Allow simple escape of ';' by writing "\;"
		 */
		for (inquotes = 0, sep = str; *sep; sep++) {
			if ((*sep == '\'') && (*(sep - 1) != '\\'))
				inquotes = !inquotes;

			if (!inquotes && (*sep == ';') && /* separator		*/
			(sep != str) && /* past string start	*/
			(*(sep - 1) != '\\')) /* and NOT escaped	*/
				break;
		}

		/*
		 * Limit the token to data between separators
		 */
		token = str;
		if (*sep) {
			str = sep + 1; /* start of command for next pass */
			*sep = '\0';
		} else
			str = sep; /* no more commands for next pass */
#ifdef DEBUG_PARSER
		printf ("token: \"%s\"\n", token);
#endif

		/* find macros in this token and replace them */
		process_macros_n(token, finaltoken);

		/* Extract arguments */
		if ((argc = parse_line_n(finaltoken, argv)) == 0) {
			rc = -1; /* no command at all */
			continue;
		}

		/* Look up command in command table */
		if ((cmdtp = find_cmd(argv[0])) == NULL) {
			printf("Unknown command '%s' - try 'help'\n", argv[0]);
			rc = -1; /* give up after bad command */
			continue;
		}

		/* found - check max args */
		if (argc > cmdtp->maxargs) {
			printf("Usage:\n%s\n", cmdtp->usage);
			rc = -1;
			continue;
		}

#if (CONFIG_COMMANDS & CFG_CMD_BOOTD)
		/* avoid "bootd" recursion */
		if (cmdtp->cmd == do_bootd) {
#ifdef DEBUG_PARSER
			printf ("[%s]\n", finaltoken);
#endif
			if (flag & CMD_FLAG_BOOTD) {
				puts ("'bootd' recursion detected\n");
				rc = -1;
				continue;
			} else {
				flag |= CMD_FLAG_BOOTD;
			}
		}
#endif	/* CFG_CMD_BOOTD */

		/* OK - call function to do the command */
		if ((cmdtp->cmd)(cmdtp, flag, argc, argv) != 0) {
			rc = -1;
		}

		repeatable &= cmdtp->repeatable;

		/* Did the user stop this? */
		if (had_ctrlc())
			return 0; /* if stopped then not repeatable */
	}

	return rc ? rc : repeatable;
}
