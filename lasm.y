/* Hi */

%{
#include <stddef.h>
#include "lasm.h"
arg_t  *arg_mk(char *arg);
stmt_t *stmt_mk(char *opcode, arg_t *args, attr_t *attrs);
attr_t *attr_label_mk(char *label);
%}

%union {
	stmt_t *stmt;
	arg_t  *arg;
	attr_t *attr;
	char   *str;
}

%left  COLON
%token <str> COMMA
%token <str> ARROW
%token <str> NUM
%token <str> IDENT
%token STMT_END

%type <stmt>      statements /* a list of statements */
%type <stmt>      statement  /* an operation with attrs */
%type <attr>      attr_list  /* a list of attributes */
%type <attr>      attr       /* currently, a synonym for label */
%type <attr>      label      /* presently the only attribute */
%type <arg>       args       /* a list of arguments */
%type <str>       arg

%start statements

%%

statements : /* empty */
	   { $$ = NULL; }
           | statements statement
	   {
		$2->l.prev = $1;
		$$ = $2;
           }

statement : attr_list IDENT args STMT_END
	  {
		$$ = stmt_mk($2, $3, $1);
	  }

attr_list : /* empty */
	  { $$ = NULL; }
	  | attr_list STMT_END /* eat STMT_ENDs in the attr_list */
	  { $$ = $1; }
	  | attr_list attr
	  {
		$2->l.prev = $1;
		$$ = $2;
	  }

attr : label
     { $$ = $1; }

label : IDENT COLON
      { $$ = attr_label_mk($1); }

args : /* empty */
     { $$ = NULL; }
     | args arg
     {
	$$ = arg_mk($2);
	$$->l.prev = $1;
     }

arg : IDENT
    | ARROW
    | NUM
    | COMMA

%%
