#ifndef IZ_SAVE_BACKUP_H
#define IZ_SAVE_BACKUP_H

#include <stdint.h>

enum { IZ_SAVE_BACKUP_LABEL_SIZE = 32 };
typedef enum { IZ_SAVE_BACKUP_DONE, IZ_SAVE_BACKUP_EMPTY, IZ_SAVE_BACKUP_ERROR } IzSaveBackupResult;

IzSaveBackupResult iz_backup_saves(const char *self, uint64_t title, char label[IZ_SAVE_BACKUP_LABEL_SIZE]);

#endif
