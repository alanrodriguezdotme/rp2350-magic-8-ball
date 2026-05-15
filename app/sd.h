#pragma once

#include <stdbool.h>

// Initializes the no-OS-FatFS driver and mounts the first SD card defined
// in hw_config.c at logical drive "0:". Returns true on success.
bool sd_card_mount(void);

// Walks the root directory and prints each entry to stdout. Useful as a
// "is the card actually working" smoke test. Returns true if the directory
// could be opened (even if empty).
bool sd_card_list_root(void);
