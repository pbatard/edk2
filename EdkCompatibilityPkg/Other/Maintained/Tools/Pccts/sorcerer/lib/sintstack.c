/*
 * sint.c
 *
 * SOFTWARE RIGHTS
 *
 * We reserve no LEGAL rights to SORCERER -- SORCERER is in the public
 * domain.  An individual or company may do whatever they wish with
 * source code distributed with SORCERER or the code generated by
 * SORCERER, including the incorporation of SORCERER, or its output, into
 * commerical software.
 *
 * We encourage users to develop software with SORCERER.  However, we do
 * ask that credit is given to us for developing SORCERER.  By "credit",
 * we mean that if you incorporate our source code into one of your
 * programs (commercial product, research project, or otherwise) that you
 * acknowledge this fact somewhere in the documentation, research report,
 * etc...  If you like SORCERER and have developed a nice tool with the
 * output, please mention that you developed it using SORCERER.  In
 * addition, we ask that this header remain intact in our source code.
 * As long as these guidelines are kept, we expect to continue enhancing
 * this system and expect to make other tools available as they are
 * completed.
 *
 * SORCERER 1.00B
 * Terence Parr
 * AHPCRC, University of Minnesota
 * 1992-1994
 */
#include <stdio.h>
#include <setjmp.h>

#ifdef PCCTS_USE_STDARG
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#include "CASTBase.h"
#include "sintstack.h"

SIntStack *
#ifdef __USE_PROTOS
sint_newstack(int size)
#else
sint_newstack(size)
int size;
#endif
{
  SIntStack *p = (SIntStack *) calloc(1, sizeof(SIntStack));
  require(p!=NULL, "sint_newstack: out of memory");
  p->data = (int *) calloc(size, sizeof(int));
  require(p!=NULL, "sint_newstack: out of memory");
  p->size = size;
  p->sp = size;
  return p;
}

void
#ifdef __USE_PROTOS
sint_freestack(SIntStack *st)
#else
sint_freestack(st)
SIntStack *st;
#endif
{
  if ( st==NULL ) return;
  if ( st->data==NULL ) return;
  free(st->data);
  free(st);
}

void
#ifdef __USE_PROTOS
sint_push(SIntStack *st,int i)
#else
sint_push(st,i)
SIntStack *st;
int i;
#endif
{
  require(st->sp>0, "sint_push: stack overflow");
  st->data[--(st->sp)] = i;
}

int
#ifdef __USE_PROTOS
sint_pop(SIntStack *st)
#else
sint_pop(st)
SIntStack *st;
#endif
{
  require(st->sp<st->size, "sint_pop: stack underflow");
  return st->data[st->sp++];
}

int
#ifdef __USE_PROTOS
sint_stacksize(SIntStack *st)
#else
sint_stacksize(st)
SIntStack *st;
#endif
{
  return st->size - st->sp;
}

void
#ifdef __USE_PROTOS
sint_stackreset(SIntStack *st)
#else
sint_stackreset(st)
SIntStack *st;
#endif
{
  st->sp = st->size;
}

int
#ifdef __USE_PROTOS
sint_stackempty(SIntStack *st)
#else
sint_stackempty(st)
SIntStack *st;
#endif
{
  return st->sp==st->size;
}

int
#ifdef __USE_PROTOS
sint_top(SIntStack *st)
#else
sint_top(st)
SIntStack *st;
#endif
{
  require(st->sp<st->size, "sint_top: stack underflow");
  return st->data[st->sp];
}
