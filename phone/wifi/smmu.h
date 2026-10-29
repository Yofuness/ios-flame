#ifndef SMMU_H
#define SMMU_H
#include <stdint.h>

/* stream SID/MASK through a translation-off bank (or one already set up like that); NAME for the log. 0 = ok. */
int smmu_identity(const char *name, uint32_t sid, uint32_t mask);

extern int smmu_dry;	/* 1: only report what would be written */

#endif
