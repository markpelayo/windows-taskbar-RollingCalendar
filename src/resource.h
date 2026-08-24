// resource.h — command IDs.
//
// The menu is rebuilt from scratch every time it opens, so these are the only
// stable identifiers in it. Ranges are reserved generously: a dynamic list that
// outgrows its range would start colliding with the next command, and that is
// the sort of bug that shows up as "clicking a calendar quits the app".

#pragma once

#define IDI_APPICON 101

// ---- fixed commands ----------------------------------------------------
#define IDM_PROJECT_PAGE        1001
#define IDM_DEBUG_TIME          1002
#define IDM_DEBUG_RESET         1003
#define IDM_DEMO_CALENDAR       1004
#define IDM_ADD_CALENDAR        1005
#define IDM_RESTORE_STRIP       1006
#define IDM_RESTORE_ALL         1007
#define IDM_REFRESH_NOW         1008
#define IDM_QUIT                1009
#define IDM_MOVE_WIDGET         1010
#define IDM_RESET_POSITION      1011

#define IDM_KEYWORDS_IMPORT     1020
#define IDM_KEYWORDS_SAMPLE     1021
#define IDM_KEYWORDS_SAVE_CSV   1022
#define IDM_KEYWORDS_CLEAR      1023

#define IDM_SOUNDHOURS_OFF      1030
#define IDM_SOUNDHOURS_ALLDAY   1031
#define IDM_SOUNDHOURS_CUSTOM   1032

#define IDM_ALERTS_OFF          1040
#define IDM_ALERTS_CUSTOM_LEAD  1041
#define IDM_ALERTS_SOUND_OFF    1042
#define IDM_ALERTS_SOUND_IMPORT 1043
#define IDM_ALERTS_VOICE_OFF    1044
#define IDM_ALERTS_VOICE_MANAGE 1045
#define IDM_ALERTS_CATS_ALL     1046
#define IDM_ALERTS_TEST         1047

#define IDM_CHIME_OFF           1050
#define IDM_CHIME_HOURLY        1051
#define IDM_CHIME_QUARTERLY     1052
#define IDM_CHIME_STRIKE_HOUR   1053
#define IDM_CHIME_VOL_CUSTOM    1054
#define IDM_CHIME_HEAR_PAST     1055
#define IDM_CHIME_HEAR_HALF     1056
#define IDM_CHIME_HEAR_TO       1057
#define IDM_CHIME_HEAR_HOUR     1058
#define IDM_CHIME_STOP          1059

#define IDM_STARTUP_OFF         1060
#define IDM_STARTUP_ON          1061

#define IDM_LABEL_NOW_NAME      1070
#define IDM_LABEL_NOW_LEFT      1071
#define IDM_LABEL_NEXT_NAME     1072
#define IDM_LABEL_NEXT_DUR      1073

// ---- dynamic ranges ----------------------------------------------------
#define IDM_RANGE_FIRST         2000
#define IDM_TIMERANGE_BASE      2000   // + index into the six presets
#define IDM_WIDTH_BASE          2100   // + index, 100..450 step 50
#define IDM_LABELWIDTH_BASE     2200   // + index into the seven presets
#define IDM_PROFILE_BASE        2300   // + index; activate
#define IDM_PROFILE_RENAME_BASE 2400
#define IDM_PROFILE_REMOVE_BASE 2500
#define IDM_SOUNDWINDOW_BASE    2600   // + index; toggle
#define IDM_SOUNDWINDOW_DEL_BASE 2700
#define IDM_LEAD_BASE           2800   // + index into the lead presets
#define IDM_SOUNDNAME_BASE      2900
#define IDM_VOICE_BASE          3000
#define IDM_CATEGORY_BASE       3100
#define IDM_CHIMEVOL_BASE       3200
#define IDM_CHIMEVOL_DEL_BASE   3300
#define IDM_STARTUP_DELAY_BASE  3400   // + index into 5/10/15/20/30/60
#define IDM_DAYROW_BASE         3500   // + row index; inert, listed only
#define IDM_RANGE_LAST          4000
