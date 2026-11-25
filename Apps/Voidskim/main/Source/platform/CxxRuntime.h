#pragma once

/**
 * Runs the destructors of function-local statics (registered through __cxa_atexit), e.g. Jet's
 * scratch buffers. Call once, when the game is done with them.
 */
void runStaticDestructors();
