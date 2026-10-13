/* A small tool picker in freestanding C: Samantha's hands, distilled. It reads a sentence and names the tool
   she would reach for (or "null" when the sentence is not a command). Hashed words and letter triples feed a
   linear scorer with integer weights trained on the Turing project's training rows; no floats, no libc, no
   network. The Python reference in turing/training/smallpicker/ref.py is the spec, and kernel/picker_golden.h
   holds sentences with its answers so `pickcheck` proves this C agrees. */
#ifndef PICKER_H
#define PICKER_H

/* Tool name for the sentence, never NULL; "null" means not a command. */
const char *picker_pick(const char *text);

/* Runs every golden sentence; returns how many agree with the reference, and sets *total. */
int picker_selfcheck(int *total);

#endif
