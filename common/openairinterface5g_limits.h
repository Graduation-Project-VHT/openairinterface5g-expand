/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef OPENAIRINTERFACE5G_LIMITS_H_
#define OPENAIRINTERFACE5G_LIMITS_H_

#        define MAX_MOBILES_PER_GNB 16
#        define NUMBER_OF_eNB_MAX 1
#        define NUMBER_OF_gNB_MAX 1
#        define NUMBER_OF_RU_MAX 2
#        define NUMBER_OF_NR_RU_MAX 2
#        define NUMBER_OF_UCI_MAX (MAX_MOBILES_PER_GNB * 4)
#        define NUMBER_OF_ULSCH_MAX (MAX_MOBILES_PER_GNB)
#        define NUMBER_OF_DLSCH_MAX (MAX_MOBILES_PER_GNB)
#        define NUMBER_OF_SRS_MAX (MAX_MOBILES_PER_GNB)
#        define NUMBER_OF_SCH_STATS_MAX (MAX_MOBILES_PER_GNB)


#        ifndef PHYSIM
#            ifndef UE_EXPANSION
#                    define NUMBER_OF_UE_MAX 40
#                    define NUMBER_OF_CONNECTED_eNB_MAX 1
#                    define NUMBER_OF_CONNECTED_gNB_MAX 1
#            else
#                    define NUMBER_OF_UE_MAX 256
#                    define NUMBER_OF_CONNECTED_eNB_MAX 1
#                    define NUMBER_OF_CONNECTED_gNB_MAX 1
#            endif
#        else
#                    define NUMBER_OF_UE_MAX 4
#                    define NUMBER_OF_CONNECTED_eNB_MAX 1
#                    define NUMBER_OF_CONNECTED_gNB_MAX 1
#        endif

#endif /* OPENAIRINTERFACE5G_LIMITS_H_ */
