#ifndef GAME_INSTALLER_H
#define GAME_INSTALLER_H

#include <tamtypes.h>

typedef void (*game_progress_callback_t)(u32 done, u32 total, void *ctx);

int game_install(const char *iso_path, const char *title,
                 const char *elf_source,
                 game_progress_callback_t progress, void *ctx);

#endif

/* Shared with app_installer */
int write_direct_kelf_verified(const char *source, const char *destination,
                                u64 source_size, const char *expected_hash,
                                u64 *copied_bytes, u64 *work_done,
                                u64 total_work,
                                void *progress, void *progress_context,
                                char actual_hash[65]);
#include "apps/system_version.h"
void run_game_installer(psx_revision_t revision,
                        const system_version_result_t *version);
