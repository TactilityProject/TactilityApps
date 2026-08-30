#include <stdio.h>
#include <stdlib.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

// Frees the frame buffer allocated in doomgeneric_Create(). Called from
// DoomEsp.cpp after the game loop stops, so a second run reallocates fresh
// rather than leaking DOOMGENERIC_RESX*RESY*4 bytes per run.
void doomgeneric_Shutdown(void)
{
    if (DG_ScreenBuffer != NULL)
    {
        free(DG_ScreenBuffer);
        DG_ScreenBuffer = NULL;
    }
}

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);

	DG_Init();

	D_DoomMain ();
}

