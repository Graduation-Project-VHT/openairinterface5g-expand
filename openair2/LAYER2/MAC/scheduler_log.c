#include "scheduler_log.h"
#include "common/utils/LOG/log.h"
#include <stdio.h>
#include <time.h>

FILE *DL_scheduler_csv;
FILE *UL_scheduler_csv;

void scheduler_log_init(void)
{
  DL_scheduler_csv = fopen("/openairinterface5g/logs/DL_scheduler_log.csv", "w");
  UL_scheduler_csv = fopen("/openairinterface5g/logs/UL_scheduler_log.csv", "w");

  if (UL_scheduler_csv == NULL || DL_scheduler_csv == NULL) {
    LOG_E(MAC, "[SCHED_LOG] ERROR: Cannot open CSV log file. Check folder permissions!\n");
    return;
  }

  const char *header = "timestamp_ms,frame,subframe,rnti,direction,nb_rb,mcs,tbs_bytes,cqi,retx,hol_delay_ms,avg_thr_kbps,mlwdf_score,qos_alpha,cqi_profile";

  fprintf(DL_scheduler_csv, "%s\n", header);
  fprintf(UL_scheduler_csv, "%s\n", header);

  fflush(DL_scheduler_csv);
  fflush(UL_scheduler_csv);
  LOG_I(MAC, "[SCHED_LOG] CSV loggin started, writing to /openairinterface5g/logs/\n");
}

void scheduler_log_close(void)
{
  if (DL_scheduler_csv != NULL && UL_scheduler_csv != NULL) {
    fclose(DL_scheduler_csv);
    fclose(UL_scheduler_csv);
    DL_scheduler_csv = NULL;
    UL_scheduler_csv = NULL;
    LOG_I(MAC, "[SCHED_LOG] CSV logging stopped.\n");
  }
}
