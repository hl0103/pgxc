#ifndef XC_MAINTENANCE_MODE_H
#define XC_MAINTENANCE_MODE_H

#include <unistd.h>
#include "c.h"
#include "postgres.h"
#include "utils/guc.h"

extern bool xc_maintenance_mode;
extern GucContext	currentGucContext;

#endif /* XC_MAINTENANCE_MODE_H */
