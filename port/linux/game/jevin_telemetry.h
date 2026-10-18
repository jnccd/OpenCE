/*
jevin_TELEMETRY.H

The AI teammate's view of the game, read out of the script console.
*/

#ifndef __jevin_TELEMETRY_H
#define __jevin_TELEMETRY_H

#pragma once

/* jevin_objects [radius] [reference], jevin_state: TRUE when the expression
was one of them (hs.c, before the script compiler) */
boolean jevin_console_command(char const *expression);

#endif
