/*
 * Tactility replacements for neatvi's cmd.c and lsp.c: both start other processes (fork/exec),
 * which Tactility doesn't have. Shell commands (:!, filters, pipes) and LSP support report failure.
 */
#include <stddef.h>
#include "vi.h"

char *cmd_pipe(char *cmd, char *ibuf, int oproc)
{
	return NULL;
}

char *cmd_unix(char *path, char *ibuf)
{
	return NULL;
}

int cmd_exec(char *cmd)
{
	return 1;
}

int lsp_init(char *cmd[])
{
	return 1;
}

void lsp_done(void)
{
}

int lsp_on(void)
{
	return 0;
}

void lsp_modified(char *path, char *ft)
{
}

int lsp_definition(char *path, int row, int off, char *ft, char *dst, int dstlen, int *drow, int *doff)
{
	return 1;
}

char *lsp_find(char *path, int row, int off, char *ft)
{
	return NULL;
}

char *lsp_hover(char *path, int row, int off, char *ft)
{
	return NULL;
}
