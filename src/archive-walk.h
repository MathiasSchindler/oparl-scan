#ifndef OPARL_ARCHIVE_WALK_H
#define OPARL_ARCHIVE_WALK_H
#include "tool_util.h"
int archive_walk(const char *path, ToolWalkCallback callback, void *user, int depth);
#endif
