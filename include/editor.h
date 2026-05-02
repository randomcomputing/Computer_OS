#ifndef EDITOR_H
#define EDITOR_H

// Full-screen text editor, nano-style.
//
// Keybindings:
//   Arrows / Home / End / PgUp / PgDn   navigate
//   Backspace / Delete                  delete
//   Enter                               newline
//   Tab                                 4 spaces
//   Ctrl-S                              save
//   Ctrl-X                              exit (prompts if modified)
//   Ctrl-K                              cut current line
//   Ctrl-U                              paste cut buffer
//
// Pass NULL or "" for `path` to open an empty buffer ("untitled").
// Returns 0 on clean exit, -1 if the file couldn't be loaded.
int editor_run(const char* path);

#endif