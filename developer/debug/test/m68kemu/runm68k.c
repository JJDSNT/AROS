/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    RunM68K — run an m68k binary via m68kemu.library
    Usage: RunM68K [RIGEL] [DISPLAY] <filename> [args]

    RIGEL    run with the Rigel chipset (needs a Rigel-enabled library)
    DISPLAY  RIGEL plus a window showing the chipset's frames
*/
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/m68kemu.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct Library *M68KEmuBase;

int main(int argc, char **argv)
{
    ULONG flags = 0;
    int file = 1;

    /* Leading switches; everything after the file name goes to the program */
    for (; file < argc; file++) {
        if (strcasecmp(argv[file], "RIGEL") == 0)
            flags |= M68KEMU_RUN_RIGEL;
        else if (strcasecmp(argv[file], "DISPLAY") == 0)
            flags |= M68KEMU_RUN_DISPLAY;
        else
            break;
    }

    if (file >= argc) {
        printf("Usage: RunM68K [RIGEL] [DISPLAY] <m68k-binary> [args]\n");
        return 20;
    }

    M68KEmuBase = OpenLibrary("m68kemu.library", 0);
    if (!M68KEmuBase) {
        printf("Cannot open m68kemu.library\n");
        return 20;
    }

    /* Build argument string */
    char argbuf[512] = "";
    size_t len = 0;
    for (int i = file + 1; i < argc; i++) {
        if (i > file + 1 && len + 1 < sizeof(argbuf)) {
            argbuf[len++] = ' ';
            argbuf[len] = '\0';
        }
        size_t arglen = strnlen(argv[i], sizeof(argbuf));
        if (len + arglen < sizeof(argbuf) - 2) {
            memcpy(argbuf + len, argv[i], arglen);
            len += arglen;
            argbuf[len] = '\0';
        }
    }
    if (len + 1 < sizeof(argbuf)) {
        argbuf[len++] = '\n';
        argbuf[len] = '\0';
    }

    LONG result = flags
        ? RunFileEx(argv[file], 0, argbuf, (ULONG)len, flags)
        : RunFile(argv[file], 0, argbuf, (ULONG)len);

    if (flags && result == -1)
        printf("RunM68K: could not run %s (is Rigel built into m68kemu.library?)\n",
               argv[file]);

    CloseLibrary(M68KEmuBase);
    return result;
}
