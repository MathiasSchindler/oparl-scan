#ifndef OPARL_SEARCH_MCP_H
#define OPARL_SEARCH_MCP_H

#include "search.h"

int search_mcp(const SearchIndex *index, SearchWorkspace *workspace,
               const char *body, size_t length, const char *protocol,
               const char *method, const char *name, ToolByteBuffer *reply, int *http_status);

#endif
