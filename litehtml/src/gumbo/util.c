// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jdtang@google.com (Jonathan Tang)

#include "util.h"

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>

#include "gumbo.h"
#include "parser.h"

/* Custom longjmp to avoid GCC __builtin_longjmp interception */
extern void gumbo_longjmp(jmp_buf env, int val);

/* Defined in parser.c */
extern jmp_buf gumbo_oom_jmpbuf;

// TODO(jdtang): This should be elsewhere, but there's no .c file for
// SourcePositions and yet the constant needs some linkage, so this is as good
// as any.
const GumboSourcePosition kGumboEmptySourcePosition = {0, 0, 0};

void* gumbo_parser_allocate(GumboParser* parser, size_t num_bytes) {
  void* ptr = malloc(num_bytes);
  if (ptr == NULL) {
    // OOM - abort the entire parse via longjmp
    gumbo_longjmp(gumbo_oom_jmpbuf, 1);
  }
  return ptr;
}

void gumbo_parser_deallocate(GumboParser* parser, void* ptr) {
  (void) parser;
  free(ptr);
}

char* gumbo_copy_stringz(GumboParser* parser, const char* str) {
  char* buffer = gumbo_parser_allocate(parser, strlen(str) + 1);
  strcpy(buffer, str);
  return buffer;
}

// Debug function to trace operation of the parser.  Pass --copts=-DGUMBO_DEBUG
// to use.
void gumbo_debug(const char* format, ...) {
  (void) format;
}
