/*
 * main_sdl.c — SDL2 entry point.
 *
 * The game's original entry point is void main(int, UBYTE*[]) in
 * engine/game/PERSO.C; it was renamed lba_main (logged edit) because
 * SDL2main owns main()/WinMain() on MinGW.
 */
#include <SDL.h>
#include <stdio.h>
#include "port.h"

void lba_main(int argc, UBYTE *argv[]);

int main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IONBF, 0); /* engine printf()s are our trace */
	setvbuf(stderr, NULL, _IONBF, 0);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	lba_main(argc, (UBYTE **)argv);

	SDL_Quit();
	return 0;
}
