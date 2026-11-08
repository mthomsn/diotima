#ifndef AGENT_H
#define AGENT_H
typedef enum { AGENT_PROBER, AGENT_TEACHER, AGENT_PRACTICE, AGENT_EXTRACTOR, AGENT_AUDITOR,
	AGENT_RESEARCHER, AGENT_PDF_READER, AGENT_DEMO_BUILDER, AGENT_VISUAL_MAKER, AGENT_VISUAL_ANNOTATOR } agent_kind_t;

/* Compose the system prompt (base -> kind dir -> shared dir, joined by ---; the
 * graph stages get their own dir only) into a
 * file under the data root; returns its path (static buffer) or NULL. */
const char *agent_prompt_file(agent_kind_t k);
const char *agent_name(agent_kind_t k);
#endif
