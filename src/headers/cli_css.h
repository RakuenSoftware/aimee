/* cli_css.h: `aimee css` command — CSS migration assistant signal queries. */
#ifndef DEC_CLI_CSS_H
#define DEC_CLI_CSS_H 1

/* argv = {<op>, <project>, ...}; returns a process exit code. */
int handle_css(int argc, char **argv, int json_output);

#endif /* DEC_CLI_CSS_H */
