#ifndef SCHEDULER_LOG_H
#define SCHEDULER_LOG_H

#include <stdio.h>

// Global file pointer - shared across DL and UL loggers
extern FILE *DL_scheduler_csv;
extern FILE *UL_scheduler_csv;

// Init
void scheduler_log_init(void);

// Close
void scheduler_log_close(void);

#endif // !SCHEDULER_LOG_H
