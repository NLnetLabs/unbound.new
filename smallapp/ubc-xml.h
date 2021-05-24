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
//	Common declarations and prototypes for the ubc-xml routines

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <expat.h>

#ifdef XML_LARGE_SIZE
#  define XML_FMT_INT_MOD "ll"
#else
#  define XML_FMT_INT_MOD "l"
#endif

#ifdef XML_UNICODE_WCHAR_T
#  include <wchar.h>
#  define XML_FMT_STR "ls"
#else
#  define XML_FMT_STR "s"
#endif

#define	XML_INT(f)  "%" f XML_FMT_INT_MOD "u"
#define	XML_STR(f)  "%" f XML_FMT_STR

struct parser_data {
	FILE            *out;      // Where to write normal output
	FILE            *err;      // Where to write error  output
	XML_Parser       parser;
	const XML_Char  *command;
	const XML_Char **argv;
	uint32_t         argc;
	uint32_t         flags;
};

extern void XMLCALL xmlStart(void *, const XML_Char *, const XML_Char **);
extern void XMLCALL xmlEnd(void *, const XML_Char *);
extern void xmlComment(void *, const XML_Char *);
extern void xmlText(void *, const XML_Char *, int);
extern void clrCommandInfo(struct parser_data *pdp);

#define	HASH64   // Use folded 64-bit hash by default

#if	defined(HASH64)
#define	HASH_INIT  0xcbf29ce484222325ULL
#define HASH_MULT  0x00000100000001B3ULL

typedef uint64_t cmdHashType;
#else // !defined(HASH64)
#define	HASH_INIT  0x811C9DC5U
#define HASH_MULT  0x01000193U

typedef uint32_t cmdHashType;
#endif

static inline cmdHashType
hashFunc(cmdHashType h, XML_Char c)
{
	return ((h + c) * HASH_MULT);
}

static inline uint32_t
hashFold(cmdHashType h)
{
#if	defined(HASH64)
	return ((uint32_t)(h >> 32) ^ (uint32_t)(h & 0xffffffffULL));
#else // !defined(HASH64)
	return (hash);
#endif // !defined(HASH64)
}

extern uint32_t string_hash(const XML_Char *);
extern XML_Char *textCopy(const XML_Char *text, int len);
extern void parse_error(struct parser_data *, char const *, ...);
