#ifndef MCP_H
#define MCP_H
#include "../vendor/cJSON.h"

typedef struct {
	const char *name;
	const char *description;
	const char *schema;   /* JSON Schema text for the input */
	/* Returns malloc'd result text; sets *is_error. Runs on a server thread. */
	char *(*run)(const cJSON *args, int *is_error);
} tool_t;

#define MCP_PREFIX "mcp__diotima__"

/* Channels: one per concurrent claude child (its own URL, tool set and
 * admission table). 0 = the learner-facing agent, 1 = graph extraction. */
#define MCP_CHANNELS 5
#define MCP_CHAN_AGENT 0
#define MCP_CHAN_GRAPH 1
#define MCP_CHAN_JOB0 2   /* 2..4: one per concurrent background job */

int mcp_start(void);                                    /* loopback server on a random port; 0 ok */
const char *mcp_config(int chan);                       /* JSON for claude --mcp-config */
void mcp_set_tools(int chan, const tool_t *tools, int n);
void mcp_set_ctx(int chan, void *ctx);   /* handed to that channel's handlers via mcp_call_ctx() */
void *mcp_call_ctx(void);                /* inside a handler: its channel's ctx */
/* Inside a handler: attach a PNG to the result as image content (the model sees it). */
void mcp_attach_image(const char *png_path);
char *mcp_base64(const unsigned char *p, size_t n);   /* malloc'd */

/* Admission: the stream's tool_use (id, prefixed name, input) must be noted
 * before a matching tools/call is executed, and each id runs at most once. */
void mcp_note_tool_use(int chan, const char *id, const char *name, const cJSON *input);
#endif
