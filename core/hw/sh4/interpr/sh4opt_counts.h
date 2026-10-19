/*
	sh4opt execution counts. With SH4OPT_COUNTS=<file> set, the interpreter counts every
	instruction executed from main RAM and every non-sequential control transfer while
	recording, then writes them for `python3 -m sh4opt --counts`. Flycast runs on its
	configured executor (normally the dynarec) and switches to the interpreter only while
	recording, then back.

	Recording is started and stopped with the "Record" button drawn top-left in game.
	The first recording is written to <file>, later ones to <file>-2, <file>-3...

	SH4OPT_FRAMES=<start>:<count>  also record automatically over this vblank window
	SH4OPT_EXIT=1                  exit Flycast once that automatic window is written
*/
#pragma once

// True when SH4OPT_COUNTS is set.
bool sh4opt_counting_requested();
// True while a recording is requested or running: only the interpreter sees every instruction.
bool sh4opt_needs_interpreter();
// True once after the counter stopped the CPU so that the emulator changes executor.
bool sh4opt_take_executor_switch();
// Called once per vblank on the emulation thread: starts and stops recordings.
void sh4opt_vblank();
// GUI: request a start or stop (applied at the next vblank), and show progress.
void sh4opt_toggle_recording();
bool sh4opt_is_recording();
unsigned sh4opt_recorded_vblanks();
