// Shim ctype.h — implementations in nyx_doom.c (ASCII only).
#ifndef NYX_SHIM_CTYPE_H
#define NYX_SHIM_CTYPE_H

int isprint(int c);
int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int ispunct(int c);
int tolower(int c);
int toupper(int c);

#endif
