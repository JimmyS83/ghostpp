// iob_func_compat.cpp - fix for gmp.lib compiled with VS2008
#include <stdio.h>

FILE _iob[] = { *stdin, *stdout, *stderr };

extern "C" FILE* __cdecl __iob_func(void)
{
    return _iob;
}