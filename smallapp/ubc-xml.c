// Copyright (c) 2021 Secure64 Software
// All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Secure64 Software nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//	Common routines and drivers for the ubc-xml utilities

#include "ubc-xml.h"

void
parse_error(struct parser_data *pdp, char const *fmt, ...)
{
	va_list ap;

	fprintf(pdp->err, "Error at line " XML_INT("") ": ",
	                  XML_GetCurrentLineNumber(pdp->parser));
	va_start(ap, fmt);
	vfprintf(pdp->err, fmt, ap);
	va_end(ap);
	fprintf(pdp->err, "\n");
}

#define	BSIZE	BUFSIZ

// int
// setCommandInfo(struct parser_data *pdp,
//                const XML_Char **)
//
//	Record the command name and optional argument(s) in the parser data
//	structure. All string arguments are temporary and must be copied

static int
setCommandInfo(struct parser_data *pdp,
               const XML_Char **attrs)
{
	const XML_Char *key, *val;

	if ((key = *attrs++) == NULL || strcmp(key, "command") != 0) {
		parse_error(pdp, "First attribute must be command");
		return (1);
	}
	if ((val = *attrs++) == NULL) {
		parse_error(pdp, "Missing command name");
		return (1);
	}
	if ((pdp->command = strdup(val)) == NULL) {
		parse_error(pdp, "Cannot set command name" XML_STR(""), val);
		return (1);
	}

	const XML_Char **args = attrs;
	uint32_t argc = 0;

	while ((key = *args++) != NULL) {
		if (*args++ == NULL) {
			parse_error(pdp, "Missing value for " XML_STR(""), key);
			return (1);
		}

		argc++;
	}

	pdp->argc = argc;

	return (0);
}

void
clrCommandInfo(struct parser_data *pdp)
{
	const XML_Char **args, *cp;

	if ((cp = pdp->command) != NULL) {
		pdp->command = NULL;
		free((void *) cp);
	}
	if ((args = pdp->argv) != NULL) {
		pdp->argv = NULL;
		pdp->argc = 0;

		while ((cp = *args++) != NULL) {
			free((void *) cp);
		}

		free(pdp->argv);
	}
}

uint32_t
string_hash(const XML_Char *name)
{
	cmdHashType hash = HASH_INIT;
	XML_Char c;

	while ((c = *name++) != 0) {
		hash = hashFunc(hash, c);
	}

	return (hashFold(hash));
}

XML_Char *
textCopy(const XML_Char *text, int len)
{
	XML_Char *cp;

	if ((cp = malloc((len + 1) * sizeof(XML_Char))) != NULL) {
		memcpy(cp, text, len);
		cp[len] = '\0';
	}

	return (cp);
}

#define	STACK_DEPTH  8

static uint32_t objStack[STACK_DEPTH];
static uint32_t objIndex = 0;

static void XMLCALL
catchStart(void *udata, const XML_Char *name, const XML_Char **attrs)
{
	struct parser_data *pdp = udata;

	// Monitor nesting of objects, and don't allow recursion - it is
	// almost certainly a syntax error

	if (objIndex >= STACK_DEPTH) {
		parse_error(pdp, "stack too deep for " XML_STR(""), name);
		exit(1);
	}

	uint32_t h = string_hash(name);
	uint32_t ndx = objIndex;

	while (ndx-- != 0) {
		if (h == objStack[ndx]) {
			parse_error(pdp, "nested " XML_STR(""), name);
			exit(1);
		}
	}

	objStack[objIndex++] = h;

	if (strcmp(name, "Command") == 0) {
		if (setCommandInfo(udata, attrs) != 0) {
			exit(1);
		}
	}

	xmlStart(udata, name, attrs);
}

static void XMLCALL
catchEnd(void *udata, const XML_Char *name)
{
	struct parser_data *pdp = udata;

	// Make sure we are popping the same token

	if (objIndex == 0) {
		parse_error(pdp, "stack empty " XML_STR(""), name);
		exit(1);
	}

	uint32_t h = string_hash(name);

	if (objStack[--objIndex] != h) {
		parse_error(pdp, "nested " XML_STR(""), name);
		exit(1);
	}

	xmlEnd(udata, name);
}

static void
catchText(void *udata, const XML_Char *info, int len)
{
	XML_Char c;

	// Skip leading whitespace and blank lines. If we encounter an '@',
	// strip it and quit the loop

	while (len > 0 &&
	        ((c = *info) == ' ' || c == '\t' || c == '\n' || c == '@')) {
		info++;
		len--;

		if (c == '@') {
			break;
		}
	}
	if (c == '@' || len > 0) {
		xmlText(udata, info, len);
	}
}

static void
parseFile(XML_Parser p, FILE *fp, struct parser_data *pdp)
{
	char buffer[BSIZE];
	int done;

	pdp->parser = p;

	XML_SetElementHandler(p, catchStart, catchEnd);
	XML_SetCommentHandler(p, xmlComment);
	XML_SetCharacterDataHandler(p, catchText);
	XML_SetUserData(p, pdp);

	do {
		size_t len;

		if ((len = fread(buffer, 1, BSIZE, fp)) > 0) {
			done = 0;
		} else if (feof(fp)) {
			done = 1;
		} else {
			parse_error(pdp->err, "File read error");
			break;
		}
		if (XML_Parse(p, buffer, (int) len, done) == XML_STATUS_ERROR) {
			parse_error(pdp, "parse error " XML_STR(""),
			                  XML_ErrorString(XML_GetErrorCode(p)));
			break;
		}
	} while (!done);
}

static void
process(char *file)
{
	struct parser_data pdata = { stdout, stderr };

	XML_Parser parser = XML_ParserCreate(NULL);

	if (parser == NULL) {
		printf("No parser!\n");
	} else {
		FILE *fp = (file == NULL) ? stdin : fopen(file, "r");

		if (fp != NULL) {
			parseFile(parser, fp, &pdata);

			if (file != NULL) {
				fclose(fp);
			}
		} else {
			printf("Cannot open %s for read: %m\n", file);
		}

		XML_ParserFree(parser);
	}
}

int
main(int argc, char *argv[])
{
	if (argc == 1) {
		process(NULL);
	} else {
		for (argc--, argv++; argc != 0; argc--, argv++) {
			process(argv[0]);
		}
	}

	return (0);
}
